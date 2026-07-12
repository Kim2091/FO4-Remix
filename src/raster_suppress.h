#pragma once

#include <d3d11.h>
#include <cstdint>

// ---------------------------------------------------------------------------
// Game-raster suppression (2026-07-11).
//
// The game's own D3D11 rendering runs alongside the Remix path tracer even
// though its output is never what the player looks at. With texture mods
// installed (the "normal person's load order" target), the game's raster
// pass keeps every streamed texture referenced on the GPU timeline, so WDDM
// can never demote them -- game VRAM directly starves dxvk-remix's budget --
// and the rasterization itself competes for GPU time with path tracing.
//
// The fix suppresses draw execution at the D3D11 hook layer: the ENGINE
// still runs its full CPU render loop (it must -- see the dependency list),
// our detours observe every call as before, but scene draws are not
// forwarded to the driver. Textures the GPU never references become
// demotable, and the raster GPU cost disappears.
//
// What must keep working, and why this is the right layer:
//   - BSShaderProperty::GetRenderPasses detours: CPU pass building,
//     untouched by GPU execution.
//   - DrawCapture chunk captures: need the engine to ISSUE DrawIndexed with
//     the record SRVs bound. Suppressing at the hook's forward site (after
//     observation) preserves them exactly; patching the engine's render
//     loop would not.
//   - Texture readbacks (CopySubresourceRegion + Map): copies are never
//     suppressed, only draws.
//   - HUD overlay: draws whose render target is the detected UI RT are
//     FORWARDED, so the overlay capture keeps real content.
//   - Occlusion-query scopes (Begin/End of D3D11_QUERY_OCCLUSION[_PREDICATE]):
//     draws inside them are FORWARDED so any engine visibility feedback
//     stays truthful (the proxy geometry is a trivial GPU cost).
//
// Enabled by [Performance] SuppressGameRaster (default 0). Suppression stays
// dormant until the UI render target has been detected (present_hook's
// existing detection), so early boot/menu rendering is never affected.
// ---------------------------------------------------------------------------
namespace RasterSuppress {

// present_hook publishes the detected UI render target once detection locks
// (and again if it ever re-detects). Null = unknown; suppression is dormant.
void NotifyUiRT(ID3D11Texture2D* tex);

// present_hook's OMSetRenderTargets hook reports, on every bind, whether the
// UI RT is the current color target. Single-render-thread ordering is
// assumed, same as DrawCapture's bind tracking.
//
// The first uiBound=true of a frame also opens the frame's UI PHASE: from
// that point until the frame ends, EVERY draw is forwarded. Scaleform
// rasterizes glyphs, filters, and modal elements through intermediate
// render targets that are not the final UI RT (first field test: the
// main-menu save-picker never appeared because its new glyphs/elements
// were suppressed); the engine renders the scene strictly before the UI,
// so phase-forwarding executes all of that UI plumbing -- plus the final
// backbuffer composite, which keeps the game window's UI live -- while
// scene passes, shadows, and post (all pre-UI) stay suppressed.
void NotifyUiTargetBound(bool uiBound);

// present_hook calls this once per Present (frame boundary). The UI phase
// carries over into the next frame while the UI RT is STILL bound (pure-UI
// menus never rebind); otherwise it resets until the next UI bind.
void NotifyFrameEnd();

// draw_capture's Begin/End hooks report occlusion-query scopes. Cheap: the
// query-type inspection only runs when suppression is enabled.
void NotifyQueryBegin(ID3D11Asynchronous* async);
void NotifyQueryEnd(ID3D11Asynchronous* async);

// Per-draw decision, called by every draw hook before its forward call.
// True = swallow the draw. Also owns the stats counters.
bool ShouldSuppress();

// Drain the since-last-call counters for the periodic [VRAM] log line.
void ConsumeStats(uint64_t* suppressed, uint64_t* forwardedUi,
                  uint64_t* forwardedQuery);

}  // namespace RasterSuppress
