#pragma once

#include <cstdint>

struct PluginConfig {
    // [Logging]
    bool logShapeInfo;       // Log shape name, vertex format, flags for every extracted shape
    bool logLargeShapes;     // Log shapes with extent > 500
    bool logRejections;      // Log rejected meshes (NaN, extent, etc.)
    bool logTextures;        // Log extracted texture info
    bool logLights;          // Log extracted light info

    // [Limits]
    float maxExtent;         // Reject shapes with extent larger than this (default 10000)

    // [Lights]
    bool  lightsEnabled;     // Master toggle for all extracted lights
    float lightIntensity;    // Multiplier for light radiance (default 1.0)
    float lightRadius;       // Multiplier for light radius (default 1.0)
    float lightColorStrength;// 0 = white, 1 = full game color (default 1.0)
};

// Global config instance
extern PluginConfig g_config;

// Load config from FO4RemixPlugin.ini next to the DLL.
// Call once at plugin load time.
void LoadConfig();
