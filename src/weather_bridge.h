#pragma once

namespace WeatherBridge {
    // Reads the GameHour TESGlobal (formID 0x00000038) each frame and queues
    // derived sun-position values via RemixRenderer::QueueConfigVariable
    // (never the blocking SetConfigVariable -- see the game-freeze note in
    // weather_bridge.cpp). Safe to call every frame; no-op until the form
    // database is ready. Config-write failures logged once per key at the
    // OnFrame drain site, then silenced.
    //
    // Currently scoped to time-of-day only. Weather signals (rain/snow/fog/
    // volumetric fog/isInterior) are NOT wired up — see
    // docs/superpowers/specs/2026-04-30-fo4-time-of-day-design.md for the
    // deferred work and the FO4 Sky-struct reverse-engineering blocker.
    void PushOncePerFrame();
}
