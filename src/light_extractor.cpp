#include "light_extractor.h"
#include "config.h"

#include "f4se_common/f4se_version.h"
#include "f4se_common/Relocation.h"
#include "f4se/PluginAPI.h"
#include "f4se/NiTypes.h"

#include <cmath>
#include <cstring>

static constexpr uintptr_t OFF_REFR_POS         = 0xD0;
static constexpr uintptr_t OFF_REFR_ROT         = 0xC0;
static constexpr uintptr_t OFF_REFR_BASE_FORM   = 0xE0;
static constexpr uintptr_t OFF_CELL_OBJECT_LIST = 0x70;
static constexpr uint8_t   FORM_TYPE_LIGH       = 34;
static constexpr uintptr_t OFF_FORM_TYPE        = 0x1A;

// TESObjectLIGH DATA subrecord offset from base form pointer.
// Verified via memory scan: radius=256 at +0x14C, color=0x00FFFFFF at +0x150.
static constexpr uintptr_t OFF_LIGH_DATA = 0x148;

// DATA subrecord internal layout
static constexpr uintptr_t OFF_DATA_RADIUS  = 0x04;
static constexpr uintptr_t OFF_DATA_COLOR   = 0x08;
static constexpr uintptr_t OFF_DATA_FLAGS   = 0x0C;
static constexpr uintptr_t OFF_DATA_FOV     = 0x14;

// FNAM (fade) is typically right after the DATA subrecord (0x38 bytes)
static constexpr uintptr_t OFF_LIGH_FADE = OFF_LIGH_DATA + 0x38;

static constexpr uint32_t LIGH_FLAG_SPOTLIGHT = 0x100;

// Intensity tuning constant: converts FO4 light into Remix HDR radiance.
// Scales by radius to ensure lights are visible at their intended range
// given Bethesda-unit scene distances (~70 units/meter).
// Tuned so that INI Intensity=1.0 gives reasonable brightness.
static constexpr float kIntensityScale = 0.1f;

std::vector<ExtractedLight> LightExtractor::ExtractCellLights(uintptr_t cellPtr)
{
    std::vector<ExtractedLight> result;

    if (!cellPtr) return result;

    struct SimpleArray {
        uintptr_t* entries;
        uint32_t capacity;
        uint32_t pad0C;
        uint32_t count;
    };
    auto& objectList = *reinterpret_cast<SimpleArray*>(cellPtr + OFF_CELL_OBJECT_LIST);

    uint32_t lightCount = 0;
    static bool s_loggedDiscovery = false;

    for (uint32_t i = 0; i < objectList.count; i++) {
        uintptr_t refrPtr = objectList.entries[i];
        if (!refrPtr) continue;

        uintptr_t baseForm = *reinterpret_cast<uintptr_t*>(refrPtr + OFF_REFR_BASE_FORM);
        if (!baseForm) continue;

        uint8_t formType = *reinterpret_cast<uint8_t*>(baseForm + OFF_FORM_TYPE);
        if (formType != FORM_TYPE_LIGH) continue;

        // Read REFR world position
        float* refrPos = reinterpret_cast<float*>(refrPtr + OFF_REFR_POS);
        float* refrRot = reinterpret_cast<float*>(refrPtr + OFF_REFR_ROT);

        // Read light properties from base form
        uintptr_t dataBase = baseForm + OFF_LIGH_DATA;
        uint32_t rawRadius = *reinterpret_cast<uint32_t*>(dataBase + OFF_DATA_RADIUS);
        uint32_t rawColor  = *reinterpret_cast<uint32_t*>(dataBase + OFF_DATA_COLOR);
        uint32_t rawFlags  = *reinterpret_cast<uint32_t*>(dataBase + OFF_DATA_FLAGS);
        float spotFOV      = *reinterpret_cast<float*>(dataBase + OFF_DATA_FOV);
        float fade         = *reinterpret_cast<float*>(baseForm + OFF_LIGH_FADE);

        // Log first few lights for verification
        if (g_config.logLights && !s_loggedDiscovery && lightCount < 5) {
            _MESSAGE("FO4RemixPlugin: Light REFR=0x%llX baseForm=0x%llX "
                     "pos=(%.1f, %.1f, %.1f) rot=(%.2f, %.2f, %.2f) "
                     "radius=%u color=0x%08X flags=0x%08X fov=%.1f fade=%.2f",
                     (unsigned long long)refrPtr, (unsigned long long)baseForm,
                     refrPos[0], refrPos[1], refrPos[2],
                     refrRot[0], refrRot[1], refrRot[2],
                     rawRadius, rawColor, rawFlags, spotFOV, fade);
        }

        // Sanity checks
        if (rawRadius == 0 || rawRadius > 100000) continue;
        if (fade <= 0.0f) fade = 1.0f;

        // Unpack color
        float r = (rawColor & 0xFF) / 255.0f;
        float g = ((rawColor >> 8) & 0xFF) / 255.0f;
        float b = ((rawColor >> 16) & 0xFF) / 255.0f;

        float intensity = fade * kIntensityScale * (float)rawRadius;

        ExtractedLight light = {};
        // Stable hash from REFR form ID (consistent across runs)
        uint32_t refrFormID = *reinterpret_cast<uint32_t*>(refrPtr + 0x14);
        light.hash = FnvHashCombine(0xCBF29CE484222325ULL, (uint64_t)refrFormID);

        // Apply X/Y swap to match mesh coordinate system
        light.position[0] = refrPos[1]; // Bethesda Y -> Remix X
        light.position[1] = refrPos[0]; // Bethesda X -> Remix Y
        light.position[2] = refrPos[2]; // Z unchanged

        light.radiance[0] = r * intensity;
        light.radiance[1] = g * intensity;
        light.radiance[2] = b * intensity;

        light.radius = (float)rawRadius;

        // Spot light detection
        light.isSpotLight = (rawFlags & LIGH_FLAG_SPOTLIGHT) != 0;
        if (light.isSpotLight) {
            // Compute direction from REFR rotation (Euler angles in radians)
            float rx = refrRot[0]; // pitch
            float rz = refrRot[2]; // yaw
            // Bethesda convention: default light direction is -Z (downward),
            // rotated by the reference's rotation
            float dx = sinf(rz) * cosf(rx);
            float dy = cosf(rz) * cosf(rx);
            float dz = -sinf(rx);
            // Apply X/Y swap
            light.spotDirection[0] = dy;
            light.spotDirection[1] = dx;
            light.spotDirection[2] = dz;
            light.spotFOV = spotFOV;
            light.spotSoftness = 0.2f; // Slight softness on cone edge
        }

        result.push_back(light);
        lightCount++;
    }

    if (!s_loggedDiscovery && lightCount > 0) {
        s_loggedDiscovery = true;
    }

    if (g_config.logLights)
        _MESSAGE("FO4RemixPlugin: Extracted %u lights from cell", lightCount);
    return result;
}
