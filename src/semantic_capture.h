#pragma once

#include <cstdint>

namespace SemanticCapture {

    // Per-drawable state tracked in g_drawableMap. Exposed in the header so
    // future phases (1B onward) can extend it without a forward-declaration
    // dance.
    struct DrawableState {
        uint64_t firstSeenFrame      = 0;  // for "new drawable" detection
        uint64_t lastSeenFrame       = 0;  // updated every fire; drives TTL eviction
        uint32_t fireCount           = 0;  // total fires for this PassKey
        uint32_t lastTechniqueFlags  = 0;  // technique enum from r8d (e.g. 0x18)
    };

    // Install the BSLightingShaderProperty render-pass-equivalent hook
    // (slot 0x2B at Fallout4.exe RVA 0x02172540) and start tracking
    // captured drawables in the DrawableMap.
    //
    // No-op if [SemanticCapture] Enabled=0. Idempotent.
    // Returns true on success.
    bool Install();

    // Remove the hook. Idempotent. Logs the final fire count.
    void Uninstall();

    // Run the periodic sweep + stats line. Call once per ~60 frames from
    // OnFrame on the Remix thread. Internally rate-limits if called more
    // frequently than every kSweepPeriodFrames frames.
    void Tick();
}
