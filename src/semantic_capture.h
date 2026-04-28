#pragma once

#include <cstdint>
#include <unordered_set>

struct ID3D11Device;
struct NiTransform;

namespace SemanticCapture {

    // Per-drawable state tracked in g_drawableMap. Exposed in the header so
    // future phases (1B onward) can extend it without a forward-declaration
    // dance.
    struct DrawableState {
        // ---- Telemetry (Phase 1A; unchanged behavior) ----
        uint64_t firstSeenFrame      = 0;
        uint64_t lastSeenFrame       = 0;
        uint32_t fireCount           = 0;
        uint32_t lastTechniqueFlags  = 0;

        // ---- 1B: pointers captured by the hot-path detour on first-seen ----
        void*    geometry            = nullptr;  // BSGeometry*  (rdx)
        void*    property            = nullptr;  // BSLightingShaderProperty* (rcx)
        void*    material            = nullptr;  // [property+0x48]

        // ---- 1B: submission state (mutated on Remix thread only) ----
        // Field order: 8-byte aligned types first to avoid padding around the bool.
        uint64_t meshHash            = 0;        // == PassKey, used as Remix submission key
        uint64_t materialHash        = 0;        // index into g_materialCache
        bool     submittedToRemix    = false;
        // Set of texture hashes this drawable references. NOT the refcount owner —
        // the canonical refcount tracking lives in g_drawables[meshHash].textureHashes
        // inside remix_renderer.cpp. This field is populated by the resolver as a
        // secondary record; ReleaseDrawable does not consult it.
        std::unordered_set<uint64_t> textureHashes;
    };

    // Convert a Bethesda NiTransform (right-handed, X/Y in Bethesda order)
    // into a Remix-friendly row-major 3x4 matrix: rotation columns 0/1
    // swapped, translation x/y swapped, scale applied. Output is
    // mesh.worldTransform[3][4].
    void BuildRemixTransform(const NiTransform& niXf, float out[3][4]);

    // Install the BSLightingShaderProperty render-pass-equivalent hook
    // (slot 0x2B at Fallout4.exe RVA 0x02172540) and start tracking
    // captured drawables in the DrawableMap.
    //
    // No-op if [SemanticCapture] Enabled=0. Idempotent.
    // Returns true on success.
    bool Install();

    // Remove the hook. Idempotent. Logs the final fire count.
    void Uninstall();

    // Run the resolve loop + (rate-limited) sweep + stats line. Call every
    // frame from hkPresent (game thread); a D3D11 device is required for
    // texture readbacks performed by resolvers. The resolve loop runs every
    // call (cheap when every drawable is already submitted); the sweep is
    // internally rate-limited to every kSweepPeriodFrames frames.
    //
    // device is the D3D11 device used by texture readback inside resolvers.
    void Tick(ID3D11Device* device);

    // Drop every tracked drawable: release Remix-side handles and clear
    // g_drawableMap. Called from the F4SE PreLoadGame handler.
    void ClearDrawableMap();

    // Build the set of submitted drawable hashes whose state.lastSeenFrame
    // is within `maxAge` of `currentFrame`. Caller passes an empty set;
    // function locks g_drawableMutex briefly and fills it. Used by OnFrame
    // to skip drawing drawables the engine stopped firing for (LOD swaps,
    // off-frustum culling) without evicting their cached mesh handles.
    void SnapshotActiveDrawables(uint64_t currentFrame,
                                 uint64_t maxAge,
                                 std::unordered_set<uint64_t>& out);
}
