#include "raster_suppress.h"
#include "config.h"

#include <atomic>

namespace RasterSuppress {

// All state is written on the game's render thread (OM binds, query scopes,
// draw calls are same-thread ordered -- the assumption DrawCapture's bind
// tracking already relies on); atomics make the reads safe from anywhere.
static std::atomic<ID3D11Texture2D*> g_uiRT{nullptr};
static std::atomic<bool>             g_uiBound{false};
// Frame's UI phase: set on the first UI-RT bind, cleared at Present unless
// the UI RT is still bound (sticky pure-UI menus). While true, everything
// forwards -- the engine's UI plumbing (glyph atlases, Scaleform filters,
// composite) runs through targets that are not the UI RT itself.
static std::atomic<bool>             g_uiPhase{false};
static std::atomic<int>              g_occlusionDepth{0};

static std::atomic<uint64_t> g_suppressed{0};
static std::atomic<uint64_t> g_forwardedUi{0};
static std::atomic<uint64_t> g_forwardedQuery{0};

void NotifyUiRT(ID3D11Texture2D* tex) {
    g_uiRT.store(tex, std::memory_order_release);
}

void NotifyUiTargetBound(bool uiBound) {
    g_uiBound.store(uiBound, std::memory_order_relaxed);
    if (uiBound) {
        g_uiPhase.store(true, std::memory_order_relaxed);
    }
}

void NotifyFrameEnd() {
    // Carry the phase across the boundary while the UI RT stays bound:
    // static popups/menus can go many frames without a single rebind, and
    // their glyph/filter updates must keep forwarding.
    g_uiPhase.store(g_uiBound.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
}

// Only occlusion queries open a forward-scope: their results are derived
// from draws we would otherwise swallow, and a 0-samples answer reads as
// "occluded" to the engine. Timestamp/event/statistics queries measure the
// timeline itself and stay accurate without any draws.
static bool IsOcclusionQuery(ID3D11Asynchronous* async) {
    if (!async) return false;
    ID3D11Query* q = nullptr;
    if (FAILED(async->QueryInterface(__uuidof(ID3D11Query), (void**)&q)) || !q) {
        return false;
    }
    D3D11_QUERY_DESC desc = {};
    q->GetDesc(&desc);
    q->Release();
    return desc.Query == D3D11_QUERY_OCCLUSION ||
           desc.Query == D3D11_QUERY_OCCLUSION_PREDICATE;
}

void NotifyQueryBegin(ID3D11Asynchronous* async) {
    if (!g_config.suppressGameRaster) return;
    if (IsOcclusionQuery(async)) {
        g_occlusionDepth.fetch_add(1, std::memory_order_relaxed);
    }
}

void NotifyQueryEnd(ID3D11Asynchronous* async) {
    if (!g_config.suppressGameRaster) return;
    if (IsOcclusionQuery(async)) {
        // Floor at zero: an End without a seen Begin (hook installed
        // mid-scope) must not wedge the counter negative.
        int cur = g_occlusionDepth.load(std::memory_order_relaxed);
        while (cur > 0 &&
               !g_occlusionDepth.compare_exchange_weak(
                   cur, cur - 1, std::memory_order_relaxed)) {
        }
    }
}

bool ShouldSuppress() {
    if (!g_config.suppressGameRaster) return false;
    // Dormant until the UI RT is known: suppressing before detection would
    // leave the player with no readable UI anywhere during boot, and the
    // detection itself needs a few drawn+cleared frames to lock.
    if (!g_uiRT.load(std::memory_order_acquire)) return false;
    // uiPhase subsumes uiBound (a UI bind opens the phase); checking the
    // phase alone keeps the per-draw fast path to two relaxed loads.
    if (g_uiPhase.load(std::memory_order_relaxed)) {
        g_forwardedUi.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (g_occlusionDepth.load(std::memory_order_relaxed) > 0) {
        g_forwardedQuery.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_suppressed.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ConsumeStats(uint64_t* suppressed, uint64_t* forwardedUi,
                  uint64_t* forwardedQuery) {
    if (suppressed)     *suppressed     = g_suppressed.exchange(0, std::memory_order_relaxed);
    if (forwardedUi)    *forwardedUi    = g_forwardedUi.exchange(0, std::memory_order_relaxed);
    if (forwardedQuery) *forwardedQuery = g_forwardedQuery.exchange(0, std::memory_order_relaxed);
}

}  // namespace RasterSuppress
