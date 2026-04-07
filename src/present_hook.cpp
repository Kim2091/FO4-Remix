#include "present_hook.h"
#include "config.h"
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

// Per-cell scene data — main thread produces, remix thread consumes
struct PendingCellScene {
    uint32_t cellFormID;
    ExtractionResult data;
};

// Extraction timing constants
static constexpr int kInitialRetryDelay = 120;     // ~2 seconds at 60fps
static constexpr int kMaxRetryDelay = 600;         // ~10 seconds
static constexpr int kMaxInitialAttempts = 15;
static constexpr uint32_t kMinExpectedMeshes = 5;
static constexpr int kNewCellCheckInterval = 60;    // ~1 second at 60fps — check for new cells
static constexpr int kRefreshInterval = 1800;       // ~30 seconds at 60fps — refresh one existing cell
static constexpr int kReextractDelay = 180;         // ~3 seconds — retry cells with missing textures

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

// ---------------------------------------------------------------------------
// Grouped state objects — replaces scattered file-scope globals
// ---------------------------------------------------------------------------

// Remix thread lifecycle and shared camera
static struct RemixThreadState {
    std::thread              thread;
    std::atomic<bool>        running { false };
    std::atomic<bool>        ready { false };
    std::mutex               cameraMutex;
    CameraState              sharedCamera = {};
    uint32_t                 gameWidth = 1280;
    uint32_t                 gameHeight = 720;
} g_remix;

// Scene data pipeline (main thread -> remix thread)
static struct ScenePipeline {
    std::mutex               mutex;
    std::vector<PendingCellScene> pendingScenes;
    std::vector<uint32_t>    pendingUnloads;
    std::atomic<bool>        ready { false };

    // Multi-cell tracking
    std::unordered_set<uint32_t>          extractedCells;
    std::vector<uint32_t>                 refreshQueue;
    size_t                                refreshIndex = 0;
    std::unordered_map<uint32_t, uintptr_t> cellPtrMap;

    // Extraction timing
    int                      attempts = 0;
    int                      nextExtractFrame = 0;
    bool                     firstExtractionDone = false;
    std::unordered_map<uint32_t, int> pendingReextract;
} g_scene;

// Overlay capture from game UI render target
static struct OverlayCapture {
    ID3D11Texture2D*         stagingTex = nullptr;
    uint32_t                 stagingWidth = 0;
    uint32_t                 stagingHeight = 0;
    DXGI_FORMAT              stagingFormat = DXGI_FORMAT_UNKNOWN;

    std::mutex               mutex;
    std::vector<uint8_t>     pixels;
    uint32_t                 width = 0;
    uint32_t                 height = 0;
    DXGI_FORMAT              dxgiFormat = DXGI_FORMAT_UNKNOWN;
    std::atomic<bool>        ready { false };
} g_overlay;

// Skinned mesh tracking and bone transform pipeline
static struct SkinnedMeshState {
    std::mutex               trackingMutex;
    std::vector<ExtractedSkinnedMesh> tracked;

    std::mutex               boneMutex;
    std::vector<ExtractedSkinnedMesh> forRendering;
    uint32_t                 updateFrameCounter = 0;
} g_skinning;

// UI render target detection state
static struct UIDetectionState {
    ID3D11Texture2D*         renderTarget = nullptr;
    bool                     locked = false;
    std::atomic<bool>        clearedThisFrame { false };
    std::atomic<bool>        drawnThisFrame { false };
    bool                     contextHooked = false;

    static constexpr int     kMaxClearCandidates = 8;
    ID3D11Texture2D*         clearCandidates[kMaxClearCandidates] = {};
    int                      clearCandidateCount = 0;
    int                      presentCallCount = 0;
} g_ui;

// Merge newly extracted skinned meshes into the persistent tracking list.
// Called on the game thread after extraction completes.
static void MergeSkinnedMeshesIntoTracking(const std::vector<PendingCellScene>& scenes) {
    std::lock_guard<std::mutex> lock(g_skinning.trackingMutex);
    for (const auto& scene : scenes) {
        for (const auto& sm : scene.data.skinnedMeshes) {
            // Check if this mesh is already tracked (by hash)
            auto it = std::find_if(g_skinning.tracked.begin(),
                                   g_skinning.tracked.end(),
                                   [&](const ExtractedSkinnedMesh& existing) {
                                       return existing.hash == sm.hash;
                                   });
            if (it != g_skinning.tracked.end()) {
                // Update existing entry (bone pointers may have changed on refresh)
                _MESSAGE("FO4RemixPlugin: [SKINNING] Updated tracked skinned mesh hash=0x%llX owner=0x%08X bones=%u",
                         (unsigned long long)sm.hash, sm.ownerFormID, sm.boneCount);
                *it = sm;
            } else {
                // New skinned mesh
                _MESSAGE("FO4RemixPlugin: [SKINNING] Added tracked skinned mesh hash=0x%llX owner=0x%08X bones=%u verts=%u",
                         (unsigned long long)sm.hash, sm.ownerFormID, sm.boneCount, sm.vertexCount);
                g_skinning.tracked.push_back(sm);
            }
        }
    }
}


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
                if (g_ui.locked) {
                    if (tex == g_ui.renderTarget) {
                        g_ui.clearedThisFrame = true;
                    }
                } else {
                    // Detection phase: add matching textures to per-frame candidate list
                    D3D11_TEXTURE2D_DESC desc;
                    tex->GetDesc(&desc);
                    if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                        desc.Width == g_remix.gameWidth && desc.Height == g_remix.gameHeight &&
                        desc.Usage == D3D11_USAGE_DEFAULT &&
                        g_ui.clearCandidateCount < g_ui.kMaxClearCandidates) {
                        // Avoid duplicates
                        bool already = false;
                        for (int i = 0; i < g_ui.clearCandidateCount; i++) {
                            if (g_ui.clearCandidates[i] == tex) { already = true; break; }
                        }
                        if (!already) {
                            g_ui.clearCandidates[g_ui.clearCandidateCount++] = tex;
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
                if (g_ui.locked) {
                    if (tex == g_ui.renderTarget) {
                        // If the game didn't clear the RT this frame, do it ourselves
                        // to prevent stale content from prior frames causing ghosting.
                        if (!g_ui.clearedThisFrame) {
                            float clearColor[4] = {0, 0, 0, 0};
                            g_originalClearRTV(context, ppRTVs[0], clearColor);
                            g_ui.clearedThisFrame = true;
                        }
                        g_ui.drawnThisFrame = true;
                    }
                } else {
                    // Check if this sole-bound RT is a ClearRTV candidate
                    for (int i = 0; i < g_ui.clearCandidateCount; i++) {
                        if (g_ui.clearCandidates[i] == tex) {
                            if (g_ui.renderTarget != tex) {
                                if (g_ui.renderTarget) g_ui.renderTarget->Release();
                                g_ui.renderTarget = tex;
                                g_ui.renderTarget->AddRef();
                                _MESSAGE("FO4RemixPlugin: UI RT detected (cleared + sole-bound): tex=%p", tex);
                            }
                            g_ui.clearedThisFrame = true;
                            g_ui.drawnThisFrame = true;
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

    if (!RemixAPI::Initialize(nullptr, g_remix.gameWidth, g_remix.gameHeight)) {
        _MESSAGE("FO4RemixPlugin: ERROR - Remix API init failed on remix thread");
        return;
    }
    _MESSAGE("FO4RemixPlugin: Remix API initialized on remix thread");

    if (!RemixRenderer::Init()) {
        _MESSAGE("FO4RemixPlugin: ERROR - Renderer init failed on remix thread");
        return;
    }

    g_remix.ready = true;

    while (g_remix.running) {
        // Check for pending scene data from main thread
        if (g_scene.ready) {
            std::vector<PendingCellScene> cellScenes;
            std::vector<uint32_t> unloads;
            {
                std::lock_guard<std::mutex> lock(g_scene.mutex);
                cellScenes = std::move(g_scene.pendingScenes);
                unloads = std::move(g_scene.pendingUnloads);
                g_scene.ready = false;
            }

            // Unload cells that are no longer loaded by the engine
            for (uint32_t cellID : unloads) {
                _MESSAGE("FO4RemixPlugin: Remix thread unloading cell 0x%08X", cellID);
                RemixRenderer::UnloadCell(cellID);
            }

            // Load new/refreshed cell scenes
            for (auto& cell : cellScenes) {
                if (!cell.data.meshes.empty() || !cell.data.skinnedMeshes.empty()) {
                    _MESSAGE("FO4RemixPlugin: Remix thread loading cell 0x%08X (%zu meshes, %zu skinned, %zu textures, %zu lights)",
                             cell.cellFormID, cell.data.meshes.size(), cell.data.skinnedMeshes.size(),
                             cell.data.textures.size(), cell.data.lights.size());
                    RemixRenderer::LoadCellScene(cell.cellFormID, std::move(cell.data));
                }
            }
        }

        // Grab latest camera from main thread
        CameraState cam;
        {
            std::lock_guard<std::mutex> lock(g_remix.cameraMutex);
            cam = g_remix.sharedCamera;
        }

        // Grab latest bone transform data from game thread
        std::vector<ExtractedSkinnedMesh> skinnedSnapshot;
        {
            std::lock_guard<std::mutex> lock(g_skinning.boneMutex);
            skinnedSnapshot = g_skinning.forRendering;
        }

        // Grab latest overlay data from game thread
        OverlayData overlay;
        if (g_overlay.ready) {
            std::lock_guard<std::mutex> lock(g_overlay.mutex);
            overlay.pixels.swap(g_overlay.pixels);
            overlay.width = g_overlay.width;
            overlay.height = g_overlay.height;
            overlay.dxgiFormat = static_cast<uint32_t>(g_overlay.dxgiFormat);
            overlay.valid = true;
            g_overlay.ready = false;
        }

        // Pump messages before rendering so input is processed even when frames are slow
        PumpMessages();

        RemixRenderer::OnFrame(cam, skinnedSnapshot, overlay);

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
    g_ui.presentCallCount++;

    // Reset per-frame ClearRTV candidate list for next frame's detection
    g_ui.clearCandidateCount = 0;

    // Start remix thread on first Present call
    if (!g_remix.running && !g_remix.thread.joinable()) {
        // Query game swap chain for resolution
        DXGI_SWAP_CHAIN_DESC scDesc = {};
        if (SUCCEEDED(swapChain->GetDesc(&scDesc)) && scDesc.BufferDesc.Width > 0) {
            g_remix.gameWidth = scDesc.BufferDesc.Width;
            g_remix.gameHeight = scDesc.BufferDesc.Height;
            _MESSAGE("FO4RemixPlugin: Game resolution: %ux%u", g_remix.gameWidth, g_remix.gameHeight);
        }
        _MESSAGE("FO4RemixPlugin: hkPresent - launching Remix thread");
        g_remix.running = true;
        g_remix.thread = std::thread(RemixThreadFunc);
    }

    // Update shared camera state for the remix thread
    if (g_remix.ready) {
        CameraState cam = Camera::Get();
        std::lock_guard<std::mutex> lock(g_remix.cameraMutex);
        g_remix.sharedCamera = cam;
    }

    // Per-frame bone transform update for all tracked skinned meshes (EVERY frame).
    // This is what makes skeletal animations work — must not be on the 30-second timer.
    if (g_remix.ready && g_gameDataReady && g_config.skinningEnabled) {
        g_skinning.updateFrameCounter++;

        // Update bone transforms and copy for Remix thread under a single lock sequence.
        // Lock order: g_skinning.trackingMutex first, then g_skinning.boneMutex.
        {
            std::lock_guard<std::mutex> lockSkinned(g_skinning.trackingMutex);
            if (!g_skinning.tracked.empty()) {
                Skinning::UpdateBoneTransforms(g_skinning.tracked);
            }

            // Copy updated transforms for Remix thread consumption
            {
                std::lock_guard<std::mutex> lockBone(g_skinning.boneMutex);
                g_skinning.forRendering = g_skinning.tracked;
            }
        }

        // Rate-limited bone update logging (every 300 frames, ~5 seconds)
        if (g_skinning.updateFrameCounter % 300 == 0) {
            std::lock_guard<std::mutex> lock(g_skinning.trackingMutex);
            if (!g_skinning.tracked.empty()) {
                _MESSAGE("FO4RemixPlugin: [SKINNING] Bone update frame %u: %zu tracked skinned meshes",
                         g_skinning.updateFrameCounter, g_skinning.tracked.size());
                // Log first mesh bone[0] position for debugging
                const auto& firstMesh = g_skinning.tracked[0];
                if (firstMesh.boneCount > 0 && !firstMesh.currentBoneTransforms.empty()) {
                    const auto& b0 = firstMesh.currentBoneTransforms[0];
                    _MESSAGE("FO4RemixPlugin: [SKINNING]   Mesh hash=0x%llX bone[0] pos=[%.1f, %.1f, %.1f]",
                             (unsigned long long)firstMesh.hash, b0[3], b0[7], b0[11]);
                }
            }
        }
    }

    // Hook OMSetRenderTargets + ClearRenderTargetView on first opportunity
    if (!g_ui.contextHooked) {
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
                    g_ui.contextHooked = true;
                    _MESSAGE("FO4RemixPlugin: OMSetRenderTargets hooked at %p, ClearRTV hooked at %p",
                             omSetRTsAddr, clearRTVAddr);
                } else if (omHooked) {
                    g_ui.contextHooked = true;
                    _MESSAGE("FO4RemixPlugin: OMSetRenderTargets hooked at %p (ClearRTV hook failed)", omSetRTsAddr);
                }
                hookCtx->Release();
            }
            hookDevice->Release();
        }
    }

    // Capture UI render target for screen overlay (isolated UI without 3D scene).
    // Require BOTH flags: ClearRTV (RT was zeroed) AND OMSetRTs (RT was drawn to).
    // If only drawn without a clear, the RT has accumulated stale content from
    // prior frames — skip the copy to avoid ghosting/trailing artifacts.
    bool uiCleared = g_ui.clearedThisFrame.exchange(false);
    bool uiDrawn = g_ui.drawnThisFrame.exchange(false);
    bool uiActiveThisFrame = uiCleared && uiDrawn;

    if (g_remix.ready && g_ui.renderTarget && uiActiveThisFrame) {
        ID3D11Device* device = nullptr;
        swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&device);
        if (device) {
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);

            if (context) {
                D3D11_TEXTURE2D_DESC uiDesc;
                g_ui.renderTarget->GetDesc(&uiDesc);

                // Create/recreate staging texture if dimensions or format changed
                if (!g_overlay.stagingTex || g_overlay.stagingWidth != uiDesc.Width ||
                    g_overlay.stagingHeight != uiDesc.Height || g_overlay.stagingFormat != uiDesc.Format) {
                    if (g_overlay.stagingTex) { g_overlay.stagingTex->Release(); g_overlay.stagingTex = nullptr; }

                    D3D11_TEXTURE2D_DESC stagingDesc = {};
                    stagingDesc.Width = uiDesc.Width;
                    stagingDesc.Height = uiDesc.Height;
                    stagingDesc.MipLevels = 1;
                    stagingDesc.ArraySize = 1;
                    stagingDesc.Format = uiDesc.Format;
                    stagingDesc.SampleDesc.Count = 1;
                    stagingDesc.Usage = D3D11_USAGE_STAGING;
                    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

                    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &g_overlay.stagingTex);
                    if (SUCCEEDED(hr)) {
                        g_overlay.stagingWidth = uiDesc.Width;
                        g_overlay.stagingHeight = uiDesc.Height;
                        g_overlay.stagingFormat = uiDesc.Format;
                        _MESSAGE("FO4RemixPlugin: Created UI overlay staging texture %ux%u fmt=%u",
                                 g_overlay.stagingWidth, g_overlay.stagingHeight, (unsigned)g_overlay.stagingFormat);
                    } else {
                        _MESSAGE("FO4RemixPlugin: Failed to create UI overlay staging texture (0x%08X)", hr);
                    }
                }

                if (g_overlay.stagingTex) {
                    context->CopyResource(g_overlay.stagingTex, g_ui.renderTarget);

                    D3D11_MAPPED_SUBRESOURCE mapped;
                    HRESULT hr = context->Map(g_overlay.stagingTex, 0, D3D11_MAP_READ, 0, &mapped);
                    if (SUCCEEDED(hr)) {
                        uint32_t w = g_overlay.stagingWidth;
                        uint32_t h = g_overlay.stagingHeight;
                        uint32_t rowBytes = w * 4;

                        std::lock_guard<std::mutex> lock(g_overlay.mutex);
                        g_overlay.pixels.resize(rowBytes * h);

                        // Copy pixels and un-premultiply alpha.
                        // The UI RT uses SrcAlpha/1-SrcAlpha blending onto a (0,0,0,0) background,
                        // so RGB values are premultiplied. DrawScreenOverlay expects straight alpha.
                        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
                        uint8_t* dst = g_overlay.pixels.data();
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

                        g_overlay.width = w;
                        g_overlay.height = h;
                        g_overlay.dxgiFormat = uiDesc.Format;
                        g_overlay.ready = true;

                        context->Unmap(g_overlay.stagingTex, 0);

                        // Lock the RT pointer after first successful capture —
                        // stops re-detection from picking the wrong RT on future frames
                        if (!g_ui.locked) {
                            g_ui.locked = true;
                            _MESSAGE("FO4RemixPlugin: UI RT locked at %p (%ux%u)",
                                     g_ui.renderTarget, w, h);
                        }
                    }
                }
            }
            if (context) context->Release();
            device->Release();
        }
    }

    // Multi-cell extraction: initial bootstrap, then incremental new-cell + round-robin refresh
    if (g_remix.ready && g_gameDataReady && g_ui.presentCallCount >= g_scene.nextExtractFrame) {
        if (!g_scene.firstExtractionDone) {
            // Initial extraction: retry with backoff until we get enough meshes from any cell
            if (g_scene.attempts < kMaxInitialAttempts && SceneExtractor::IsPlayerCellReady()) {
                g_scene.attempts++;
                _MESSAGE("FO4RemixPlugin: Initial multi-cell extraction attempt %d/%d (frame %d)...",
                         g_scene.attempts, kMaxInitialAttempts, g_ui.presentCallCount);

                auto loadedCells = SceneExtractor::GetLoadedCells();

                ID3D11Device* device = nullptr;
                swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&device);

                std::vector<PendingCellScene> newScenes;
                size_t totalMeshes = 0;

                for (auto& cellInfo : loadedCells) {
                    auto extraction = SceneExtractor::ExtractCell(cellInfo.cellPtr, device);
                    totalMeshes += extraction.meshes.size() + extraction.skinnedMeshes.size();
                    if (!extraction.meshes.empty() || !extraction.skinnedMeshes.empty()) {
                        g_scene.extractedCells.insert(cellInfo.formID);
                        g_scene.refreshQueue.push_back(cellInfo.formID);
                        g_scene.cellPtrMap[cellInfo.formID] = cellInfo.cellPtr;
                        newScenes.push_back({ cellInfo.formID, std::move(extraction) });
                    }
                }

                if (device) device->Release();

                if (totalMeshes >= kMinExpectedMeshes) {
                    _MESSAGE("FO4RemixPlugin: Initial extraction succeeded: %zu cells, %zu total meshes",
                             newScenes.size(), totalMeshes);
                    g_scene.firstExtractionDone = true;
                    g_scene.nextExtractFrame = g_ui.presentCallCount + kNewCellCheckInterval;
                    g_scene.refreshIndex = 0;
                    // Merge skinned meshes into tracking BEFORE moving data to Remix thread
                    MergeSkinnedMeshesIntoTracking(newScenes);
                    std::lock_guard<std::mutex> lock(g_scene.mutex);
                    g_scene.pendingScenes = std::move(newScenes);
                    g_scene.ready = true;
                } else {
                    int delay = kInitialRetryDelay << (g_scene.attempts - 1);
                    if (delay > kMaxRetryDelay) delay = kMaxRetryDelay;
                    g_scene.nextExtractFrame = g_ui.presentCallCount + delay;
                    _MESSAGE("FO4RemixPlugin: Initial extraction got %zu meshes (need >= %u), "
                             "retrying in %d frames (~%.1fs)",
                             totalMeshes, kMinExpectedMeshes, delay, delay / 60.0f);
                }
            } else if (g_scene.attempts >= kMaxInitialAttempts) {
                _MESSAGE("FO4RemixPlugin: WARNING - Gave up initial extraction after %d attempts",
                         kMaxInitialAttempts);
                g_scene.firstExtractionDone = true;
                g_scene.nextExtractFrame = g_ui.presentCallCount + kNewCellCheckInterval;
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
            for (uint32_t cellID : g_scene.extractedCells) {
                if (currentlyLoaded.find(cellID) == currentlyLoaded.end()) {
                    toUnload.push_back(cellID);
                }
            }

            // Detect new cells to extract
            std::vector<CellInfo> toExtract;
            for (auto& cellInfo : loadedCells) {
                if (g_scene.extractedCells.find(cellInfo.formID) == g_scene.extractedCells.end()) {
                    toExtract.push_back(cellInfo);
                }
            }

            if (!toUnload.empty() || !toExtract.empty()) {
                _MESSAGE("FO4RemixPlugin: Cell transition - %zu currently loaded, %zu to unload, %zu to extract, %zu tracked",
                         currentlyLoaded.size(), toUnload.size(), toExtract.size(), g_scene.extractedCells.size());
            }

            // Re-extract cells that had missing textures (streaming wasn't done)
            for (auto it = g_scene.pendingReextract.begin(); it != g_scene.pendingReextract.end(); ) {
                if (g_ui.presentCallCount >= it->second) {
                    uint32_t cellID = it->first;
                    auto ptrIt = g_scene.cellPtrMap.find(cellID);
                    if (ptrIt != g_scene.cellPtrMap.end() && currentlyLoaded.count(cellID)) {
                        toExtract.push_back({ ptrIt->second, cellID });
                        // Remove from extractedCells so it gets treated as new
                        g_scene.extractedCells.erase(cellID);
                        _MESSAGE("FO4RemixPlugin: Re-extracting cell 0x%08X (texture streaming retry)", cellID);
                    }
                    it = g_scene.pendingReextract.erase(it);
                } else {
                    ++it;
                }
            }

            // Pick one existing cell to refresh (round-robin)
            CellInfo refreshCell = { 0, 0 };
            static int g_refreshFrameCounter = 0;
            g_refreshFrameCounter += kNewCellCheckInterval;
            if (g_refreshFrameCounter >= kRefreshInterval && !g_scene.refreshQueue.empty()) {
                g_refreshFrameCounter = 0;
                // Find next valid cell in refresh queue
                for (size_t attempts = 0; attempts < g_scene.refreshQueue.size(); attempts++) {
                    if (g_scene.refreshIndex >= g_scene.refreshQueue.size()) g_scene.refreshIndex = 0;
                    uint32_t candidateID = g_scene.refreshQueue[g_scene.refreshIndex];
                    g_scene.refreshIndex++;
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
                    if (!extraction.meshes.empty() || !extraction.skinnedMeshes.empty()) {
                        g_scene.extractedCells.insert(cellInfo.formID);
                        g_scene.refreshQueue.push_back(cellInfo.formID);
                        g_scene.cellPtrMap[cellInfo.formID] = cellInfo.cellPtr;

                        // Check if any mesh is missing its diffuse texture (not yet streamed in)
                        bool hasMissingTextures = false;
                        for (auto& mesh : extraction.meshes) {
                            if (mesh.diffuseTextureHash == 0) {
                                hasMissingTextures = true;
                                break;
                            }
                        }
                        if (hasMissingTextures) {
                            g_scene.pendingReextract[cellInfo.formID] = g_ui.presentCallCount + kReextractDelay;
                            _MESSAGE("FO4RemixPlugin: Cell 0x%08X has missing textures, scheduling re-extract in ~3s",
                                     cellInfo.formID);
                        }

                        newScenes.push_back({ cellInfo.formID, std::move(extraction) });
                    }
                }

                // Refresh one existing cell (meshes/textures only -- lights don't need
                // refreshing and destroying+recreating them breaks Remix rendering)
                if (refreshCell.cellPtr != 0) {
                    _MESSAGE("FO4RemixPlugin: Refreshing cell 0x%08X", refreshCell.formID);
                    auto extraction = SceneExtractor::ExtractCell(refreshCell.cellPtr, device);
                    extraction.lights.clear();
                    if (!extraction.meshes.empty() || !extraction.skinnedMeshes.empty()) {
                        newScenes.push_back({ refreshCell.formID, std::move(extraction) });
                    }
                }

                if (device) device->Release();
            }

            // Process unloads
            if (!toUnload.empty()) {
                for (uint32_t cellID : toUnload) {
                    _MESSAGE("FO4RemixPlugin: Cell 0x%08X no longer loaded, scheduling unload", cellID);
                    g_scene.extractedCells.erase(cellID);
                    g_scene.cellPtrMap.erase(cellID);
                    g_scene.pendingReextract.erase(cellID);
                    // Remove from refresh queue
                    g_scene.refreshQueue.erase(
                        std::remove(g_scene.refreshQueue.begin(), g_scene.refreshQueue.end(), cellID),
                        g_scene.refreshQueue.end());
                }
                if (g_scene.refreshIndex >= g_scene.refreshQueue.size()) g_scene.refreshIndex = 0;
                needsUpdate = true;

                // Remove tracked skinned meshes belonging to unloaded cells.
                // We remove by ownerFormID: any skinned mesh whose owner was in an unloaded cell
                // should be removed to prevent reading stale bone pointers.
                {
                    std::lock_guard<std::mutex> lock(g_skinning.trackingMutex);
                    size_t beforeCount = g_skinning.tracked.size();
                    // We don't have a direct cell->ownerFormID mapping, so we remove ALL
                    // skinned meshes that were tracked. They will be re-added on re-extraction.
                    // This is conservative but safe. In practice, cell unloads are infrequent.
                    // A more precise approach would be to track which ownerFormIDs belong
                    // to which cells, but for now we just remove all and let re-extraction
                    // re-populate them.
                    // Actually, we CAN be more precise: remove meshes whose bone node pointers
                    // might be invalidated. Since we know which cells are unloading, and those
                    // cells' REFRs will be destroyed, we can check if an owner's REFR belongs
                    // to one of those cells. But without that mapping, let's remove all tracked
                    // meshes and let re-extraction + the next round-robin cycle repopulate.
                    // This causes a brief flash of missing characters but ensures safety.
                    if (beforeCount > 0) {
                        _MESSAGE("FO4RemixPlugin: [SKINNING] Clearing %zu tracked skinned meshes due to %zu cell unloads",
                                 beforeCount, toUnload.size());
                        g_skinning.tracked.clear();
                    }
                }
            }

            // Merge skinned meshes into tracking BEFORE moving data to Remix thread
            if (!newScenes.empty()) {
                MergeSkinnedMeshesIntoTracking(newScenes);
            }

            if (!newScenes.empty() || needsUpdate) {
                SceneExtractor::ClearTextureCache();
                std::lock_guard<std::mutex> lock(g_scene.mutex);
                // Append to any pending scenes not yet consumed
                for (auto& scene : newScenes) {
                    g_scene.pendingScenes.push_back(std::move(scene));
                }
                for (uint32_t cellID : toUnload) {
                    g_scene.pendingUnloads.push_back(cellID);
                }
                g_scene.ready = true;
            }

            g_scene.nextExtractFrame = g_ui.presentCallCount + kNewCellCheckInterval;
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
        std::lock_guard<std::mutex> lock(g_scene.mutex);
        for (uint32_t cellID : g_scene.extractedCells) {
            g_scene.pendingUnloads.push_back(cellID);
        }
        g_scene.ready = true;
    }

    // Reset all main-thread extraction tracking
    g_scene.extractedCells.clear();
    g_scene.refreshQueue.clear();
    g_scene.refreshIndex = 0;
    g_scene.cellPtrMap.clear();
    g_scene.pendingReextract.clear();

    // Clear all tracked skinned meshes (bone pointers are invalidated on save load)
    {
        std::lock_guard<std::mutex> lock(g_skinning.trackingMutex);
        _MESSAGE("FO4RemixPlugin: [SKINNING] Clearing %zu tracked skinned meshes (save load reset)",
                 g_skinning.tracked.size());
        g_skinning.tracked.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_skinning.boneMutex);
        g_skinning.forRendering.clear();
    }
    g_skinning.updateFrameCounter = 0;

    // Reset UI RT detection — RT resource may change after load
    if (g_ui.renderTarget) {
        g_ui.renderTarget->Release();
        g_ui.renderTarget = nullptr;
    }
    g_ui.locked = false;
    g_ui.clearedThisFrame = false;
    g_ui.clearCandidateCount = 0;

    // Force the initial bootstrap path with retry/quality-gate logic
    g_scene.firstExtractionDone = false;
    g_scene.attempts = 0;
    g_scene.nextExtractFrame = g_ui.presentCallCount + kInitialRetryDelay;

    SceneExtractor::ClearTextureCache();
}

void PresentHook::Uninstall() {
    g_remix.running = false;
    if (g_remix.thread.joinable()) {
        g_remix.thread.join();
    }
    if (g_ui.renderTarget) {
        g_ui.renderTarget->Release();
        g_ui.renderTarget = nullptr;
    }
    if (g_overlay.stagingTex) {
        g_overlay.stagingTex->Release();
        g_overlay.stagingTex = nullptr;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}
