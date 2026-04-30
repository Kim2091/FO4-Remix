#include "present_hook.h"
#include "config.h"
#include "fo4_diagnostics.h"
#include "remix_api.h"
#include "remix_renderer.h"
#include "camera.h"
#include "semantic_capture.h"
#include "weather_bridge.h"

#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>

#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present g_originalPresent = nullptr;

// Game readiness — set by F4SE message handler in main.cpp
extern std::atomic<bool> g_gameDataReady;

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
    HWND                     gameHwnd = nullptr;
} g_remix;

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

    if (!RemixAPI::Initialize(g_remix.gameHwnd, g_remix.gameWidth, g_remix.gameHeight)) {
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
        // Grab latest camera from main thread
        CameraState cam;
        {
            std::lock_guard<std::mutex> lock(g_remix.cameraMutex);
            cam = g_remix.sharedCamera;
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
    const uint64_t frameIndex = Diagnostics::Tick();
    if (Diagnostics::ShouldEmitPeriodic(frameIndex)) {
        const auto gs = Diagnostics::SnapshotGameState();
        Diagnostics::EmitPeriodic(frameIndex, gs);
    }

    // Override Remix's RIDEV_NOLEGACY keyboard registration once the overlay
    // thread has had time to set it up (~1s after init). Internally idempotent:
    // returns immediately after the first successful RIDEV_REMOVE.
    if (frameIndex > 60) {
        RemixAPI::RestoreLegacyKeyboardInput();
    }

    // Once the Remix thread has finished init (g_remix.ready is set right
    // after Remix Startup / dxvk_RegisterD3D9Device returns), re-bind
    // raw-input keyboard+mouse to the game HWND. Win32 raw-input is
    // process-wide last-call-wins per device class; firing AFTER the
    // runtime's overlay-HWND registration lets us reclaim input for FO4.
    // Frame-number gating is unreliable -- new runtime builds can finish
    // init at very different frame counts.  Idempotent (returns after first
    // successful registration).
    if (g_remix.ready && g_remix.gameHwnd) {
        RemixAPI::RebindRawInputToGameWindow(g_remix.gameHwnd);
    }

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
            g_remix.gameHwnd = scDesc.OutputWindow;
            _MESSAGE("FO4RemixPlugin: Game resolution: %ux%u (gameHwnd=%p)",
                     g_remix.gameWidth, g_remix.gameHeight, g_remix.gameHwnd);
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

    // Push time-of-day to Remix's atmosphere config keys. Same readiness
    // gate as the bone-update block — Remix must be initialized AND game
    // data must be loaded before LookupFormByID can return a valid TESGlobal.
    if (g_remix.ready && g_gameDataReady) {
        WeatherBridge::PushOncePerFrame();
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

    // Phase 1B: tick the semantic-capture resolve loop + TTL sweep.
    // Called from hkPresent (game thread) so we have the D3D11 device for
    // texture readbacks. Mirrors Skyrim's EndFrame call site exactly.
    // Internally rate-limits resolve work; resolver cost is bounded per call.
    if (g_remix.ready && g_gameDataReady) {
        ID3D11Device* semanticDevice = nullptr;
        swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&semanticDevice);
        if (semanticDevice) {
            SemanticCapture::Tick(semanticDevice);
            semanticDevice->Release();
        }
    }

    // Phase 1B: cell orchestration retired. Geometry capture is now event-driven
    // via semantic_capture (BSLightingShaderProperty render-pass hook). Terrain
    // and lights regress until later phases revive them on event-driven paths.

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

    {
        MH_STATUS mhInit = MH_Initialize();
        if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
            _MESSAGE("FO4RemixPlugin: ERROR - MH_Initialize failed (%d)", (int)mhInit);
            return false;
        }
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

    // Reset UI RT detection — RT resource may change after load
    if (g_ui.renderTarget) {
        g_ui.renderTarget->Release();
        g_ui.renderTarget = nullptr;
    }
    g_ui.locked = false;
    g_ui.clearedThisFrame = false;
    g_ui.clearCandidateCount = 0;
    g_ui.drawnThisFrame = false;
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
