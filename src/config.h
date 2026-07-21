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

    // [Lights]
    bool  lightsEnabled;     // Master toggle for all extracted lights
    float lightIntensity;    // Multiplier for light radiance (default 1.0)
    float lightRadius;       // Multiplier for light radius (default 1.0)
    float lightColorStrength;// 0 = white, 1 = full game color (default 1.0)
    // In-place light updates via remixapi UpdateLightDefinition (2026-07-18,
    // BetaRT recipe). When a snapshot changes an existing light's derived
    // params (position from the REFR at poll time, radius/radiance after
    // config multipliers, spot shaping, near-camera flags), the definition is
    // updated on the SAME handle+hash so the runtime's persistent RTXDI
    // reservoirs survive (no re-seed boiling). Runtime without the entry
    // point (or a failed update) falls back to destroy+recreate. Off = the
    // legacy behavior: an existing hash keeps its creation-time params until
    // the light leaves the snapshot entirely.
    bool  lightsLiveUpdate;  // default true
    // Lights within this many game units of the camera get
    // ignoreViewModel + ignoreFirstPersonPlayerShadow so the 1st-person
    // arms/weapon can't shadow the whole scene from a light at arm's reach
    // (the classic flashlight-through-viewmodel artifact; BetaRT sets these
    // on its held-torch light). Re-evaluated on every snapshot diff (~1Hz +
    // cell changes) via the live-update path. 0 = disabled.
    float lightsNearCameraIgnoreVMUnits;  // default 150

    // [Skinning]
    bool  skinningEnabled;   // Extract and animate skinned meshes (characters, creatures)
    bool  viewModelEnabled;  // Render the 1st-person arms/weapon/Pip-Boy (synthetic-space remap)
    bool  viewModelBoneConventionFix;  // Camera bone is NIF-camera-convention {right,up,back} vs cameraNode {right,fwd,up}
    // Submit a second SetupCamera of type VIEW_MODEL each frame (2026-07-18,
    // BetaRT recipe): same pose as the world camera, but with the game's
    // 1st-person FOV (fDefault1stPersonFOV, horizontal->vertical converted)
    // and a small near plane. The runtime renders VIEW_MODEL-categorized
    // geometry with this camera, so the arms/weapon keep their own FOV when
    // the world FOV changes (ADS zoom, FOV mods) instead of distorting.
    bool  viewModelSeparateCamera;     // default true
    // Manual vertical-FOV override for the view-model camera, degrees.
    // 0 = auto (fDefault1stPersonFOV converted at the live aspect).
    float viewModelFovOverride;        // default 0
    // Tag viewmodel draws with REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL so
    // the runtime applies its view-model handling (separate camera above,
    // rtx.viewModel.* options). Buckets are split by the flag so a mesh
    // shared between a 1P part and a world object can't be mistagged.
    bool  viewModelCategoryTag;        // default true
    // Live render-target texture refresh period, in game frames (2026-07-18
    // Pip-Boy screen). The screen mesh's texture is a render target the
    // engine composites the Scaleform UI into at runtime; a one-shot capture
    // shows whatever was in it at resolve time (blank). Drawables whose
    // extraction detected an RT-backed texture get a SHADOW re-resolve every
    // this-many frames while the engine is drawing them: a new texture
    // generation extracts asynchronously while the old instance keeps
    // rendering, then SubmitDrawable swaps the handles in place (no flicker).
    // 0 = off (static capture, old behavior).
    uint32_t viewModelScreenRefreshFrames;  // default 12

    // Pip-Boy screen FEED (2026-07-18 v2). The screen material's texture is
    // NOT the Scaleform RT (log-proven: [LiveTex] silent while Screen:0
    // submits fine) -- the engine composites the UI onto the screen mesh
    // via a separate draw the capture pipeline never sees. Instead, the
    // overlay's multi-layer capture routes the Pip-Boy Scaleform layer's
    // PIXELS onto the submitted "Screen:0" viewmodel drawable as its
    // diffuse+emissive (the mesh's own UVs are the engine's mapping for
    // exactly this content), refreshed every ScreenRefreshFrames via an
    // in-place handle swap. The fed layer is dropped from the full-screen
    // composite while the feed is live (the giant floating Pip-Boy UI).
    bool  pipboyScreenFeed;           // default true
    float pipboyScreenTintR;          // default 0.08 (vanilla fPipboyEffectColor)
    float pipboyScreenTintG;          // default 1.00
    float pipboyScreenTintB;          // default 0.09
    float pipboyScreenEmissiveScale;  // default 1.5

    // Readable panel for the fed Pip-Boy layer (2026-07-18 v2 field fix).
    // The physical screen is ~100px tall at typical camera distance -- the
    // viewmodel anchor glues the 1P rig to the camera, so the engine's
    // pipboy-view camera zoom (what makes the screen readable in vanilla)
    // never happens in the Remix render. Until that camera story exists,
    // the fed layer ALSO composites as a centered panel scaled to this
    // fraction of screen height (never upscaled past native). 0 = no panel
    // (mesh only -- the v2 initial behavior, unusable in the field test).
    float overlayPipboyPanelFrac;     // default 0.55

    // [Overlay] Multi-layer UI capture (2026-07-18). FO4's Scaleform UI is
    // NOT one surface: the HUD/interface RT, the main menu, the Pip-Boy,
    // and terminals each composite into their OWN render target, so the
    // single locked-RT capture only ever shipped the HUD layer. Every
    // Scaleform target shares one fingerprint -- an R8G8B8A8 DEFAULT-usage
    // texture cleared to transparent black at the start of its UI pass,
    // then sole-bound and drawn. When enabled, ALL such layers are captured
    // each frame and CPU-composited in clear order into the one overlay
    // image DrawScreenOverlay ships; draws to any recognized layer also
    // open the raster-suppression UI phase (fixes menus whose draws never
    // touched the locked RT). 0 = legacy single-RT capture.
    bool overlayMultiLayer;  // default true

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
    float    cullingLodChunkFarExtentRatio;  // default 0 (off); skip LOD chunk when
                                             // box distance > extent * ratio

    // VRAM-pressure force-eviction (2026-07-20). The drawable TTL
    // (kTTLFrames=18000, ~5 min) never fires on a cross-country run, so
    // drawables pin materials pin textures until the driver budget cliff
    // (Sanctuary->Concord: 13.5/14.7 GiB, 1 fps). When process-local VRAM
    // usage exceeds ForceEvictVramPct% of the DXGI budget, Tick's sweep
    // force-evicts the oldest-seen submitted drawables (min age 300 frames,
    // ForceEvictPerSweep per sweep) so the LRU cascade can reclaim.
    uint32_t cullingForceEvictVramPct;       // default 88; 0 = off (tier-2 oldest-first fallback)
    uint32_t cullingForceEvictPerSweep;      // default 512

    // Tier-1 view-based parking (2026-07-20 rework): at this softer
    // threshold, submitted drawables behind the camera and farther than
    // ForceEvictBehindDistance are PARKED (Remix resources released, entry
    // kept + resolver-skipped) furthest-first, before the oldest-first
    // fallback above ever fires. Un-parks on view re-entry or when usage
    // drops 5 points below the threshold.
    uint32_t cullingForceEvictViewPct;       // default 80; 0 = off
    float    cullingForceEvictBehindDistance; // game units, default 8000 (~2 cells)

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
    bool  roughnessMapsEnabled;     // extract _s.dds -> per-pixel roughness maps (default true; off = roughnessConstant fallback)
    float roughnessMapFloor;        // 0..1 floor on _s.dds-derived per-pixel roughness (default 0.15; decals clamp at >= 0.3)
    // Re-capture-on-approach (2026-07-08): FO4 streams textures progressively,
    // so an object first resolved at distance captures a reduced mip and the
    // name-keyed texture cache locks that blurry version for the session. When
    // on, the Tick polls each submitted lighting drawable's live diffuse
    // resolution (~every 128 frames) and, when the engine has streamed a
    // sharper mip in, releases + re-resolves it for the full-res texture.
    // DEFAULT OFF: the release+re-resolve churn causes lag spikes while moving,
    // and the win is a progressive sharpen rather than immediate. Opt-in.
    bool  textureUpgradeOnApproach; // default false

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
    // Requires a runtime built with the rtx_fork_overlay.cpp layout fix
    // (dxvk-remix 8990aed, 2026-07-02): older runtimes created the overlay
    // image with VK_IMAGE_LAYOUT_UNDEFINED and tripped
    // assert(dstLayout != VK_IMAGE_LAYOUT_UNDEFINED) in dxvk_barrier.cpp on
    // the first HUD upload. The code default stays OFF (a missing ini key on
    // an un-fixed runtime must not crash), but the shipped ini enables it as
    // of 2026-07-03. Known costs when enabled: the capture is
    // a CopyResource + blocking Map on the game's render thread each frame
    // the UI draws, and interactive-menu input focus is not routed (pixels
    // only -- fine for the passive HUD).
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

    // Batched-draw mirrored-facing fix (2026-07-08). dxvk-remix decides its
    // facing-flip compensation for mirrored transforms from the draw call's
    // BASE transform only (RtInstance::m_isObjectToWorldMirrored,
    // rtx_instance_manager.cpp:1300) and composes GPU-instancing per-instance
    // transforms into the TLAS transform (rtx_accel_manager.cpp:1135), which
    // per the Vulkan spec cannot change facing. The batched path used to send
    // identity base + det<0 per-instance transforms (every FO4 placement is
    // mirrored by the Beth->Remix X/Y swap), so batched instances rendered
    // with facing OPPOSITE to single-member draws of the same data: repeated
    // single-sided statics (street lamps, PA stands, merge-expanded records,
    // the "unidentified exterior inverted culling") were inside-out. When
    // true, OnFrame hoists the X/Y-swap reflection P into the batched base
    // transform and pre-multiplies each member transform by P^-1 (row 0/1
    // swap); composed placement is unchanged, facing matches the single path.
    bool batchedMirrorBase;

    // [Precombines]
    // Expand each BSMergeInstancedTriShape into one Remix drawable per
    // hardware instance, with transforms read from the shape's structured
    // instance buffer (see lighting_static.cpp ReadMergeInstanceTransforms).
    // Without this, merged precombined geometry (Sanctuary roads, light
    // poles, hedges) renders once, unrotated, at the cluster origin.
    // Requires gpuInstancingEnabled (extras would otherwise each build a
    // full Remix mesh); silently inactive when that is off.
    bool mergeInstanceExpansion;

    // Rotation reading of the 80-byte instance records. true = the stored
    // rows are used as-is (engine row-vector convention); false =
    // transposed reading. Together with MergeInstanceConjugate this makes
    // every decode variant reachable from the ini without a rebuild while
    // the true record convention is pinned down empirically.
    bool mergeInstanceRowVector;

    // Interpret the instance records in the X/Y-swapped space (M' = P*S*P,
    // t' = P*t) before composing. Covers the hypothesis that the game
    // authors the GPU records in its render-mirrored coordinate order
    // rather than scene-graph order. Net linear map per (RowVector,
    // Conjugate), for an identity leaf: (1,0) P*S^T; (0,0) P*S;
    // (1,1) S^T*P; (0,1) S*P.
    bool mergeInstanceConjugate;

    // Partition multi-segment merged shapes by capturing the engine's own
    // DrawIndexedInstanced parameters for the shape's instance buffer
    // (draw_capture.h): per-sub-model index ranges + record counts, exactly
    // as vanilla renders them. Adds a DrawIndexedInstanced vtable hook
    // (one atomic load per draw once idle). Off = take-5 heuristics only
    // (equal-block split, else whole mesh x all records).
    bool mergeInstanceDrawCapture;

    // Render merge-expanded precombine geometry double-sided (default true,
    // the 2026-07-07 vanilla-faithful choice: baked kit content winding is
    // not consistently front-facing under our decode). Set false for the
    // single-sided experiment -- the inside-out evidence that forced
    // double-siding predated the batchedMirrorBase fix, which may have been
    // the real culprit; single-sided merges re-enable the per-instance
    // mirrored-record winding flip and would be a path-tracing perf win if
    // the content winding holds up.
    bool mergeTwoSided;

    // Frame-rate target for the Remix render thread. The thread loop paces to
    // this by sleeping only the unused remainder of the frame budget after
    // OnFrame returns. 0 = uncapped (yield-only between frames).
    uint32_t remixMaxFPS;

    // Max concurrent async texture readbacks (staging textures in flight).
    // Directly sets scene pop-in time after a load: a slot turns over in
    // ~2-3 ticks, so N slots resolve ~N textures per few frames. Staging
    // VRAM is transient (~1.4MB per 1K BC7 chain). 0 = built-in default.
    uint32_t maxPendingTextureReadbacks;

    // Percentage of logical CPU cores given to the async texture-decode
    // worker pool (BC decompression + per-pixel transforms off the game
    // render thread; see bs_extraction.cpp). Workers run at below-normal
    // priority, so this sets decode throughput -- i.e. how fast textures
    // finish during streaming -- without contending with the game's own
    // threads when the CPU saturates. Resolved worker count is
    // round(cores * percent / 100), clamped to [1, cores - 1]; the pool
    // must never be empty (queued textures would never finish and nothing
    // would render). Default 25 (%): 2 workers on 8 cores, 4 on 16, 8 on 32.
    uint32_t decodeWorkerPercent;

    // Absolute ceiling on the decode pool regardless of the percentage
    // (0 = no cap). BC decode is memory-bandwidth-bound, not compute-bound:
    // on a 32-logical-core machine the 25% default resolved to 8 workers and
    // measured SLOWER than 4 (2026-07-09 user report) -- past ~4 threads the
    // decoders fight each other and the game for DRAM instead of adding
    // throughput. Default 4.
    uint32_t decodeWorkerMax;

    // SOFT byte budget (MiB) for the CPU-side decoded-texture cache in
    // bs_extraction (the name+resolution-keyed mip-chain cache that feeds
    // SubmitDrawable re-supplies). Decoded RGBA chains are large (~22 MiB
    // per 2048^2) and the cache previously grew without bound for the whole
    // session (multi-hour play = unbounded RAM). Past the budget, entries
    // untouched for kTexCacheColdFrames are evicted oldest-first; HOT
    // entries are never evicted even over budget -- this cache is the live
    // working set, and evicting hot entries triggers a re-readback/
    // re-decode feedback loop (the 2026-07-10 crash-in-5-seconds
    // deployment). 0 = unbounded (legacy).
    uint32_t cpuTextureCacheMiB;

    // Longest texture edge this plugin will extract and upload to Remix
    // ([Materials] MaxTextureDimension, default 2048, 0 = uncapped). Larger
    // resident textures upload from the first mip at or under the cap --
    // the engine's own chain provides it, no resampling. Exists for texture
    // mods: 4K packs resident at load put ~4x the bytes in the Remix
    // material-texture pool (11.4 GiB observed, paging the process out of
    // the adapter budget), plus 4x the readback/decode/CPU-cache cost.
    // Vanilla content tops out at 2048, so the default only affects mods.
    // Applied consistently to the resolution hash fold, the readback, and
    // the upgrade-poll compare (see CapDim in bs_extraction.cpp).
    uint32_t maxTextureDimension;

    // Persistent disk cache of CONVERTED texture chains ([Materials]
    // DiskTextureCache, default 1). The convert stage (BC decode +
    // octahedral/invert/tint/palette bakes) costs ~11ms per chain across
    // ~2k unique chains in a fresh area -- the pop-in floor once the rest
    // of the pipeline went async. The output is deterministic per content
    // hash, so it is written once to %LOCALAPPDATA%\FO4Remix\texcache and
    // later sessions stream the converted bytes from disk instead.
    // LIMITATION: the key folds the texture NAME + the source resource's
    // desc, not the pixel content -- a texture-pack swap that keeps
    // identical names, dims, format and mip counts serves stale pixels.
    // Clear the folder (or disable this key) after swapping texture mods.
    bool     diskTextureCache;
    // Folder size cap in GiB ([Materials] DiskTextureCacheGiB, default 8).
    // Checked once per session on a worker thread; oldest files (by last
    // write) are deleted until the folder is back under 90% of the cap.
    uint32_t diskTextureCacheGiB;

    // Destroy parked Remix handles only during load screens (PreLoadGame
    // drain request) and on shutdown, instead of every 30 frames
    // ([Performance] DeferHandleDestroyToLoad, default 1). Mitigation for
    // the AV-inside-api->CreateMesh session killer (2026-07-10/11): both
    // incidents featured TexUpgrade churn with interleaved mid-gameplay
    // destroys, and a create-vs-CS-side-destruction race inside the runtime
    // is the live suspect. Parking is free plugin-side (handles are already
    // erased from the caches; CancelParkedHandle rescues re-created content
    // for as long as it stays parked); the VRAM held by parked handles is
    // reported as parked= on the [VRAM] line, with an 8192-handle emergency
    // drain valve. 0 = 30-frame cadence (A/B).
    bool deferHandleDestroyToLoad;

    // Suppress the game's own raster draws at the D3D11 hook layer
    // (raster_suppress.h has the full design). The engine keeps running its
    // complete CPU render loop -- GetRenderPasses detours, DrawCapture
    // chunk observation, texture streaming, and readbacks all still work --
    // but scene draw calls are not forwarded to the driver: game-side
    // textures become WDDM-demotable instead of pinning VRAM against the
    // path tracer (the modded-texture budget fight), and the raster GPU
    // cost disappears. Still executed: the frame's UI PHASE (first UI-RT
    // bind through end of frame -- Scaleform glyph atlases, filters, and
    // the backbuffer composite live in intermediate targets, so the whole
    // tail forwards; scene/shadow/post passes run before it and stay
    // suppressed) and draws inside occlusion-query scopes. Wants
    // [Overlay] HudOverlayEnabled=1. The game window shows live UI over a
    // stale scene while this is on. Default 0.
    bool suppressGameRaster;

    // Per-Tick wall-clock budget (milliseconds) for the semantic-capture
    // resolve loop (2026-07-09 hitching fix). Tick runs on the game render
    // thread inside hkPresent; a cell attach makes hundreds of drawables
    // resolvable in the same tick, and each resolve pays mesh parse + CPU
    // BC-decompress of every texture mip + Remix handle creation -- measured
    // 12.6ms AVERAGE per game frame during streaming windows (spikes far
    // higher), the "hitching when new geometry loads" report. The loop now
    // stops after this many ms and picks the backlog up next tick (entries
    // stay due; the work self-drains). At least one resolve always runs per
    // tick so a single over-budget item can't stall progress. 0 = unbounded
    // (legacy burst behavior).
    float resolveBudgetMs;

    // [Performance] MaxUploadMiBPerTick (default 48, 0 = uncapped). Hard cap
    // on bytes handed to the runtime per tick (CreateTexture chains +
    // CreateMesh geometry). Every byte becomes CS-chunk payload; the
    // 2026-07-17 hang dump proved the failure mode when a burst outruns the
    // CS thread's drain rate: CS-queue backpressure blocks the present
    // thread inside FlushCsChunk while it holds the device spinlock, and
    // the game thread spins unboundedly entering its next create call. The
    // supply-pass zero-copy (5b5b956) removed the memcpys that had been
    // accidentally rate-limiting exactly this.
    uint32_t maxUploadMiBPerTick;

    // [Window] (2026-07-18, BetaRT overlay-mode port). The plugin runs a
    // dual-window setup: the game window renders raster (or live UI over a
    // stale scene with SuppressGameRaster) while the plugin-created Remix
    // window shows the path-traced output. OverlayMode turns the Remix
    // window into a borderless, topmost, click-through overlay glued to the
    // game window's client rect: keys and clicks pass through to the game
    // window beneath, the game keeps focus, and the two windows read as one.
    // When the Remix dev menu opens (GetUIState != NONE) the overlay becomes
    // interactive and takes focus so ImGui gets the mouse; closing the menu
    // restores click-through and refocuses the game. 0 = legacy free-
    // floating window.
    bool windowOverlayMode;      // default true
    // Virtual-key code that toggles the Remix dev menu via SetUIState
    // (0 = disabled). Polled with GetAsyncKeyState on the Remix thread, so
    // it works regardless of which window has focus -- this makes the menu
    // reachable even with [Overlay] RestoreLegacyInput=1, whose documented
    // trade-off was losing the runtime's own raw-input hotkeys (Alt+X).
    // Default 0x77 (VK_F8).
    uint32_t windowMenuHotkey;
};

// Global config instance
extern PluginConfig g_config;

// Load config from FO4RemixPlugin.ini next to the DLL.
// Call once at plugin load time.
void LoadConfig();
