#include "present_hook.h"
#include "bs_extraction.h"
#include "config.h"
#include "draw_capture.h"
#include "raster_suppress.h"
#include "fo4_diagnostics.h"
#include "remix_api.h"
#include "remix_renderer.h"
#include "camera.h"
#include "semantic_capture.h"
#include "weather_bridge.h"
#include "window_manager.h"
#include "fo4_tracy.h"

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_4.h>   // IDXGIAdapter3::QueryVideoMemoryInfo ([VRAM] log)
#include <MinHook.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>
#include <algorithm>
#include <timeapi.h>   // timeBeginPeriod/timeEndPeriod (links winmm)

// Latest process-local adapter memory reading, published by the [VRAM]
// telemetry block in HookedPresent (~every 60 frames) and consumed by
// SemanticCapture::Tick's pressure force-eviction via
// PresentHook::GetVramBudgetSnapshot (2026-07-20). Zero until first query.
static std::atomic<uint64_t> g_vramUsedMiB{0};
static std::atomic<uint64_t> g_vramBudgetMiB{0};

bool PresentHook::GetVramBudgetSnapshot(uint64_t* usedMiB, uint64_t* budgetMiB) {
    const uint64_t used   = g_vramUsedMiB.load(std::memory_order_relaxed);
    const uint64_t budget = g_vramBudgetMiB.load(std::memory_order_relaxed);
    if (usedMiB)   *usedMiB   = used;
    if (budgetMiB) *budgetMiB = budget;
    return budget != 0;
}

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
    // Map-first capture (2026-07-20): true when stagingTex holds a queued
    // CopyResource from a previous capture frame that has not been read yet.
    // The read maps with DO_NOT_WAIT one-plus frames later, so the game
    // thread never blocks on the GPU pipeline (the old copy-then-map-now
    // pattern stalled the game thread for a full GPU frame every UI frame).
    bool                     stagingPending = false;

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

    // STICKY bound state (2026-07-11 save-picker freeze fix). drawnThisFrame
    // only fires on OMSetRenderTargets EVENTS; in pure-UI modes (main-menu
    // popups) the engine can leave the UI RT bound across many frames with
    // no rebind, so event-only tracking read as "not drawn" and the capture
    // below froze on the last frame before the popup. Set on a matching
    // bind, cleared on any other bind. Same-render-thread ordering.
    std::atomic<bool>        currentlyBound { false };

    // Liveness stamp (present index of the last matching bind/clear) driving
    // the stale-lock auto-unlock: if the engine switches or recreates its
    // interface RT (load transitions), a permanent lock would freeze the
    // overlay forever AND -- under raster suppression -- swallow every draw
    // to the NEW UI RT. After kUiStaleUnlockFrames without activity the lock
    // drops, suppression goes dormant (RasterSuppress::NotifyUiRT(null)),
    // and detection re-runs to find the current RT.
    std::atomic<uint64_t>    lastActivityPresent { 0 };

    static constexpr int     kMaxClearCandidates = 8;
    ID3D11Texture2D*         clearCandidates[kMaxClearCandidates] = {};
    int                      clearCandidateCount = 0;
    int                      presentCallCount = 0;
} g_ui;

// ---------------------------------------------------------------------------
// Multi-layer UI capture (2026-07-18). FO4's Scaleform UI is NOT one
// surface: the HUD/interface RT, the main menu, the Pip-Boy, and terminals
// each composite into their OWN render target, so the single locked-RT
// capture only ever shipped the HUD layer (user: "only VATS and the main
// game ui work; pause menu partially"). Every Scaleform target shares one
// fingerprint -- an R8G8B8A8 DEFAULT-usage texture cleared to TRANSPARENT
// BLACK at the start of its UI pass, then sole-bound and drawn. Track the
// frame's ordered set of such layers (clear order == Scaleform render
// order, bottom-most first); the Present capture composites them all into
// the one overlay image DrawScreenOverlay ships. All state is owned by the
// game render thread like g_ui above.
// ---------------------------------------------------------------------------
static struct MultiLayerUI {
    static constexpr int kMaxLayers = 8;
    struct Layer {
        ID3D11Texture2D* tex = nullptr;   // AddRef'd for the frame
        uint32_t         w = 0, h = 0;
        bool             bound = false;   // sole-bound at least once this frame
    };
    Layer layers[kMaxLayers];
    int   count = 0;

    // Per-SOURCE staging pool (2026-07-20: keyed by source texture identity,
    // not size — the map-first capture holds one queued copy per source
    // across frames, and two same-sized layers, e.g. HUD + a screen-sized
    // menu, must not overwrite each other's pending copy). `src` is an
    // identity compare only, no ref held; a recycled pointer at worst reads
    // one stale frame before the next copy refreshes it.
    struct Staging {
        ID3D11Texture2D* tex = nullptr;
        ID3D11Texture2D* src = nullptr;
        uint32_t         w = 0, h = 0;
        bool             pendingCopy = false;  // a queued CopyResource awaits mapping
        uint64_t         lastUse = 0;          // present index, for eviction
    };
    Staging stagings[kMaxLayers];

    // Pip-Boy feed supply state (map-first): a feed copy was queued and
    // should be mapped + supplied on a following present.
    ID3D11Texture2D* feedSupplySrc = nullptr;
    uint32_t         feedSupplyW = 0, feedSupplyH = 0;

    // Screen-sized premultiplied composite scratch.
    std::vector<uint8_t> composite;

    // Pip-Boy screen feed source (2026-07-18 v2): identity of the layer
    // whose pixels are being routed onto the Screen:0 mesh (see
    // SemanticCapture::SupplyPipboyScreenFeed). Identity compare only --
    // no ref held. Cleared on the load reset with the rest of the state.
    ID3D11Texture2D* pipboyFeedLayer = nullptr;

    // Diag: signature of the last logged layer set ([UILayer] on change).
    uint64_t lastLogSig = 0;
    int      logCount = 0;

    void ReleaseFrameLayers() {
        for (int i = 0; i < count; ++i) {
            if (layers[i].tex) layers[i].tex->Release();
            layers[i] = {};
        }
        count = 0;
    }
    void ReleaseStagings() {
        for (auto& s : stagings) {
            if (s.tex) s.tex->Release();
            s = {};
        }
    }
} g_uiLayers;

// Fetch (or create) the staging slot for a source layer texture. Keyed by
// source identity; a size change for the same source recreates the staging
// (and drops any pending copy). When the pool is full the least-recently
// used slot is evicted. Render thread only.
static MultiLayerUI::Staging* GetLayerStaging(ID3D11Device* device,
                                              ID3D11Texture2D* src,
                                              uint32_t w, uint32_t h,
                                              uint64_t presentIdx) {
    int freeSlot = -1;
    int lruSlot = -1;
    uint64_t lruStamp = UINT64_MAX;
    MultiLayerUI::Staging* found = nullptr;
    for (int i = 0; i < MultiLayerUI::kMaxLayers; ++i) {
        auto& s = g_uiLayers.stagings[i];
        if (s.tex && s.src == src) { found = &s; break; }
        if (!s.tex && freeSlot < 0) freeSlot = i;
        if (s.tex && s.lastUse < lruStamp) { lruStamp = s.lastUse; lruSlot = i; }
    }
    if (found) {
        if (found->w == w && found->h == h) {
            found->lastUse = presentIdx;
            return found;
        }
        found->tex->Release();
        *found = {};                        // size changed: rebuild below
    }
    MultiLayerUI::Staging* slot = found;
    if (!slot) {
        if (freeSlot >= 0) {
            slot = &g_uiLayers.stagings[freeSlot];
        } else if (lruSlot >= 0) {
            slot = &g_uiLayers.stagings[lruSlot];
            slot->tex->Release();
            *slot = {};
        } else {
            return nullptr;
        }
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &tex))) return nullptr;
    slot->tex = tex;
    slot->src = src;
    slot->w = w;
    slot->h = h;
    slot->pendingCopy = false;
    slot->lastUse = presentIdx;
    return slot;
}

// Blend one mapped premultiplied-alpha layer into the screen-sized
// premultiplied composite. Screen-sized layers blend 1:1; other sizes
// (Pip-Boy 876x700, terminal screens) scale-to-fit inside maxFrac of the
// screen, centered, bilinear (premultiplied space is the correct domain
// for filtering). maxFrac is 0.82 for ordinary panels; the fed Pip-Boy
// layer passes a smaller configurable fraction (see
// overlayPipboyPanelFrac).
static void BlendLayerIntoComposite(const uint8_t* src, uint32_t srcPitch,
                                    uint32_t sw, uint32_t sh,
                                    uint8_t* dst, uint32_t dw, uint32_t dh,
                                    float maxFrac = 0.82f) {
    auto blendPx = [](uint8_t* d, const uint8_t* s) {
        const uint32_t a = s[3];
        if (a == 0) return;
        const uint32_t ia = 255u - a;
        d[0] = (uint8_t)(s[0] + (d[0] * ia + 127u) / 255u);
        d[1] = (uint8_t)(s[1] + (d[1] * ia + 127u) / 255u);
        d[2] = (uint8_t)(s[2] + (d[2] * ia + 127u) / 255u);
        d[3] = (uint8_t)(a + (d[3] * ia + 127u) / 255u);
    };
    if (sw == dw && sh == dh) {
        for (uint32_t y = 0; y < sh; ++y) {
            const uint8_t* srow = src + (size_t)y * srcPitch;
            uint8_t* drow = dst + (size_t)y * dw * 4;
            for (uint32_t x = 0; x < sw; ++x) {
                blendPx(drow + x * 4, srow + x * 4);
            }
        }
        return;
    }
    // Never upscale: a 876x700 Pip-Boy layer blown up to 82% of a 1440p+
    // screen was "floating and way too large" (2026-07-18 verify) -- native
    // size reads correctly; only oversized layers shrink to fit.
    const float fit =
        (std::min)(1.0f, (std::min)(maxFrac * dw / sw, maxFrac * dh / sh));
    const uint32_t tw = (std::max)(1u, (uint32_t)(sw * fit));
    const uint32_t th = (std::max)(1u, (uint32_t)(sh * fit));
    const uint32_t ox = (dw - tw) / 2;
    const uint32_t oy = (dh - th) / 2;
    for (uint32_t y = 0; y < th; ++y) {
        const float syf = (y + 0.5f) * sh / th - 0.5f;
        const int32_t sy0 = (std::max)(0, (int32_t)syf);
        const int32_t sy1 = (std::min)((int32_t)sh - 1, sy0 + 1);
        const float fy = syf - sy0;
        const uint8_t* r0 = src + (size_t)sy0 * srcPitch;
        const uint8_t* r1 = src + (size_t)sy1 * srcPitch;
        uint8_t* drow = dst + ((size_t)(oy + y) * dw + ox) * 4;
        for (uint32_t x = 0; x < tw; ++x) {
            const float sxf = (x + 0.5f) * sw / tw - 0.5f;
            const int32_t sx0 = (std::max)(0, (int32_t)sxf);
            const int32_t sx1 = (std::min)((int32_t)sw - 1, sx0 + 1);
            const float fx = sxf - sx0;
            uint8_t px[4];
            for (int c = 0; c < 4; ++c) {
                const float top = r0[sx0 * 4 + c] * (1.0f - fx) + r0[sx1 * 4 + c] * fx;
                const float bot = r1[sx0 * 4 + c] * (1.0f - fx) + r1[sx1 * 4 + c] * fx;
                px[c] = (uint8_t)(top * (1.0f - fy) + bot * fy + 0.5f);
            }
            blendPx(drow + x * 4, px);
        }
    }
}

// Present-call index for the UI liveness stamps above (game render thread).
static std::atomic<uint64_t> g_presentIndex{0};
// PreLoadGame reset request (F4SE messaging thread -> render thread).
// The g_ui fields (renderTarget COM pointer, locked, candidate list) are
// owned by the render thread: hkClearRenderTargetView / hkOMSetRenderTargets
// / the capture block all read them with no lock, so the reset must run ON
// that thread (top of hkPresent), not on the message thread -- a cross-
// thread Release()+null raced the in-flight frame's GetDesc/CopyResource
// (use-after-free on every unlucky save load).
static std::atomic<bool> g_uiResetRequested{false};
// ~5s at 60fps. Loading screens keep their tips/spinner UI on the same RT,
// so a live RT never goes quiet this long; a switched/dead RT does.
static constexpr uint64_t kUiStaleUnlockFrames = 300;


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
                // Multi-layer UI tracking: a transparent-black clear of an
                // R8G8B8A8 DEFAULT-usage surface is the start-of-pass
                // signature of EVERY Scaleform target (interface RT, main
                // menu, Pip-Boy 876x700, terminals). Record the frame's
                // layers in clear order for the Present composite. Size
                // floor rejects small FX scratch buffers.
                if (g_config.overlayMultiLayer &&
                    g_uiLayers.count < MultiLayerUI::kMaxLayers) {
                    bool known = false;
                    for (int i = 0; i < g_uiLayers.count; ++i) {
                        if (g_uiLayers.layers[i].tex == tex) { known = true; break; }
                    }
                    if (!known) {
                        D3D11_TEXTURE2D_DESC ld;
                        tex->GetDesc(&ld);
                        if (ld.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                            ld.Usage == D3D11_USAGE_DEFAULT &&
                            ld.SampleDesc.Count == 1 &&
                            ld.Width >= 256 && ld.Height >= 256 &&
                            ld.Width <= 4096 && ld.Height <= 4096) {
                            auto& L = g_uiLayers.layers[g_uiLayers.count++];
                            L.tex = tex;
                            L.tex->AddRef();
                            L.w = ld.Width;
                            L.h = ld.Height;
                            L.bound = false;
                        }
                    }
                }
                if (g_ui.locked) {
                    if (tex == g_ui.renderTarget) {
                        g_ui.clearedThisFrame = true;
                        g_ui.lastActivityPresent.store(
                            g_presentIndex.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
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
    // Raster suppression tracks whether the UI RT is the CURRENT color
    // target. The UI is always sole-bound (the detection below depends on
    // it), so any other bind -- MRT, depth-only, none -- flips this false.
    bool uiBoundNow = false;
    bool uiLayerBound = false;
    if (numViews == 1 && ppRTVs && ppRTVs[0]) {
        ID3D11Resource* resource = nullptr;
        ppRTVs[0]->GetResource(&resource);
        if (resource) {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) {
                // Multi-layer UI tracking: mark recorded layers that
                // actually get bound (draw evidence for the composite), and
                // treat the bind as a UI-phase opener -- menus whose draws
                // never touch the locked RT (main menu, Pip-Boy, terminals)
                // must forward under raster suppression too.
                if (g_config.overlayMultiLayer) {
                    for (int i = 0; i < g_uiLayers.count; ++i) {
                        if (g_uiLayers.layers[i].tex == tex) {
                            g_uiLayers.layers[i].bound = true;
                            uiLayerBound = true;
                            break;
                        }
                    }
                }
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
                        g_ui.lastActivityPresent.store(
                            g_presentIndex.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
                        uiBoundNow = true;
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
                                RasterSuppress::NotifyUiRT(g_ui.renderTarget);
                            }
                            g_ui.clearedThisFrame = true;
                            g_ui.drawnThisFrame = true;
                            g_ui.lastActivityPresent.store(
                                g_presentIndex.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
                            uiBoundNow = true;
                            break;
                        }
                    }
                }
                tex->Release();
            }
            resource->Release();
        }
    }
    g_ui.currentlyBound.store(uiBoundNow, std::memory_order_relaxed);
    RasterSuppress::NotifyUiTargetBound(uiBoundNow || uiLayerBound);
    g_originalOMSetRTs(context, numViews, ppRTVs, pDSV);
}

static void RemixThreadFunc() {
    _MESSAGE("FO4RemixPlugin: Remix thread started");
    FO4_TRACY_SET_THREAD_NAME("RemixThread");

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

    // Window manager: overlay glue + menu interactivity + hotkey. Runs on
    // this thread (it owns the Remix window and its message pump); installed
    // after device creation so its subclass wraps the runtime's WndProc.
    WindowManager::Init(RemixAPI::GetRemixWindow(), g_remix.gameHwnd);

    g_remix.ready = true;

    // 1ms Sleep granularity for the frame pacing below. Without this, Sleep
    // rounds up to the default 15.6ms timer tick (often two ticks), which is
    // exactly how the old flat Sleep(16) added ~16-31ms to every frame.
    // Matched by timeEndPeriod after the loop.
    timeBeginPeriod(1);

    using PaceClock = std::chrono::steady_clock;

    // Persistent HUD overlay (2026-07-09 flicker fix). The runtime composites
    // the overlay only on frames that SUBMIT one -- dispatchScreenOverlay
    // consumes m_pendingScreenOverlay per dispatch (rtx_fork_overlay.cpp) --
    // while the game produces captures at game fps and this thread renders at
    // RemixMaxFPS. Passing only FRESH captures through left every in-between
    // Remix frame HUD-less: a beat-frequency flicker between the two loops,
    // and a total HUD loss when the game window was unfocused (alt-tab to the
    // Remix window stops the game's Presents -> no captures at all). Keep the
    // last capture across iterations and resubmit it every frame; a fresh
    // capture replaces it via buffer swap (no extra copies).
    OverlayData lastOverlay;

    while (g_remix.running) {
        const PaceClock::time_point frameStart = PaceClock::now();

        // Grab latest camera from main thread
        CameraState cam;
        {
            std::lock_guard<std::mutex> lock(g_remix.cameraMutex);
            cam = g_remix.sharedCamera;
        }

        // Swap in the latest overlay capture from the game thread, if any.
        if (g_overlay.ready) {
            std::lock_guard<std::mutex> lock(g_overlay.mutex);
            lastOverlay.pixels.swap(g_overlay.pixels);
            lastOverlay.width = g_overlay.width;
            lastOverlay.height = g_overlay.height;
            lastOverlay.dxgiFormat = static_cast<uint32_t>(g_overlay.dxgiFormat);
            lastOverlay.valid = true;
            g_overlay.ready = false;
        }
        const OverlayData& overlay = lastOverlay;

        // Pump messages before rendering so input is processed even when frames are slow
        PumpMessages();

        // Top-level backstop (2026-07-09): every remixapi call inside OnFrame
        // is individually guarded (RemixCallGuarded -- the July-8/9 airport
        // crashes were an uncaught dxvk std::out_of_range terminating the
        // process from this thread), but a C++ exception from anywhere else
        // in the frame body would still land on this noexcept thread proc and
        // terminate. Skip the frame instead: state is per-frame rebuilt, so
        // the next iteration recovers.
        try {
            RemixRenderer::OnFrame(cam, overlay);
        } catch (const std::exception& e) {
            static std::atomic<int> sOnFrameCatch{0};
            const int n = sOnFrameCatch.fetch_add(1, std::memory_order_relaxed);
            if (n < 16) {
                _MESSAGE("FO4RemixPlugin: [RemixThread] OnFrame C++ exception #%d "
                         "what=%s -- frame skipped", n, e.what());
            }
        } catch (...) {
            static std::atomic<int> sOnFrameCatchU{0};
            const int n = sOnFrameCatchU.fetch_add(1, std::memory_order_relaxed);
            if (n < 16) {
                _MESSAGE("FO4RemixPlugin: [RemixThread] OnFrame unknown C++ "
                         "exception #%d -- frame skipped", n);
            }
        }

        // Pump again after rendering
        PumpMessages();

        // Overlay glue + menu interactivity sync + hotkey (cheap: a few
        // win32 reads per frame; SetWindowPos only when the rect changed).
        WindowManager::Tick(RemixAPI::GetInterface());

        FO4_TRACY_FRAME_MARK();

        // Pace to RemixMaxFPS by sleeping only the UNUSED remainder of the
        // frame budget. The previous flat Sleep(16) added a full budget on
        // top of every frame's render time (16ms render + 16ms sleep = 31fps,
        // not the intended "60fps cap"). RemixMaxFPS=0 = uncapped; Sleep(0)
        // still yields the timeslice so a trivial frame (menu, empty scene)
        // doesn't monopolize a core.
        if (g_config.remixMaxFPS > 0) {
            const auto budget = std::chrono::microseconds(1000000u / g_config.remixMaxFPS);
            const auto elapsed = PaceClock::now() - frameStart;
            if (elapsed < budget) {
                std::this_thread::sleep_for(budget - elapsed);
            } else {
                Sleep(0);
            }
        } else {
            Sleep(0);
        }
    }
    timeEndPeriod(1);

    _MESSAGE("FO4RemixPlugin: Remix thread shutting down");
    WindowManager::Shutdown();
    RemixRenderer::Shutdown();
    RemixAPI::Shutdown();
}

static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    FO4_TRACY_SCOPE("hkPresent");
    const uint64_t frameIndex = Diagnostics::Tick();
    const uint64_t presentIdx =
        g_presentIndex.fetch_add(1, std::memory_order_relaxed) + 1;
    // Frame boundary for raster suppression's UI-phase tracking: the phase
    // resets here unless the UI RT is still bound (sticky pure-UI menus).
    RasterSuppress::NotifyFrameEnd();
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
    // Paused while the Remix dev menu is open (2026-07-18): the menu owns
    // input then, and re-claiming raw input mid-menu would stomp whatever
    // the runtime's input path registered. Resumes the moment it closes.
    if (g_remix.ready && g_remix.gameHwnd && !WindowManager::IsMenuOpen()) {
        RemixAPI::RebindRawInputToGameWindow(g_remix.gameHwnd);
    }

    g_ui.presentCallCount++;

    // Deferred PreLoadGame reset (see g_uiResetRequested): drop the UI RT
    // detection state on the thread that owns it. Suppression is re-armed
    // dormant exactly like the stale-unlock path below -- the old reset
    // left RasterSuppress pointed at the dead RT, which swallowed every
    // load-screen UI draw until re-detection happened to match.
    if (g_uiResetRequested.exchange(false, std::memory_order_acq_rel)) {
        _MESSAGE("FO4RemixPlugin: [UIRT] reset (save game load): re-detecting");
        RasterSuppress::NotifyUiRT(nullptr);
        RasterSuppress::NotifyUiTargetBound(false);
        // Multi-layer UI: the load tears down/recreates the engine's UI
        // surfaces; drop the frame layers AND the size-keyed staging pool.
        g_uiLayers.ReleaseFrameLayers();
        g_uiLayers.ReleaseStagings();
        g_uiLayers.pipboyFeedLayer = nullptr;
        g_uiLayers.feedSupplySrc = nullptr;
        g_overlay.stagingPending = false;
        BsExtraction::ClearLiveRTScreenSources();
        if (g_ui.renderTarget) {
            g_ui.renderTarget->Release();
            g_ui.renderTarget = nullptr;
        }
        g_ui.locked = false;
        g_ui.currentlyBound.store(false, std::memory_order_relaxed);
        g_ui.clearCandidateCount = 0;
        g_ui.clearedThisFrame = false;
        g_ui.drawnThisFrame = false;
        g_ui.lastActivityPresent.store(presentIdx, std::memory_order_relaxed);
    }

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

    // Push time-of-day to Remix's atmosphere config keys. Game-data gate is
    // load-bearing: LookupFormByID returns null until the form DB is ready.
    // Same gate as the semantic-capture tick below.
    if (g_remix.ready && g_gameDataReady) {
        WeatherBridge::PushOncePerFrame();
    }

    // Hook OMSetRenderTargets + ClearRenderTargetView on first opportunity.
    // Only when a consumer exists: the HUD overlay needs them to find the UI
    // render target for capture, and raster suppression needs the same
    // detection (plus per-bind tracking) to keep UI draws executing. With
    // neither feature on, hkOMSetRenderTargets on every bind is pure overhead.
    if (!g_ui.contextHooked &&
        (g_config.hudOverlayEnabled || g_config.suppressGameRaster)) {
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

    // Merge-instanced draw capture: hook DrawIndexedInstanced so the static
    // resolver can reuse the engine's own per-sub-model draw parameters for
    // BSMergeInstancedTriShape partitioning (see draw_capture.h). Idle cost
    // once hooked is one relaxed atomic load per draw call. Raster
    // suppression rides the same context hooks (its gates live inside the
    // draw detours), so it also forces the install.
    if ((g_config.mergeInstanceExpansion && g_config.mergeInstanceDrawCapture) ||
        g_config.suppressGameRaster) {
        if (!DrawCapture::Hooked()) {
            ID3D11Device* dcDev = nullptr;
            swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&dcDev);
            if (dcDev) {
                ID3D11DeviceContext* dcCtx = nullptr;
                dcDev->GetImmediateContext(&dcCtx);
                if (dcCtx) {
                    DrawCapture::InstallHook(dcCtx);
                    dcCtx->Release();
                }
                dcDev->Release();
            }
        }
        DrawCapture::OnPresent();
    }

    // Stale-lock auto-unlock (2026-07-11 save-picker freeze). If the engine
    // switches or recreates its interface RT (load transitions do), the
    // locked pointer goes quiet: no matching binds, no matching clears. A
    // permanent lock then freezes the overlay on its last captured frame
    // forever -- and under raster suppression ALSO swallows every draw to
    // the new UI RT, so the real UI never even rasterizes. Drop the lock,
    // put suppression dormant (all draws forward while unlocked), and let
    // the detection phase re-run; it re-locks within a few frames of the
    // new RT's clear+sole-bind pattern.
    if (g_ui.locked &&
        presentIdx - g_ui.lastActivityPresent.load(std::memory_order_relaxed) >
            kUiStaleUnlockFrames) {
        _MESSAGE("FO4RemixPlugin: [UIRT] locked RT %p quiet for %llu presents -- "
                 "unlocking, re-detecting (suppression dormant meanwhile)",
                 g_ui.renderTarget,
                 (unsigned long long)kUiStaleUnlockFrames);
        RasterSuppress::NotifyUiRT(nullptr);
        RasterSuppress::NotifyUiTargetBound(false);
        if (g_ui.renderTarget) g_ui.renderTarget->Release();
        g_ui.renderTarget = nullptr;
        g_ui.locked = false;
        g_ui.currentlyBound.store(false, std::memory_order_relaxed);
        g_ui.clearCandidateCount = 0;   // stale pointers must not false-match
        g_ui.clearedThisFrame = false;
        g_ui.drawnThisFrame = false;
        g_ui.lastActivityPresent.store(presentIdx, std::memory_order_relaxed);
    }

    // Capture UI render target for screen overlay (isolated UI without 3D scene).
    // Require the RT to be zeroed this frame (ClearRTV or our force-clear --
    // stale accumulated content would ghost) AND either a bind EVENT
    // (drawnThisFrame) or a STICKY bind (currentlyBound): pure-UI modes such
    // as main-menu popups leave the UI RT bound across frames with no
    // OMSetRenderTargets traffic, and requiring a per-frame bind event froze
    // the capture on the popup's first frame.
    bool uiCleared = g_ui.clearedThisFrame.exchange(false);
    bool uiDrawn = g_ui.drawnThisFrame.exchange(false);
    bool uiActiveThisFrame =
        uiCleared && (uiDrawn || g_ui.currentlyBound.load(std::memory_order_relaxed));

    // ---- Multi-layer UI capture (2026-07-18, reworked same day) ----
    // Composite = recorded Scaleform layers (clear order, bottom-most
    // first) with the LOCKED interface RT blended LAST on top. The locked
    // RT must NOT come from the recorded-layer list: the engine frequently
    // skips its transparent clear (that is why the legacy path force-
    // clears it on bind), so clear-event recording misses it -- v1 of this
    // block dropped the entire in-game HUD whenever any other layer was
    // present, and with no captures replacing the overlay the loading
    // screen stuck on screen forever. The locked RT rides its own
    // uiActiveThisFrame machinery (force-clear + sticky bind) instead.
    bool multiLayerCaptured = false;
    const bool lockedRtActive =
        g_ui.renderTarget != nullptr && uiActiveThisFrame;
    // Composite only when a non-interface layer is actually in play (menu /
    // Pip-Boy / terminal / load screen). Plain HUD frames -- the vast
    // majority -- keep the legacy single-RT path below and its exact perf
    // profile (no zero-fill + blend + re-un-premultiply passes).
    // ---- Pip-Boy screen feed capture (2026-07-18 v2) ----
    // While the 1P Pip-Boy screen mesh is live (SemanticCapture gate:
    // Screen:0 registered + submitted + firing, viewmodel active), route
    // the Pip-Boy Scaleform layer's pixels onto the MESH instead of the
    // full-screen composite. Candidate = a bound panel layer that is
    // neither the interface RT nor screen-sized (876x700 in the field
    // log). Captured every ScreenRefreshFrames (and immediately when the
    // layer identity changes -- menu re-open can recreate the RT).
    const bool pipFeedWanted =
        g_config.overlayMultiLayer && g_config.pipboyScreenFeed &&
        g_remix.ready && SemanticCapture::PipboyScreenFeedWanted();
    if (pipFeedWanted) {
        const MultiLayerUI::Layer* cand = nullptr;
        for (int i = 0; i < g_uiLayers.count; ++i) {
            const auto& L = g_uiLayers.layers[i];
            if (!L.tex || !L.bound) continue;
            if (L.tex == g_ui.renderTarget) continue;
            if (L.w == g_remix.gameWidth && L.h == g_remix.gameHeight) continue;
            cand = &L;
            break;
        }
        if (cand) {
            const uint32_t period = g_config.viewModelScreenRefreshFrames
                ? g_config.viewModelScreenRefreshFrames : 12;
            // Map-first (2026-07-20): when a refresh is due, QUEUE the copy
            // and map it on a later present with DO_NOT_WAIT — the old
            // copy-then-map-now pattern stalled the game thread for a full
            // GPU frame on every refresh. Feed latency becomes period+1
            // frames instead of period; no pipeline stall.
            const bool due = cand->tex != g_uiLayers.pipboyFeedLayer ||
                             (presentIdx % period) == 0;
            const bool supplyPending = g_uiLayers.feedSupplySrc != nullptr;
            if (due || supplyPending) {
                ID3D11Device* fdev = nullptr;
                swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&fdev);
                if (fdev) {
                    ID3D11DeviceContext* fctx = nullptr;
                    fdev->GetImmediateContext(&fctx);
                    if (fctx) {
                        MultiLayerUI::Staging* st =
                            GetLayerStaging(fdev, cand->tex, cand->w, cand->h,
                                            presentIdx);
                        if (st) {
                            if (supplyPending &&
                                st->src == g_uiLayers.feedSupplySrc &&
                                st->pendingCopy) {
                                D3D11_MAPPED_SUBRESOURCE mapped;
                                if (SUCCEEDED(fctx->Map(st->tex, 0,
                                        D3D11_MAP_READ,
                                        D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
                                    SemanticCapture::SupplyPipboyScreenFeed(
                                        static_cast<const uint8_t*>(mapped.pData),
                                        st->w, st->h, mapped.RowPitch);
                                    fctx->Unmap(st->tex, 0);
                                    st->pendingCopy = false;
                                    g_uiLayers.feedSupplySrc = nullptr;
                                    g_uiLayers.pipboyFeedLayer = cand->tex;
                                }
                                // WAS_STILL_DRAWING: retry next present.
                            } else if (supplyPending) {
                                // Candidate changed or slot recycled — drop.
                                g_uiLayers.feedSupplySrc = nullptr;
                            }
                            if (due) {
                                fctx->CopyResource(st->tex, cand->tex);
                                st->pendingCopy = true;
                                g_uiLayers.feedSupplySrc = cand->tex;
                                g_uiLayers.feedSupplyW = st->w;
                                g_uiLayers.feedSupplyH = st->h;
                            }
                        }
                        fctx->Release();
                    }
                    fdev->Release();
                }
            }
        }
    }

    // Per-layer composite disposition:
    //   0 = composite normally (0.82 panel fit / 1:1 screen-sized)
    //   1 = composite as a REDUCED readable panel (the fed Pip-Boy layer;
    //       the physical screen is ~100px tall at camera distance because
    //       the viewmodel anchor cancels the engine's pipboy camera zoom,
    //       so mesh-only presentation was invisible in the field test)
    //   2 = exclude entirely -- presents on a mesh: RT-backed MATERIAL
    //       sources (0c0c9e7 scopes/RT screens), or the fed layer when
    //       PipboyPanelHeightFrac=0. Excluded layers stay recorded/bound-
    //       tracked: their Scaleform draws must keep executing under raster
    //       suppression or the mesh capture goes stale. When the feed is
    //       off or the mesh is gone (Power Armor, terminals), everything
    //       reverts to disposition 0 so the UI is never presented nowhere.
    auto layerDisposition = [&](ID3D11Texture2D* tex) -> int {
        if (g_config.viewModelScreenRefreshFrames != 0 &&
            BsExtraction::IsLiveRTScreenSource(tex)) return 2;
        if (pipFeedWanted && tex == g_uiLayers.pipboyFeedLayer) {
            return g_config.overlayPipboyPanelFrac > 0.0f ? 1 : 2;
        }
        return 0;
    };
    int otherBoundLayers = 0;
    for (int i = 0; i < g_uiLayers.count; ++i) {
        const auto& L = g_uiLayers.layers[i];
        if (L.tex && L.bound && L.tex != g_ui.renderTarget &&
            layerDisposition(L.tex) != 2) ++otherBoundLayers;
    }
    // Diag: log the layer set when it changes ([UILayer], capped). Outside
    // the composite gate on purpose: when every non-interface layer is a
    // live-screen exclusion (Pip-Boy raised, nothing else) the composite
    // never engages, and these lines are the only field evidence the
    // exclusion actually matched (live=1).
    if (g_config.overlayMultiLayer && g_uiLayers.count > 0) {
        uint64_t sig = 0x9E3779B97F4A7C15ull * (uint64_t)(g_uiLayers.count + 1);
        for (int i = 0; i < g_uiLayers.count; ++i) {
            sig ^= (uint64_t)(uintptr_t)g_uiLayers.layers[i].tex +
                   (g_uiLayers.layers[i].bound ? 1u : 0u) + i * 0x1000193u;
        }
        if (sig != g_uiLayers.lastLogSig && g_uiLayers.logCount < 60) {
            g_uiLayers.lastLogSig = sig;
            ++g_uiLayers.logCount;
            for (int i = 0; i < g_uiLayers.count; ++i) {
                const auto& L = g_uiLayers.layers[i];
                _MESSAGE("FO4RemixPlugin: [UILayer] #%d tex=%p %ux%u bound=%d disp=%d",
                         i, (void*)L.tex, L.w, L.h, L.bound ? 1 : 0,
                         layerDisposition(L.tex));
            }
        }
    }
    if (g_config.hudOverlayEnabled && g_config.overlayMultiLayer &&
        g_remix.ready && otherBoundLayers > 0) {
        ID3D11Device* device = nullptr;
        swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&device);
        if (device) {
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);
            if (context) {
                const uint32_t dw = g_remix.gameWidth;
                const uint32_t dh = g_remix.gameHeight;
                // Map-first capture (2026-07-20): each layer's staging holds
                // the copy queued LAST frame; map it with DO_NOT_WAIT and
                // queue this frame's copy afterwards, so the game thread
                // never drains the GPU pipeline (the old per-layer
                // copy-then-map pattern cost one full GPU sync PER LAYER
                // per frame). The composite only builds when EVERY layer
                // mapped — a partial set would drop a layer for a frame
                // (visible flicker); keeping the previous overlay one more
                // frame is invisible. Miss cases: a layer's first frame
                // (nothing queued yet) or the GPU still chewing last
                // frame's copy.
                struct CapLayer {
                    ID3D11Texture2D*         tex;
                    uint32_t                 w, h;
                    float                    maxFrac;
                    MultiLayerUI::Staging*   st;
                    bool                     mapped;
                    D3D11_MAPPED_SUBRESOURCE m;
                };
                CapLayer caps[MultiLayerUI::kMaxLayers + 1];
                int capCount = 0;
                for (int i = 0; i < g_uiLayers.count; ++i) {
                    const auto& L = g_uiLayers.layers[i];
                    if (!L.tex || !L.bound) continue;
                    if (L.tex == g_ui.renderTarget) continue;  // blended last below
                    const int disp = layerDisposition(L.tex);
                    if (disp == 2) continue;               // presents on its mesh
                    caps[capCount++] = { L.tex, L.w, L.h,
                                         disp == 1 ? g_config.overlayPipboyPanelFrac
                                                   : 0.82f,
                                         nullptr, false, {} };
                }
                // Interface RT on top: HUD, subtitles, notifications draw
                // over any panel layer (Pip-Boy, terminal).
                if (lockedRtActive) {
                    D3D11_TEXTURE2D_DESC ld;
                    g_ui.renderTarget->GetDesc(&ld);
                    caps[capCount++] = { g_ui.renderTarget, ld.Width, ld.Height,
                                         0.82f, nullptr, false, {} };
                }
                bool allMapped = capCount > 0;
                for (int i = 0; i < capCount; ++i) {
                    auto& c = caps[i];
                    c.st = GetLayerStaging(device, c.tex, c.w, c.h, presentIdx);
                    if (c.st && c.st->pendingCopy &&
                        SUCCEEDED(context->Map(c.st->tex, 0, D3D11_MAP_READ,
                                               D3D11_MAP_FLAG_DO_NOT_WAIT,
                                               &c.m))) {
                        c.mapped = true;
                    } else {
                        allMapped = false;
                    }
                }
                int blended = 0;
                auto& comp = g_uiLayers.composite;
                if (allMapped) {
                    comp.assign((size_t)dw * dh * 4, 0);
                    for (int i = 0; i < capCount; ++i) {
                        const auto& c = caps[i];
                        BlendLayerIntoComposite(
                            static_cast<const uint8_t*>(c.m.pData),
                            c.m.RowPitch, c.w, c.h, comp.data(), dw, dh,
                            c.maxFrac);
                        ++blended;
                    }
                }
                for (int i = 0; i < capCount; ++i) {
                    if (caps[i].mapped) context->Unmap(caps[i].st->tex, 0);
                }
                // Queue this frame's copies (also primes first-frame layers).
                for (int i = 0; i < capCount; ++i) {
                    if (caps[i].st) {
                        context->CopyResource(caps[i].st->tex, caps[i].tex);
                        caps[i].st->pendingCopy = true;
                    }
                }
                if (blended > 0) {
                    std::lock_guard<std::mutex> lock(g_overlay.mutex);
                    g_overlay.pixels.resize((size_t)dw * dh * 4);
                    const uint8_t* src = comp.data();
                    uint8_t* dst = g_overlay.pixels.data();
                    for (size_t px = 0; px < (size_t)dw * dh; ++px) {
                        const uint8_t r = src[px * 4 + 0];
                        const uint8_t g = src[px * 4 + 1];
                        const uint8_t b = src[px * 4 + 2];
                        const uint8_t a = src[px * 4 + 3];
                        if (a > 0 && a < 255) {
                            dst[px * 4 + 0] = (uint8_t)(std::min)(255u, (uint32_t)r * 255u / a);
                            dst[px * 4 + 1] = (uint8_t)(std::min)(255u, (uint32_t)g * 255u / a);
                            dst[px * 4 + 2] = (uint8_t)(std::min)(255u, (uint32_t)b * 255u / a);
                        } else {
                            dst[px * 4 + 0] = r;
                            dst[px * 4 + 1] = g;
                            dst[px * 4 + 2] = b;
                        }
                        dst[px * 4 + 3] = a;
                    }
                    g_overlay.width = dw;
                    g_overlay.height = dh;
                    g_overlay.dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
                    g_overlay.ready = true;
                    multiLayerCaptured = true;
                    // Same post-first-capture lock as the legacy path: the
                    // locked state arms the on-bind force-clear (the engine
                    // frequently skips the interface RT's clear) and the
                    // stale-unlock lifecycle.
                    if (!g_ui.locked && lockedRtActive) {
                        g_ui.locked = true;
                        _MESSAGE("FO4RemixPlugin: UI RT locked at %p (multi-layer)",
                                 g_ui.renderTarget);
                    }
                }
                context->Release();
            }
            device->Release();
        }
    }

    // Gate on hudOverlayEnabled: without it this block ran the full capture
    // every frame — CopyResource + Map(READ) on a just-copied staging texture
    // (a forced CPU<->GPU pipeline stall on the game's render thread) plus a
    // ~1M-pixel un-premultiply pass — only for OnFrame to discard the result
    // because submission is gated on the same flag.
    if (!multiLayerCaptured &&
        g_config.hudOverlayEnabled && g_remix.ready && g_ui.renderTarget && uiActiveThisFrame) {
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
                        g_overlay.stagingPending = false;
                        _MESSAGE("FO4RemixPlugin: Created UI overlay staging texture %ux%u fmt=%u",
                                 g_overlay.stagingWidth, g_overlay.stagingHeight, (unsigned)g_overlay.stagingFormat);
                    } else {
                        _MESSAGE("FO4RemixPlugin: Failed to create UI overlay staging texture (0x%08X)", hr);
                    }
                }

                if (g_overlay.stagingTex) {
                    // Map-first (2026-07-20): read the copy queued on the
                    // PREVIOUS capture frame with DO_NOT_WAIT — by now the
                    // GPU has long finished it, so this succeeds without
                    // the full-pipeline stall the old copy-then-map-now
                    // pattern forced every UI frame. The fresh copy is
                    // queued after the read (and on the priming frame,
                    // where there is nothing to read yet).
                    D3D11_MAPPED_SUBRESOURCE mapped;
                    HRESULT hr = g_overlay.stagingPending
                        ? context->Map(g_overlay.stagingTex, 0, D3D11_MAP_READ,
                                       D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)
                        : DXGI_ERROR_WAS_STILL_DRAWING;
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

                    // Queue this frame's copy for a later capture frame's
                    // non-blocking map (also primes the very first frame).
                    context->CopyResource(g_overlay.stagingTex, g_ui.renderTarget);
                    g_overlay.stagingPending = true;
                }
            }
            if (context) context->Release();
            device->Release();
        }
    }

    // Multi-layer UI: drop this frame's layer refs (recorded fresh each
    // frame by the ClearRTV hook; the staging pool persists).
    g_uiLayers.ReleaseFrameLayers();

    // Phase 1B: tick the semantic-capture resolve loop + TTL sweep.
    // Called from hkPresent (game thread) so we have the D3D11 device for
    // texture readbacks. Mirrors Skyrim's EndFrame call site exactly.
    // Internally rate-limits resolve work; resolver cost is bounded per call.
    if (g_remix.ready && g_gameDataReady) {
        FO4_TRACY_SCOPE("SemanticCapture::Tick");
        ID3D11Device* semanticDevice = nullptr;
        swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&semanticDevice);
        if (semanticDevice) {
            SemanticCapture::Tick(semanticDevice);
            semanticDevice->Release();
        }
    }

    // Phase 1B: cell orchestration retired. Geometry capture is now event-driven
    // via semantic_capture (BSLightingShaderProperty render-pass hook). Terrain
    // regresses until a later phase revives it on an event-driven path.

    // Placed lights (revived 2026-07-07; became visible the moment the
    // Vault-111 walls sealed and skybox leakage stopped lighting interiors).
    // Poll the loaded cells' LIGH refs here on the game thread -- every 60
    // frames, or immediately when the player's cell changes -- and queue a
    // full snapshot; RemixRenderer::OnFrame diffs and applies it on the
    // Remix thread. Extraction cost is a flat object-list walk per loaded
    // cell, trivial at this cadence.
    if (g_remix.ready && g_gameDataReady && g_config.lightsEnabled) {
        static uint32_t s_lightPollCounter = 60;
        static uintptr_t s_lastLightCell = 0;
        const uintptr_t cellNow = BsExtraction::GetPlayerCellPtr();
        ++s_lightPollCounter;
        if (cellNow && (s_lightPollCounter >= 60 || cellNow != s_lastLightCell)) {
            s_lightPollCounter = 0;
            s_lastLightCell = cellNow;
            std::vector<ExtractedLight> lights;
            for (const auto& ci : BsExtraction::GetLoadedCells()) {
                auto cellLights = BsExtraction::ExtractCellLights(ci.cellPtr);
                lights.insert(lights.end(), cellLights.begin(), cellLights.end());
            }
            RemixRenderer::QueueLights(std::move(lights));
        }
    }

    // [VRAM] telemetry: process-wide adapter usage (DXGI -- covers the game's
    // D3D11 AND dxvk-remix's Vulkan, same process) plus the raster-suppression
    // counters. This is the A/B instrument for the modded-texture budget
    // fight: with SuppressGameRaster on, "used" should fall well below
    // "budget" as WDDM demotes unreferenced game textures; suppressed/fwd
    // counters prove the kill switch is live.
    // 2026-07-20: the query now runs every ~60 frames (was 300, log-only) and
    // publishes the reading to g_vramUsed/BudgetMiB -- the pressure signal for
    // SemanticCapture::Tick's force-eviction (GetVramBudgetSnapshot). The log
    // line keeps its ~300-frame cadence.
    {
        static uint32_t s_vramQueryCounter = 0;
        static uint32_t s_vramLogCounter = 0;
        static IDXGIAdapter3* s_adapter3 = nullptr;
        static bool s_adapterTried = false;
        ++s_vramLogCounter;
        if (++s_vramQueryCounter >= 60) {
            s_vramQueryCounter = 0;
            if (!s_adapterTried) {
                s_adapterTried = true;
                ID3D11Device* dev = nullptr;
                swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&dev);
                if (dev) {
                    IDXGIDevice* dxgiDev = nullptr;
                    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) {
                        IDXGIAdapter* adapter = nullptr;
                        if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
                            adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&s_adapter3);
                            adapter->Release();
                        }
                        dxgiDev->Release();
                    }
                    dev->Release();
                }
                if (!s_adapter3) {
                    _MESSAGE("FO4RemixPlugin: [VRAM] IDXGIAdapter3 unavailable; no budget telemetry");
                }
            }
            if (s_adapter3) {
                DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
                if (SUCCEEDED(s_adapter3->QueryVideoMemoryInfo(
                        0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
                    g_vramUsedMiB.store(local.CurrentUsage >> 20,
                                        std::memory_order_relaxed);
                    g_vramBudgetMiB.store(local.Budget >> 20,
                                          std::memory_order_relaxed);
                    if (s_vramLogCounter >= 300) {
                        s_vramLogCounter = 0;
                        uint64_t suppressed = 0, fwdUi = 0, fwdQuery = 0;
                        RasterSuppress::ConsumeStats(&suppressed, &fwdUi, &fwdQuery);
                        _MESSAGE("FO4RemixPlugin: [VRAM] process local used=%llu MiB "
                                 "budget=%llu MiB | raster suppress=%d drawsSuppressed=%llu "
                                 "fwdUI=%llu fwdQuery=%llu (since last)",
                                 (unsigned long long)(local.CurrentUsage >> 20),
                                 (unsigned long long)(local.Budget >> 20),
                                 g_config.suppressGameRaster ? 1 : 0,
                                 (unsigned long long)suppressed,
                                 (unsigned long long)fwdUi,
                                 (unsigned long long)fwdQuery);
                    }
                }
            }
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
    // Called on the F4SE messaging (game) thread. The UI RT detection state
    // is render-thread-owned; request the reset and let hkPresent perform
    // it (see g_uiResetRequested).
    _MESSAGE("FO4RemixPlugin: Resetting extraction state (save game load)");
    g_uiResetRequested.store(true, std::memory_order_release);
}

void PresentHook::Uninstall() {
    BsExtraction::StopTextureConversionWorkers();
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
