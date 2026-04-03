#include "present_hook.h"
#include "remix_api.h"
#include "remix_renderer.h"
#include "camera.h"
#include "scene_extractor.h"

#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present g_originalPresent = nullptr;

// Game readiness — set by F4SE message handler in main.cpp
extern std::atomic<bool> g_gameDataReady;

// Remix runs on its own thread to avoid deadlocking the game's DX11 render thread
static std::thread g_remixThread;
static std::atomic<bool> g_remixRunning { false };
static std::atomic<bool> g_remixReady { false };
static std::mutex g_cameraMutex;
static CameraState g_sharedCamera = {};
static uint32_t g_gameWidth = 1280;
static uint32_t g_gameHeight = 720;

// Per-cell scene data — main thread produces, remix thread consumes
struct PendingCellScene {
    uint32_t cellFormID;
    ExtractionResult data;
};

static std::mutex g_sceneMutex;
static std::vector<PendingCellScene> g_pendingCellScenes;
static std::vector<uint32_t> g_pendingUnloads;
static std::atomic<bool> g_sceneReady { false };

// Multi-cell extraction state
static std::unordered_set<uint32_t> g_extractedCells;       // cells already sent to remix thread
static std::vector<uint32_t> g_refreshQueue;                // round-robin refresh order
static size_t g_refreshIndex = 0;
static std::unordered_map<uint32_t, uintptr_t> g_cellPtrMap; // formID -> cellPtr for refresh

// Extraction timing
static int g_extractionAttempts = 0;
static int g_nextExtractFrame = 0;
static bool g_firstExtractionDone = false;
static constexpr int kInitialRetryDelay = 120;     // ~2 seconds at 60fps
static constexpr int kMaxRetryDelay = 600;         // ~10 seconds
static constexpr int kMaxInitialAttempts = 15;
static constexpr uint32_t kMinExpectedMeshes = 5;
static constexpr int kNewCellCheckInterval = 60;    // ~1 second at 60fps — check for new cells
static constexpr int kRefreshInterval = 1800;       // ~30 seconds at 60fps — refresh one existing cell

static void PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void RemixThreadFunc() {
    _MESSAGE("FO4RemixPlugin: Remix thread started");

    // Prevent Windows from ghosting this thread's window when Present() blocks
    DisableProcessWindowsGhosting();

    if (!RemixAPI::Initialize(nullptr, g_gameWidth, g_gameHeight)) {
        _MESSAGE("FO4RemixPlugin: ERROR - Remix API init failed on remix thread");
        return;
    }
    _MESSAGE("FO4RemixPlugin: Remix API initialized on remix thread");

    if (!RemixRenderer::Init()) {
        _MESSAGE("FO4RemixPlugin: ERROR - Renderer init failed on remix thread");
        return;
    }

    g_remixReady = true;

    while (g_remixRunning) {
        // Check for pending scene data from main thread
        if (g_sceneReady) {
            std::vector<PendingCellScene> cellScenes;
            std::vector<uint32_t> unloads;
            {
                std::lock_guard<std::mutex> lock(g_sceneMutex);
                cellScenes = std::move(g_pendingCellScenes);
                unloads = std::move(g_pendingUnloads);
                g_sceneReady = false;
            }

            // Unload cells that are no longer loaded by the engine
            for (uint32_t cellID : unloads) {
                _MESSAGE("FO4RemixPlugin: Remix thread unloading cell 0x%08X", cellID);
                RemixRenderer::UnloadCell(cellID);
            }

            // Load new/refreshed cell scenes
            for (auto& cell : cellScenes) {
                if (!cell.data.meshes.empty()) {
                    _MESSAGE("FO4RemixPlugin: Remix thread loading cell 0x%08X (%zu meshes, %zu textures, %zu lights)",
                             cell.cellFormID, cell.data.meshes.size(),
                             cell.data.textures.size(), cell.data.lights.size());
                    RemixRenderer::LoadCellScene(cell.cellFormID, std::move(cell.data));
                }
            }
        }

        // Grab latest camera from main thread
        CameraState cam;
        {
            std::lock_guard<std::mutex> lock(g_cameraMutex);
            cam = g_sharedCamera;
        }

        // Pump messages before rendering so input is processed even when frames are slow
        PumpMessages();

        RemixRenderer::OnFrame(cam);

        // Pump again after rendering
        PumpMessages();

        // ~60fps cap to avoid spinning
        Sleep(16);
    }

    _MESSAGE("FO4RemixPlugin: Remix thread shutting down");
    RemixRenderer::Shutdown();
    RemixAPI::Shutdown();
}

static int g_presentCallCount = 0;

static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    g_presentCallCount++;

    // Start remix thread on first Present call
    if (!g_remixRunning && !g_remixThread.joinable()) {
        // Query game swap chain for resolution
        DXGI_SWAP_CHAIN_DESC scDesc = {};
        if (SUCCEEDED(swapChain->GetDesc(&scDesc)) && scDesc.BufferDesc.Width > 0) {
            g_gameWidth = scDesc.BufferDesc.Width;
            g_gameHeight = scDesc.BufferDesc.Height;
            _MESSAGE("FO4RemixPlugin: Game resolution: %ux%u", g_gameWidth, g_gameHeight);
        }
        _MESSAGE("FO4RemixPlugin: hkPresent - launching Remix thread");
        g_remixRunning = true;
        g_remixThread = std::thread(RemixThreadFunc);
    }

    // Update shared camera state for the remix thread
    if (g_remixReady) {
        CameraState cam = Camera::Get();
        std::lock_guard<std::mutex> lock(g_cameraMutex);
        g_sharedCamera = cam;
    }

    // Multi-cell extraction: initial bootstrap, then incremental new-cell + round-robin refresh
    if (g_remixReady && g_gameDataReady && g_presentCallCount >= g_nextExtractFrame) {
        if (!g_firstExtractionDone) {
            // Initial extraction: retry with backoff until we get enough meshes from any cell
            if (g_extractionAttempts < kMaxInitialAttempts && SceneExtractor::IsPlayerCellReady()) {
                g_extractionAttempts++;
                _MESSAGE("FO4RemixPlugin: Initial multi-cell extraction attempt %d/%d (frame %d)...",
                         g_extractionAttempts, kMaxInitialAttempts, g_presentCallCount);

                auto loadedCells = SceneExtractor::GetLoadedCells();

                ID3D11Device* device = nullptr;
                swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&device);

                std::vector<PendingCellScene> newScenes;
                size_t totalMeshes = 0;

                for (auto& cellInfo : loadedCells) {
                    auto extraction = SceneExtractor::ExtractCell(cellInfo.cellPtr, device);
                    totalMeshes += extraction.meshes.size();
                    if (!extraction.meshes.empty()) {
                        g_extractedCells.insert(cellInfo.formID);
                        g_refreshQueue.push_back(cellInfo.formID);
                        g_cellPtrMap[cellInfo.formID] = cellInfo.cellPtr;
                        newScenes.push_back({ cellInfo.formID, std::move(extraction) });
                    }
                }

                if (device) device->Release();

                if (totalMeshes >= kMinExpectedMeshes) {
                    _MESSAGE("FO4RemixPlugin: Initial extraction succeeded: %zu cells, %zu total meshes",
                             newScenes.size(), totalMeshes);
                    g_firstExtractionDone = true;
                    g_nextExtractFrame = g_presentCallCount + kNewCellCheckInterval;
                    g_refreshIndex = 0;
                    std::lock_guard<std::mutex> lock(g_sceneMutex);
                    g_pendingCellScenes = std::move(newScenes);
                    g_sceneReady = true;
                } else {
                    int delay = kInitialRetryDelay << (g_extractionAttempts - 1);
                    if (delay > kMaxRetryDelay) delay = kMaxRetryDelay;
                    g_nextExtractFrame = g_presentCallCount + delay;
                    _MESSAGE("FO4RemixPlugin: Initial extraction got %zu meshes (need >= %u), "
                             "retrying in %d frames (~%.1fs)",
                             totalMeshes, kMinExpectedMeshes, delay, delay / 60.0f);
                }
            } else if (g_extractionAttempts >= kMaxInitialAttempts) {
                _MESSAGE("FO4RemixPlugin: WARNING - Gave up initial extraction after %d attempts",
                         kMaxInitialAttempts);
                g_firstExtractionDone = true;
                g_nextExtractFrame = g_presentCallCount + kNewCellCheckInterval;
            }
        } else {
            // Steady-state: check for new/removed cells, and refresh one existing cell
            auto loadedCells = SceneExtractor::GetLoadedCells();

            // Build set of currently loaded cell form IDs
            std::unordered_set<uint32_t> currentlyLoaded;
            std::unordered_map<uint32_t, uintptr_t> currentPtrMap;
            for (auto& cellInfo : loadedCells) {
                currentlyLoaded.insert(cellInfo.formID);
                currentPtrMap[cellInfo.formID] = cellInfo.cellPtr;
            }

            // Detect cells to unload (were extracted but no longer loaded by engine)
            std::vector<uint32_t> toUnload;
            for (uint32_t cellID : g_extractedCells) {
                if (currentlyLoaded.find(cellID) == currentlyLoaded.end()) {
                    toUnload.push_back(cellID);
                }
            }

            // Detect new cells to extract
            std::vector<CellInfo> toExtract;
            for (auto& cellInfo : loadedCells) {
                if (g_extractedCells.find(cellInfo.formID) == g_extractedCells.end()) {
                    toExtract.push_back(cellInfo);
                }
            }

            // Pick one existing cell to refresh (round-robin)
            CellInfo refreshCell = { 0, 0 };
            static int g_refreshFrameCounter = 0;
            g_refreshFrameCounter += kNewCellCheckInterval;
            if (g_refreshFrameCounter >= kRefreshInterval && !g_refreshQueue.empty()) {
                g_refreshFrameCounter = 0;
                // Find next valid cell in refresh queue
                for (size_t attempts = 0; attempts < g_refreshQueue.size(); attempts++) {
                    if (g_refreshIndex >= g_refreshQueue.size()) g_refreshIndex = 0;
                    uint32_t candidateID = g_refreshQueue[g_refreshIndex];
                    g_refreshIndex++;
                    auto it = currentPtrMap.find(candidateID);
                    if (it != currentPtrMap.end()) {
                        refreshCell.formID = candidateID;
                        refreshCell.cellPtr = it->second;
                        break;
                    }
                }
            }

            // Perform extractions
            std::vector<PendingCellScene> newScenes;
            bool needsUpdate = false;

            if (!toExtract.empty() || refreshCell.cellPtr != 0) {
                ID3D11Device* device = nullptr;
                swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&device);

                // Extract new cells
                for (auto& cellInfo : toExtract) {
                    _MESSAGE("FO4RemixPlugin: Extracting new cell 0x%08X", cellInfo.formID);
                    auto extraction = SceneExtractor::ExtractCell(cellInfo.cellPtr, device);
                    if (!extraction.meshes.empty()) {
                        g_extractedCells.insert(cellInfo.formID);
                        g_refreshQueue.push_back(cellInfo.formID);
                        g_cellPtrMap[cellInfo.formID] = cellInfo.cellPtr;
                        newScenes.push_back({ cellInfo.formID, std::move(extraction) });
                    }
                }

                // Refresh one existing cell
                if (refreshCell.cellPtr != 0) {
                    _MESSAGE("FO4RemixPlugin: Refreshing cell 0x%08X", refreshCell.formID);
                    auto extraction = SceneExtractor::ExtractCell(refreshCell.cellPtr, device);
                    if (!extraction.meshes.empty()) {
                        newScenes.push_back({ refreshCell.formID, std::move(extraction) });
                    }
                }

                if (device) device->Release();
            }

            // Process unloads
            if (!toUnload.empty()) {
                for (uint32_t cellID : toUnload) {
                    _MESSAGE("FO4RemixPlugin: Cell 0x%08X no longer loaded, scheduling unload", cellID);
                    g_extractedCells.erase(cellID);
                    g_cellPtrMap.erase(cellID);
                    // Remove from refresh queue
                    g_refreshQueue.erase(
                        std::remove(g_refreshQueue.begin(), g_refreshQueue.end(), cellID),
                        g_refreshQueue.end());
                }
                if (g_refreshIndex >= g_refreshQueue.size()) g_refreshIndex = 0;
                needsUpdate = true;
            }

            if (!newScenes.empty() || needsUpdate) {
                SceneExtractor::ClearTextureCache();
                std::lock_guard<std::mutex> lock(g_sceneMutex);
                // Append to any pending scenes not yet consumed
                for (auto& scene : newScenes) {
                    g_pendingCellScenes.push_back(std::move(scene));
                }
                for (uint32_t cellID : toUnload) {
                    g_pendingUnloads.push_back(cellID);
                }
                g_sceneReady = true;
            }

            g_nextExtractFrame = g_presentCallCount + kNewCellCheckInterval;
        }
    }

    return g_originalPresent(swapChain, syncInterval, flags);
}

bool PresentHook::Install() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FO4RemixDummy";
    RegisterClassExW(&wc);

    HWND dummyHwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED,
                                      0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummyHwnd) {
        _MESSAGE("FO4RemixPlugin: ERROR - Failed to create dummy window");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 1;
    sd.BufferDesc.Height = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummyHwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    ID3D11Device* dummyDevice = nullptr;
    IDXGISwapChain* dummySwapChain = nullptr;
    ID3D11DeviceContext* dummyContext = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &dummySwapChain, &dummyDevice, nullptr, &dummyContext);

    if (FAILED(hr)) {
        _MESSAGE("FO4RemixPlugin: ERROR - D3D11CreateDeviceAndSwapChain failed (0x%08X)", hr);
        DestroyWindow(dummyHwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(dummySwapChain);
    void* presentAddr = vtable[8];

    dummyContext->Release();
    dummySwapChain->Release();
    dummyDevice->Release();
    DestroyWindow(dummyHwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (MH_Initialize() != MH_OK) {
        _MESSAGE("FO4RemixPlugin: ERROR - MH_Initialize failed");
        return false;
    }

    if (MH_CreateHook(presentAddr, &hkPresent,
                       reinterpret_cast<void**>(&g_originalPresent)) != MH_OK) {
        _MESSAGE("FO4RemixPlugin: ERROR - MH_CreateHook failed");
        MH_Uninitialize();
        return false;
    }

    if (MH_EnableHook(presentAddr) != MH_OK) {
        _MESSAGE("FO4RemixPlugin: ERROR - MH_EnableHook failed");
        MH_Uninitialize();
        return false;
    }

    _MESSAGE("FO4RemixPlugin: Present hooked at %p", presentAddr);
    return true;
}

void PresentHook::Uninstall() {
    g_remixRunning = false;
    if (g_remixThread.joinable()) {
        g_remixThread.join();
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}
