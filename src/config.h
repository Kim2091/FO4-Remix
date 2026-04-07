#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// FNV-1a hash utilities for stable, deterministic hashing
// ---------------------------------------------------------------------------
inline uint64_t FnvHash(const char* str) {
    uint64_t h = 0xCBF29CE484222325ULL;
    if (str) {
        for (; *str; ++str) {
            h ^= (uint8_t)*str;
            h *= 0x100000001B3ULL;
        }
    }
    return h;
}

inline uint64_t FnvHashCombine(uint64_t h, uint64_t val) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
    for (int i = 0; i < 8; i++) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

struct PluginConfig {
    // [Logging]
    bool logShapeInfo;       // Log shape name, vertex format, flags for every extracted shape
    bool logLargeShapes;     // Log shapes with extent > 500
    bool logRejections;      // Log rejected meshes (NaN, extent, etc.)
    bool logTextures;        // Log extracted texture info
    bool logLights;          // Log extracted light info
    bool logBoneDiag;        // One-shot bone matrix diagnostic dump (first skinned mesh, bone 0)

    // [Limits]
    float maxExtent;         // Reject shapes with extent larger than this (default 10000)

    // [Lights]
    bool  lightsEnabled;     // Master toggle for all extracted lights
    float lightIntensity;    // Multiplier for light radiance (default 1.0)
    float lightRadius;       // Multiplier for light radius (default 1.0)
    float lightColorStrength;// 0 = white, 1 = full game color (default 1.0)

    // [Skinning]
    bool  skinningEnabled;   // Extract and animate skinned meshes (characters, creatures)

    // [Emissive]
    bool  emissiveGlowMapsEnabled;  // Extract glow map textures from BSLightingShaderMaterialGlowmap
    bool  emissiveColorEnabled;     // Use emissive color/scale from BSLightingShaderProperty
    float emissiveIntensity;        // Global multiplier on FO4's fEmitColorScale
    bool  logEmissive;              // Log emissive extraction details
};

// Global config instance
extern PluginConfig g_config;

// Load config from FO4RemixPlugin.ini next to the DLL.
// Call once at plugin load time.
void LoadConfig();
