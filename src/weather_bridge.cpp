#include "weather_bridge.h"
#include "remix_renderer.h"

#include "f4se/GameForms.h"
#include "f4se/PluginAPI.h"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_set>

namespace {
    // Per-key failure dedup. First failure for a given key logs at warn;
    // subsequent failures for that same key are silenced so a misconfigured
    // Remix fork doesn't spam the F4SE log 60 times a second.
    std::mutex g_failMutex;
    std::unordered_set<std::string> g_failedKeys;

    constexpr float kPi = 3.14159265358979323846f;

    // Bethesda-hardcoded TES4-record formID for the GameHour TESGlobal.
    // Same value across TES4/Skyrim/FO4; cannot be removed or replaced by
    // mods because the engine reads it from a fixed slot at startup.
    constexpr UInt32 kGameHourFormID = 0x00000038;

    void PushFloat(const char* key, float value) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4f", value);
        if (RemixRenderer::SetConfigVariable(key, buf)) return;

        std::lock_guard<std::mutex> lk(g_failMutex);
        if (g_failedKeys.insert(key).second) {
            _MESSAGE("FO4RemixPlugin: [weather_bridge] SetConfigVariable failed "
                     "for key '%s' = %.4f (key not registered in Remix fork?)",
                     key, value);
        }
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
    PushFloat("rtx.atmosphere.sunElevation", sunElevation);
    PushFloat("rtx.atmosphere.sunRotation",  sunRotation);
}
