#include "light_extractor.h"

#include "f4se_common/f4se_version.h"
#include "f4se_common/Relocation.h"
#include "f4se/PluginAPI.h"
#include "f4se/NiTypes.h"

#include <cmath>
#include <cstring>

// Same player RelocPtr as scene_extractor.cpp
static RelocPtr<uintptr_t> s_g_player(0x032D2260);

static constexpr uintptr_t OFF_REFR_PARENT_CELL = 0xB8;
static constexpr uintptr_t OFF_REFR_POS         = 0xD0;
static constexpr uintptr_t OFF_REFR_ROT         = 0xC0;
static constexpr uintptr_t OFF_REFR_BASE_FORM   = 0xE0;
static constexpr uintptr_t OFF_CELL_OBJECT_LIST = 0x70;
static constexpr uint8_t   FORM_TYPE_LIGH       = 34;
static constexpr uintptr_t OFF_FORM_TYPE        = 0x1A;

// TESObjectLIGH DATA subrecord offset from base form pointer.
// This is an estimate from the inheritance chain. If lights have
// wrong colors/radii, adjust this offset and check the discovery log.
static constexpr uintptr_t OFF_LIGH_DATA = 0x160;

// DATA subrecord internal layout
static constexpr uintptr_t OFF_DATA_RADIUS  = 0x04;
static constexpr uintptr_t OFF_DATA_COLOR   = 0x08;
static constexpr uintptr_t OFF_DATA_FLAGS   = 0x0C;
static constexpr uintptr_t OFF_DATA_FOV     = 0x14;

// FNAM (fade) is typically right after the DATA subrecord (0x38 bytes)
static constexpr uintptr_t OFF_LIGH_FADE = OFF_LIGH_DATA + 0x38;

static constexpr uint32_t LIGH_FLAG_SPOTLIGHT = 0x100;

// Intensity tuning constant: converts FO4 light into Remix HDR radiance
static constexpr float kIntensityScale = 50.0f;

std::vector<ExtractedLight> LightExtractor::ExtractPlayerCellLights()
{
    std::vector<ExtractedLight> result;

    uintptr_t* ppPlayer = reinterpret_cast<uintptr_t*>(s_g_player.GetPtr());
    if (!ppPlayer || !*ppPlayer) return result;
    uintptr_t player = *ppPlayer;

    uintptr_t cellPtr = *reinterpret_cast<uintptr_t*>(player + OFF_REFR_PARENT_CELL);
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

        // Offset discovery: log first few lights so we can verify against xEdit
        if (!s_loggedDiscovery && lightCount < 5) {
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

        float intensity = fade * kIntensityScale;

        ExtractedLight light = {};
        light.hash = refrPtr * 0x9E3779B97F4A7C15ULL;

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

    _MESSAGE("FO4RemixPlugin: Extracted %u lights from cell", lightCount);
    return result;
}
