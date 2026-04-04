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
static constexpr int kReextractDelay = 180;         // ~3 seconds — retry cells with missing textures

// Cells that need re-extraction because textures weren't streamed in yet
static std::unordered_map<uint32_t, int> g_pendingReextract; // formID -> frame to retry

// Backbuffer capture for screen overlay
static ID3D11Texture2D* g_stagingTex = nullptr;
static uint32_t g_stagingWidth = 0;
static uint32_t g_stagingHeight = 0;
static DXGI_FORMAT g_stagingFormat = DXGI_FORMAT_UNKNOWN;

static std::mutex g_overlayMutex;
static std::vector<uint8_t> g_overlayPixels;
static uint32_t g_overlayWidth = 0;
static uint32_t g_overlayHeight = 0;
static DXGI_FORMAT g_overlayDxgiFormat = DXGI_FORMAT_UNKNOWN;
static std::atomic<bool> g_overlayReady { false };

// UI RT detection uses two hooks together:
//   ClearRTV: records R8G8B8A8_UNORM candidates cleared with {0,0,0,0}
//   OMSetRTs: checks if a sole-bound RT was a ClearRTV candidate
// The intersection uniquely identifies the UI RT — GBuffer R8G8B8A8_UNORM
// targets are only MRT-bound (never sole), and the backbuffer is never cleared.
typedef void (STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(
    ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
static PFN_OMSetRenderTargets g_originalOMSetRTs = nullptr;
typedef void (STDMETHODCALLTYPE* PFN_ClearRenderTargetView)(
    ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
static PFN_ClearRenderTargetView g_originalClearRTV = nullptr;
static ID3D11Texture2D* g_uiRenderTarget = nullptr;
static bool g_uiRTLocked = false;
static std::atomic<bool> g_uiClearedThisFrame { false };
static bool g_contextHooked = false;

// Per-frame ClearRTV candidate tracking (R8G8B8A8_UNORM textures cleared this frame)
static constexpr int kMaxClearCandidates = 8;
static ID3D11Texture2D* g_clearCandidates[kMaxClearCandidates] = {};
static int g_clearCandidateCount = 0;
static int g_presentCallCount = 0;

static void PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ClearRTV hook — Phase 1 of detection: records R8G8B8A8_UNORM candidates.
// Once locked, just sets the active flag when the known UI RT is cleared.
static void STDMETHODCALLTYPE hkClearRenderTargetView(
    ID3D11DeviceContext* context,
    ID3D11RenderTargetView* rtv,
    const FLOAT color[4])
{
    if (color[0] == 0.0f && color[1] == 0.0f && color[2] == 0.0f && color[3] == 0.0f) {
        ID3D11Resource* resource = nullptr;
        rtv->GetResource(&resource);
        if (resource) {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) {
                if (g_uiRTLocked) {
                    if (tex == g_uiRenderTarget) {
                        g_uiClearedThisFrame = true;
                    }
                } else {
                    // Detection: add matching textures to per-frame candidate list
                    D3D11_TEXTURE2D_DESC desc;
                    tex->GetDesc(&desc);
                    if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                        desc.Width == g_gameWidth && desc.Height == g_gameHeight &&
                        desc.Usage == D3D11_USAGE_DEFAULT &&
                        g_clearCandidateCount < kMaxClearCandidates) {
                        // Avoid duplicates
                        bool already = false;
                        for (int i = 0; i < g_clearCandidateCount; i++) {
                            if (g_clearCandidates[i] == tex) { already = true; break; }
                        }
                        if (!already) {
                            g_clearCandidates[g_clearCandidateCount++] = tex;
                        }
                    }
                }
                tex->Release();
            }
            resource->Release();
        }
    }
    g_originalClearRTV(context, rtv, color);
}

// OMSetRenderTargets hook — Phase 2 of detection: if a sole-bound RT was also
// a ClearRTV candidate, it's the UI RT (GBuffer targets are MRT-only,
// backbuffer is never ClearRTV'd with {0,0,0,0}).
static void STDMETHODCALLTYPE hkOMSetRenderTargets(
    ID3D11DeviceContext* context,
    UINT numViews,
    ID3D11RenderTargetView* const* ppRTVs,
    ID3D11DepthStencilView* pDSV)
{
    if (numViews == 1 && ppRTVs && ppRTVs[0]) {
        ID3D11Resource* resource = nullptr;
        ppRTVs[0]->GetResource(&resource);
        if (resource) {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) {
                if (g_uiRTLocked) {
                    if (tex == g_uiRenderTarget) {
                        g_uiClearedThisFrame = true;
                    }
                } else {
                    // Check if this sole-bound RT is a ClearRTV candidate
                    for (int i = 0; i < g_clearCandidateCount; i++) {
                        if (g_clearCandidates[i] == tex) {
                            if (g_uiRenderTarget != tex) {
                                if (g_uiRenderTarget) g_uiRenderTarget->Release();
                                g_uiRenderTarget = tex;
                                g_uiRenderTarget->AddRef();
                                _MESSAGE("FO4RemixPlugin: UI RT detected (cleared + sole-bound): tex=%p", tex);
                            }
                            g_uiClearedThisFrame = true;
                            break;
                        }
                    }
                }
                tex->Release();
            }
            resource->Release();
        }
    }
    g_originalOMSetRTs(context, numViews, ppRTVs, pDSV);
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

        // Grab latest overlay data from game thread
        OverlayData overlay;
        if (g_overlayReady) {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            overlay.pixels.swap(g_overlayPixels);
            overlay.width = g_overlayWidth;
            overlay.height = g_overlayHeight;
            overlay.dxgiFormat = static_cast<uint32_t>(g_overlayDxgiFormat);
            overlay.valid = true;
            g_overlayReady = false;
        }

        // Pump messages before rendering so input is processed even when frames are slow
        PumpMessages();

        RemixRenderer::OnFrame(cam, overlay);

        // Pump again after rendering
        PumpMessages();

        // ~60fps cap to avoid spinning
        Sleep(16);
    }

    _MESSAGE("FO4RemixPlugin: Remix thread shutting down");
    RemixRenderer::Shutdown();
    RemixAPI::Shutdown();
}

static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    g_presentCallCount++;

    // Reset per-frame ClearRTV candidate list for next frame's detection
    g_clearCandidateCount = 0;

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

    // Hook OMSetRenderTargets + ClearRenderTargetView on first opportunity
    if (!g_contextHooked) {
        ID3D11Device* hookDevice = nullptr;
        swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&hookDevice);
        if (hookDevice) {
            ID3D11DeviceContext* hookCtx = nullptr;
            hookDevice->GetImmediateContext(&hookCtx);
            if (hookCtx) {
                void** ctxVtable = *reinterpret_cast<void***>(hookCtx);

                // OMSetRenderTargets (vtable index 33) — primary UI RT detection
                void* omSetRTsAddr = ctxVtable[33];
                bool omHooked = (MH_CreateHook(omSetRTsAddr, &hkOMSetRenderTargets,
                                    reinterpret_cast<void**>(&g_originalOMSetRTs)) == MH_OK &&
                                 MH_EnableHook(omSetRTsAddr) == MH_OK);

                // ClearRenderTargetView (vtable index 50) — sets clearedThisFrame flag
                void* clearRTVAddr = ctxVtable[50];
                bool clearHooked = (MH_CreateHook(clearRTVAddr, &hkClearRenderTargetView,
                                       reinterpret_cast<void**>(&g_originalClearRTV)) == MH_OK &&
                                    MH_EnableHook(clearRTVAddr) == MH_OK);

                if (omHooked && clearHooked) {
                    g_contextHooked = true;
                    _MESSAGE("FO4RemixPlugin: OMSetRenderTargets hooked at %p, ClearRTV hooked at %p",
                             omSetRTsAddr, clearRTVAddr);
                } else if (omHooked) {
                    g_contextHooked = true;
                    _MESSAGE("FO4RemixPlugin: OMSetRenderTargets hooked at %p (ClearRTV hook failed)", omSetRTsAddr);
                }
                hookCtx->Release();
            }
            hookDevice->Release();
        }
    }

    // Capture UI render target for screen overlay (isolated UI without 3D scene).
    // The flag is set by OMSetRenderTargets (UI RT was bound this frame) or
    // ClearRenderTargetView (UI RT was cleared this frame). Either means the
    // UI pass ran and we should copy.
    bool uiActiveThisFrame = g_uiClearedThisFrame.exchange(false);

    if (g_remixReady && g_uiRenderTarget && uiActiveThisFrame) {
        ID3D11Device* device = nullptr;
        swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&device);
        if (device) {
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);

            if (context) {
                D3D11_TEXTURE2D_DESC uiDesc;
                g_uiRenderTarget->GetDesc(&uiDesc);

                // Create/recreate staging texture if dimensions or format changed
                if (!g_stagingTex || g_stagingWidth != uiDesc.Width ||
                    g_stagingHeight != uiDesc.Height || g_stagingFormat != uiDesc.Format) {
                    if (g_stagingTex) { g_stagingTex->Release(); g_stagingTex = nullptr; }

                    D3D11_TEXTURE2D_DESC stagingDesc = {};
                    stagingDesc.Width = uiDesc.Width;
                    stagingDesc.Height = uiDesc.Height;
                    stagingDesc.MipLevels = 1;
                    stagingDesc.ArraySize = 1;
                    stagingDesc.Format = uiDesc.Format;
                    stagingDesc.SampleDesc.Count = 1;
                    stagingDesc.Usage = D3D11_USAGE_STAGING;
                    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

                    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &g_stagingTex);
                    if (SUCCEEDED(hr)) {
                        g_stagingWidth = uiDesc.Width;
                        g_stagingHeight = uiDesc.Height;
                        g_stagingFormat = uiDesc.Format;
                        _MESSAGE("FO4RemixPlugin: Created UI overlay staging texture %ux%u fmt=%u",
                                 g_stagingWidth, g_stagingHeight, (unsigned)g_stagingFormat);
                    } else {
                        _MESSAGE("FO4RemixPlugin: Failed to create UI overlay staging texture (0x%08X)", hr);
                    }
                }

                if (g_stagingTex) {
                    context->CopyResource(g_stagingTex, g_uiRenderTarget);

                    D3D11_MAPPED_SUBRESOURCE mapped;
                    HRESULT hr = context->Map(g_stagingTex, 0, D3D11_MAP_READ, 0, &mapped);
                    if (SUCCEEDED(hr)) {
                        uint32_t w = g_stagingWidth;
                        uint32_t h = g_stagingHeight;
                        uint32_t rowBytes = w * 4;

                        std::lock_guard<std::mutex> lock(g_overlayMutex);
                        g_overlayPixels.resize(rowBytes * h);

                        // Copy pixels and un-premultiply alpha.
                        // The UI RT uses SrcAlpha/1-SrcAlpha blending onto a (0,0,0,0) background,
                        // so RGB values are premultiplied. DrawScreenOverlay expects straight alpha.
                        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
                        uint8_t* dst = g_overlayPixels.data();
                        for (uint32_t y = 0; y < h; y++) {
                            const uint8_t* srcRow = src + y * mapped.RowPitch;
                            uint8_t* dstRow = dst + y * rowBytes;
                            for (uint32_t x = 0; x < w; x++) {
                                uint8_t r = srcRow[x * 4 + 0];
                                uint8_t g = srcRow[x * 4 + 1];
                                uint8_t b = srcRow[x * 4 + 2];
                                uint8_t a = srcRow[x * 4 + 3];
                                if (a > 0 && a < 255) {
                                    dstRow[x * 4 + 0] = (uint8_t)(std::min)(255u, (uint32_t)r * 255u / a);
                                    dstRow[x * 4 + 1] = (uint8_t)(std::min)(255u, (uint32_t)g * 255u / a);
                                    dstRow[x * 4 + 2] = (uint8_t)(std::min)(255u, (uint32_t)b * 255u / a);
                                } else {
                                    dstRow[x * 4 + 0] = r;
                                    dstRow[x * 4 + 1] = g;
                                    dstRow[x * 4 + 2] = b;
                                }
                                dstRow[x * 4 + 3] = a;
                            }
                        }

                        g_overlayWidth = w;
                        g_overlayHeight = h;
                        g_overlayDxgiFormat = uiDesc.Format;
                        g_overlayReady = true;

                        context->Unmap(g_stagingTex, 0);

                        // Lock the RT pointer after first successful capture —
                        // stops re-detection from picking the wrong RT on future frames
                        if (!g_uiRTLocked) {
                            g_uiRTLocked = true;
                            _MESSAGE("FO4RemixPlugin: UI RT locked at %p (%ux%u)",
                                     g_uiRenderTarget, w, h);
                        }
                    }
                }
            }
            if (context) context->Release();
            device->Release();
        }
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

            if (!toUnload.empty() || !toExtract.empty()) {
                _MESSAGE("FO4RemixPlugin: Cell transition - %zu currently loaded, %zu to unload, %zu to extract, %zu tracked",
                         currentlyLoaded.size(), toUnload.size(), toExtract.size(), g_extractedCells.size());
            }

            // Re-extract cells that had missing textures (streaming wasn't done)
            for (auto it = g_pendingReextract.begin(); it != g_pendingReextract.end(); ) {
                if (g_presentCallCount >= it->second) {
                    uint32_t cellID = it->first;
                    auto ptrIt = g_cellPtrMap.find(cellID);
                    if (ptrIt != g_cellPtrMap.end() && currentlyLoaded.count(cellID)) {
                        toExtract.push_back({ ptrIt->second, cellID });
                        // Remove from extractedCells so it gets treated as new
                        g_extractedCells.erase(cellID);
                        _MESSAGE("FO4RemixPlugin: Re-extracting cell 0x%08X (texture streaming retry)", cellID);
                    }
                    it = g_pendingReextract.erase(it);
                } else {
                    ++it;
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

                        // Check if any mesh is missing its diffuse texture (not yet streamed in)
                        bool hasMissingTextures = false;
                        for (auto& mesh : extraction.meshes) {
                            if (mesh.diffuseTextureHash == 0) {
                                hasMissingTextures = true;
                                break;
                            }
                        }
                        if (hasMissingTextures) {
                            g_pendingReextract[cellInfo.formID] = g_presentCallCount + kReextractDelay;
                            _MESSAGE("FO4RemixPlugin: Cell 0x%08X has missing textures, scheduling re-extract in ~3s",
                                     cellInfo.formID);
                        }

                        newScenes.push_back({ cellInfo.formID, std::move(extraction) });
                    }
                }

                // Refresh one existing cell (meshes/textures only — lights don't need
                // refreshing and destroying+recreating them breaks Remix rendering)
                if (refreshCell.cellPtr != 0) {
                    _MESSAGE("FO4RemixPlugin: Refreshing cell 0x%08X", refreshCell.formID);
                    auto extraction = SceneExtractor::ExtractCell(refreshCell.cellPtr, device);
                    extraction.lights.clear();
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
                    g_pendingReextract.erase(cellID);
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

void PresentHook::ResetExtractionState() {
    _MESSAGE("FO4RemixPlugin: Resetting extraction state (save game load)");

    // Tell remix thread to unload everything
    {
        std::lock_guard<std::mutex> lock(g_sceneMutex);
        for (uint32_t cellID : g_extractedCells) {
            g_pendingUnloads.push_back(cellID);
        }
        g_sceneReady = true;
    }

    // Reset all main-thread extraction tracking
    g_extractedCells.clear();
    g_refreshQueue.clear();
    g_refreshIndex = 0;
    g_cellPtrMap.clear();
    g_pendingReextract.clear();

    // Reset UI RT detection — RT resource may change after load
    if (g_uiRenderTarget) {
        g_uiRenderTarget->Release();
        g_uiRenderTarget = nullptr;
    }
    g_uiRTLocked = false;
    g_uiClearedThisFrame = false;
    g_clearCandidateCount = 0;

    // Force the initial bootstrap path with retry/quality-gate logic
    g_firstExtractionDone = false;
    g_extractionAttempts = 0;
    g_nextExtractFrame = g_presentCallCount + kInitialRetryDelay;

    SceneExtractor::ClearTextureCache();
}

void PresentHook::Uninstall() {
    g_remixRunning = false;
    if (g_remixThread.joinable()) {
        g_remixThread.join();
    }
    if (g_uiRenderTarget) {
        g_uiRenderTarget->Release();
        g_uiRenderTarget = nullptr;
    }
    if (g_stagingTex) {
        g_stagingTex->Release();
        g_stagingTex = nullptr;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}
