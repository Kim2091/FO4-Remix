#pragma once

#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// Half-float -> float conversion (shared across bs_extraction and skinning)
// ---------------------------------------------------------------------------
inline float HalfToFloat(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;

    uint32_t result;
    if (exp == 0) {
        if (mant == 0) {
            result = sign;
        } else {
            // Denormalized -> renormalize
            exp = 1;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            mant &= 0x3FF;
            result = sign | ((exp + 112) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        result = sign | 0x7F800000 | (mant << 13); // Inf / NaN
    } else {
        result = sign | ((exp + 112) << 23) | (mant << 13);
    }

    float f;
    memcpy(&f, &result, 4);
    return f;
}

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

    // [Diagnostics]
    bool diagEnabled;             // Master toggle for periodic diagnostic logging (default true)

    // [SemanticCapture]
    bool semanticCaptureEnabled;  // [Phase 1A] Install BSLightingShaderProperty event-capture hook (default false)

    // Frames an UNRESOLVED drawable stays retry-eligible after the engine
    // last fired for it. The engine fires GetRenderPasses ~once per cell
    // attach (passes cached afterwards), and save-loads fire the destination
    // cell during the load screen when extraction can't succeed yet -- a
    // tight window permanently orphaned the player's own cell. 600 ~= 10s.
    uint32_t resolveRetryWindowFrames;

    // [Culling]
    uint32_t cullingTextureLRUGraceFrames;   // Frames a texture can go un-drawn before sweep cascades owner meshes (default 600)
    uint32_t cullingTextureLRUSweepPeriod;   // Frames between sweep invocations (default 60)
    uint32_t cullingTextureBudgetMiB;        // Soft cap on materialTex VRAM (default 0 = disabled, TTL-only)
    uint32_t cullingMaterialLRUGraceFrames;  // Frames a material can go un-drawn before cascade-eviction (default 600)

    // Frames a worldspace LOD chunk can go un-fired before OnFrame stops
    // drawing it (0 = disabled). The engine calls GetRenderPasses every frame
    // for geometry that survives its culling, and it HIDES a LOD chunk when
    // the chunk's cells attach at full detail -- so a chunk that stopped
    // firing is one the engine stopped rendering. Drawing it anyway overlays
    // the low-poly shell on the streamed-in buildings ("LODs stay lowest
    // quality" report, 2026-07-02). When the engine shows the chunk again
    // (cells detach / camera turns back), it fires and reappears within a
    // frame or two. Only applies while the 3D scene is actively firing, so
    // pause menus don't age chunks out of the Remix view.
    uint32_t cullingLodChunkStaleFrames;     // default 30

    // [Materials]
    // Spec-gloss -> metal-rough conversion for FO4 environment-mapped
    // materials (2026-07-02, take 2). FO4 authors metal albedo near-black;
    // the vanilla look comes from kSpecularColor/fSpecularColorScale + an
    // env map, neither of which the path tracer replicates -- untreated
    // kType_Envmap materials (power-armor stands, picket-fence LODs, street
    // lamps) render as black voids. When enabled, kType_Envmap materials get
    // metallic/roughness constants derived from the material's fSmoothness
    // scalar and a hue-preserving luminance floor on the diffuse albedo.
    // In-game verification (2026-07-02): the albedo luminance floor is what
    // recovers the black objects; the metallic/roughness constants did NOT
    // help (a metal takes F0 from albedo, so floor-lifted albedo * high
    // metallic still reads near-black -- the metallic constant fights the
    // floor). They are therefore opt-in, default OFF.
    bool  metalConversionEnabled;   // master toggle: classification + albedo floor (default true)
    bool  metalMetallicEnabled;     // apply derived metallicConstant (default false)
    bool  metalRoughnessEnabled;    // apply derived roughnessConstant (default false)
    float metalMetallic;            // metallic at fSmoothness=1; scaled down to 0.2x of this at fSmoothness=0 (default 0.85)
    float metalAlbedoLumFloor;      // 0..1 minimum diffuse luminance, hue-preserving lift below it (default 0.25)
    float metalMinRoughness;        // floor on (1 - fSmoothness) so metals aren't mirrors (default 0.15)

    // [Camera]
    // FOV source for the Remix camera (2026-07-03). Default true: read the
    // live NiCamera view frustum (exact vertical FOV + aspect + near/far,
    // tracks ADS zoom and FOV mods), falling back to fDefaultWorldFov
    // converted horizontal->vertical at 16:9. False restores the legacy
    // behavior of passing fDefaultWorldFov through raw -- which the runtime
    // treats as VERTICAL FOV, rendering ~112 deg horizontal for an 80 deg
    // setting (the "fov is different than the game" report).
    bool cameraFovFromFrustum;

    // [Overlay]
    // HUD/Scaleform overlay submission via Remix's DrawScreenOverlay API.
    // Default OFF: the in-source dxvk-remix's dispatchScreenOverlay currently
    // asserts in dxvk_barrier.cpp on a Vulkan layout transition (dstLayout ==
    // VK_IMAGE_LAYOUT_UNDEFINED). Gating this here keeps the plugin functional
    // for everything else; flip to true once the runtime barrier path is fixed.
    bool hudOverlayEnabled;

    // After Remix's overlay window registers raw input with RIDEV_NOLEGACY on
    // the keyboard, WM_KEYDOWN/WM_KEYUP stop being delivered to ANY window in
    // this process -- including the game window. When restoreLegacyInput is
    // true (default), the plugin issues a RIDEV_REMOVE for keyboard right
    // after Remix's Startup, restoring legacy keyboard messages to the game.
    // Trade-off: dev-menu raw-input hotkeys (Alt+X, etc.) stop working. Set
    // false if you need the dev menu and don't mind losing game keyboard.
    bool restoreLegacyInput;

    // [Performance]
    // Share Remix mesh handles across drawables with byte-identical geometry
    // and material, and batch identical drawables in OnFrame via
    // remixapi_InstanceInfoGpuInstancingEXT. Saves BLAS/VRAM and per-frame
    // DrawInstance count. Default true; flip false to fall back to a
    // unique-mesh-per-drawable rendering path if a regression appears.
    bool gpuInstancingEnabled;

    // Frame-rate target for the Remix render thread. The thread loop paces to
    // this by sleeping only the unused remainder of the frame budget after
    // OnFrame returns. 0 = uncapped (yield-only between frames).
    uint32_t remixMaxFPS;

    // Max concurrent async texture readbacks (staging textures in flight).
    // Directly sets scene pop-in time after a load: a slot turns over in
    // ~2-3 ticks, so N slots resolve ~N textures per few frames. Staging
    // VRAM is transient (~1.4MB per 1K BC7 chain). 0 = built-in default.
    uint32_t maxPendingTextureReadbacks;
};

// Global config instance
extern PluginConfig g_config;

// Load config from FO4RemixPlugin.ini next to the DLL.
// Call once at plugin load time.
void LoadConfig();
