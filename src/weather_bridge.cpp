#include "weather_bridge.h"
#include "remix_renderer.h"

#include "f4se/GameForms.h"
#include "f4se/PluginAPI.h"

#include <cmath>
#include <cstdio>

namespace {
    constexpr float kPi = 3.14159265358979323846f;

    // Bethesda-hardcoded TES4-record formID for the GameHour TESGlobal.
    // Same value across TES4/Skyrim/FO4; cannot be removed or replaced by
    // mods because the engine reads it from a fixed slot at startup.
    constexpr UInt32 kGameHourFormID = 0x00000038;

    // Queued, not pushed directly: SetConfigVariable blocks on the Remix-API
    // mutex, which OnFrame holds for nearly its entire frame (including the
    // multi-ms Present) and re-acquires immediately. An unfair lock at that
    // duty cycle starved this game-thread call for 20+ seconds at a time
    // (observed 2026-07-02: gameFrames=0 perf windows; the user-visible
    // "game freezes until I alt-tab" symptom). The queue hand-off is a
    // microsecond map insert; OnFrame applies it on the Remix thread.
    // Failure logging (key missing in fork) happens at the drain site.
    void PushFloat(const char* key, float value) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4f", value);
        RemixRenderer::QueueConfigVariable(key, buf);
    }
}

void WeatherBridge::PushOncePerFrame() {
    // GameHour pointer is cached after first successful lookup. TESGlobal
    // pointers are stable for the plugin's lifetime, so the cache never
    // invalidates. Pre-cache the lookup retries every frame until the form
    // database is ready (return early without logging — this is normal
    // during startup, not an error).
    static TESGlobal* cachedGameHour = nullptr;
    if (!cachedGameHour) {
        TESForm* form = LookupFormByID(kGameHourFormID);
        if (!form || form->formType != kFormType_GLOB) {
            return;
        }
        cachedGameHour = static_cast<TESGlobal*>(form);
    }

    // Bethesda's GameHour TESGlobal runs 0..24. Sun peaks at noon (hour 12) with elevation
    // ~90deg, sits at the horizon at hour 6 (dawn) and hour 18 (dusk), dips
    // negative at night. Rotation advances linearly through the full day.
    const float hour = cachedGameHour->value;
    const float sunElevation = std::sin((hour - 6.0f) / 12.0f * kPi) * 90.0f;
    const float sunRotation  = (hour / 24.0f) * 360.0f;

    // Only push when the sun actually moved. The queue hand-off is cheap but
    // there's no reason to churn the pending map (or the runtime's option
    // registry) every game frame. At default timescale the sun moves
    // ~0.13 deg/sec, so a 0.05 deg threshold re-pushes every few hundred ms;
    // imperceptible against the 0.5 deg sun disk.
    static float s_lastElevation = -1000.0f;
    static float s_lastRotation  = -1000.0f;
    constexpr float kMinDeltaDeg = 0.05f;
    if (std::fabs(sunElevation - s_lastElevation) < kMinDeltaDeg &&
        std::fabs(sunRotation  - s_lastRotation)  < kMinDeltaDeg) {
        return;
    }
    s_lastElevation = sunElevation;
    s_lastRotation  = sunRotation;

    PushFloat("rtx.atmosphere.sunElevation", sunElevation);
    PushFloat("rtx.atmosphere.sunRotation",  sunRotation);
}
