#pragma once

#include <cstdint>
#include <unordered_set>

#include "f4se/NiObjects.h"  // NiAVObject + NiPointer (via NiTypes.h)

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
        // geometry is held via NiPointer so we own a refcount on the engine
        // BSGeometry. The engine cannot destroy it underneath us; when the
        // engine releases its own refs (cell unload, LOD swap, REFR disable),
        // m_uiRefCount drops to 1 (just our ref) and the sweep evicts the
        // entry, releasing our ref and letting the destructor finally run.
        // property/material stay raw void* -- BSGeometry holds them via
        // NiPointer internally, so they live as long as geometry does.
        NiPointer<NiAVObject> geometry;          // was void*; refcount-owned
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

    // Drop every tracked drawable: release its Remix-side handles and
    // destruct its NiPointer<NiAVObject> (DecRef cascade). Called from the
    // F4SE PreLoadGame handler. The engine's reload sequence can stall
    // indefinitely waiting for BSGeometry destructors that cannot run while
    // we hold +1 refs; releasing here lets the engine fully tear down the
    // old world. Submission resumes naturally as new drawables fire post-
    // load.
    void ClearDrawableMap();
}
