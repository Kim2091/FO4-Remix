#pragma once

namespace WeatherBridge {
    // Reads the GameHour TESGlobal (formID 0x00000038) each frame and pushes
    // derived sun-position values to Remix via RemixRenderer::SetConfigVariable.
    // Safe to call every frame; no-op until the form database is ready. All
    // SetConfigVariable failures logged once per key, then silenced.
    //
    // Currently scoped to time-of-day only. Weather signals (rain/snow/fog/
    // volumetric fog/isInterior) are NOT wired up — see
    // docs/superpowers/specs/2026-04-30-fo4-time-of-day-design.md for the
    // deferred work and the FO4 Sky-struct reverse-engineering blocker.
    void PushOncePerFrame();
}
