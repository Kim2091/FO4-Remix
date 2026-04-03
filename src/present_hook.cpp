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

// Scene extraction state — main thread produces, remix thread consumes
static std::mutex g_sceneMutex;
static ExtractionResult g_pendingScene;
static std::atomic<bool> g_sceneReady { false };
static std::atomic<bool> g_sceneExtracted { false };

// Retry state for scene extraction
static int g_extractionAttempts = 0;
static int g_nextRetryFrame = 0;
static constexpr int kInitialRetryDelay = 120;     // ~2 seconds at 60fps
static constexpr int kMaxRetryDelay = 600;         // ~10 seconds
static constexpr int kMaxExtractionAttempts = 15;
static constexpr uint32_t kMinExpectedMeshes = 5;

static void RemixThreadFunc() {
    _MESSAGE("FO4RemixPlugin: Remix thread started");

    if (!RemixAPI::Initialize(nullptr)) {
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
            ExtractionResult scene;
            {
                std::lock_guard<std::mutex> lock(g_sceneMutex);
                scene = std::move(g_pendingScene);
                g_sceneReady = false;
            }
            if (!scene.meshes.empty()) {
                _MESSAGE("FO4RemixPlugin: Remix thread loading %zu meshes, %zu textures",
                         scene.meshes.size(), scene.textures.size());
                RemixRenderer::LoadScene(std::move(scene));
            }
        }

        // Grab latest camera from main thread
        CameraState cam;
        {
            std::lock_guard<std::mutex> lock(g_cameraMutex);
            cam = g_sharedCamera;
        }

        RemixRenderer::OnFrame(cam);

        // Pump window messages for the Remix window
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

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
        if (g_presentCallCount == 1) {
            _MESSAGE("FO4RemixPlugin: hkPresent - launching Remix thread");
        }
        g_remixRunning = true;
        g_remixThread = std::thread(RemixThreadFunc);
    }

    // Update shared camera state for the remix thread
    if (g_remixReady) {
        CameraState cam = Camera::Get();
        std::lock_guard<std::mutex> lock(g_cameraMutex);
        g_sharedCamera = cam;
    }

    // Extract scene geometry once the remix thread, F4SE, and game state are all ready
    if (g_remixReady && !g_sceneExtracted && g_gameDataReady) {
        bool canAttempt = g_extractionAttempts < kMaxExtractionAttempts
                       && g_presentCallCount >= g_nextRetryFrame;

        if (canAttempt && SceneExtractor::IsPlayerCellReady()) {
            g_extractionAttempts++;
            _MESSAGE("FO4RemixPlugin: Scene extraction attempt %d/%d (frame %d)...",
                     g_extractionAttempts, kMaxExtractionAttempts, g_presentCallCount);

            ID3D11Device* device = nullptr;
            swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&device);

            auto extraction = SceneExtractor::ExtractPlayerCell(device);

            if (device) device->Release();

            size_t meshCount = extraction.meshes.size();
            if (meshCount >= kMinExpectedMeshes) {
                _MESSAGE("FO4RemixPlugin: Scene extraction succeeded with %zu meshes", meshCount);
                g_sceneExtracted = true;
                std::lock_guard<std::mutex> lock(g_sceneMutex);
                g_pendingScene = std::move(extraction);
                g_sceneReady = true;
            } else {
                // Too few meshes — schedule retry with exponential backoff
                int delay = kInitialRetryDelay << (g_extractionAttempts - 1);
                if (delay > kMaxRetryDelay) delay = kMaxRetryDelay;
                g_nextRetryFrame = g_presentCallCount + delay;
                _MESSAGE("FO4RemixPlugin: Extraction got %zu meshes (need >= %u), "
                         "retrying in %d frames (~%.1fs)",
                         meshCount, kMinExpectedMeshes, delay, delay / 60.0f);
            }
        } else if (g_extractionAttempts >= kMaxExtractionAttempts) {
            g_sceneExtracted = true;
            _MESSAGE("FO4RemixPlugin: WARNING - Gave up scene extraction after %d attempts",
                     kMaxExtractionAttempts);
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
