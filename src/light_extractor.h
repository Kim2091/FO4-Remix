#pragma once

#include <cstdint>
#include <vector>

struct ExtractedLight {
    uint64_t hash;          // Unique ID derived from REFR pointer
    float position[3];      // World position (already X/Y swapped)
    float radiance[3];      // HDR RGB radiance
    float radius;           // FO4 radius in game units (used for Remix falloff)
    bool isSpotLight;       // true if FO4 flags have SpotLight bit
    float spotDirection[3]; // Direction for spot lights (X/Y swapped)
    float spotFOV;          // Spot light cone angle in degrees
    float spotSoftness;     // Cone edge softness (0 = hard)
};

namespace LightExtractor {
    // Extract all placed lights from the player's current cell.
    // Must be called on the main thread.
    std::vector<ExtractedLight> ExtractPlayerCellLights();
}
