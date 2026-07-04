#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ID3D11Device;
struct NiTransform;

namespace SemanticCapture {

    // Tag for which resolver should process this DrawableState. Set on
    // first-seen by whichever hook detour fired; never changes for the
    // life of the entry. Tick's resolve loop dispatches by this enum.
    enum class ResolverKind : uint8_t {
        Lighting = 0,   // BSLightingShaderProperty hook (RVA 0x02172540)
        Water    = 1,   // BSWaterShaderProperty hook (RVA 0x021D15A0)
    };

    // Per-drawable state tracked in g_drawableMap. Exposed in the header so
    // future phases (1B onward) can extend it without a forward-declaration
    // dance.
    struct DrawableState {
        // ---- Telemetry (Phase 1A; unchanged behavior) ----
        uint64_t firstSeenFrame      = 0;
        uint64_t lastSeenFrame       = 0;
        uint32_t fireCount           = 0;
        uint32_t lastTechniqueFlags  = 0;
        ResolverKind resolverKind = ResolverKind::Lighting;

        // ---- 1B: pointers captured by the hot-path detour on first-seen ----
        void*    geometry            = nullptr;  // BSGeometry*  (rdx)
        void*    property            = nullptr;  // BSLightingShaderProperty* (rcx)
        void*    material            = nullptr;  // [property+0x58] (BSShaderProperty::shaderMaterial)

        // ---- LOD-overlap diagnostic (2026-04-28) ----
        // NiAVObject::flags snapshot taken on the game thread inside the hook
        // detour. Read directly via geometry+0x108 -- safe at hook fire time
        // because the engine is in the middle of a GetRenderPasses call on
        // this geometry. initialFlags is the value at first-seen (captures
        // the static kFlagIsMeshLOD bit; never changes per-geometry).
        // lastFlags is refreshed every fire (captures the dynamic
        // kFlagFadedIn / kFlagLODFadingOut / kFlagNotVisible bits).
        uint64_t initialFlags        = 0;
        uint64_t lastFlags           = 0;

        // Parent NiNode chain captured on first-seen. NiAVObject::m_parent
        // lives at offset 0x28 of the geometry. Two levels up because the
        // distinguishing flag (LOD / TopFadeNode / etc.) is often set on a
        // grouping parent rather than the leaf BSTriShape we capture. The
        // resolver reads names + flags off these at submit time; both reads
        // run inside the same SEH frame as the rest of resolver work.
        void*    parent1             = nullptr;  // NiAVObject* — geometry->m_parent
        void*    parent2             = nullptr;  // NiAVObject* — parent1->m_parent

        // ---- 1B: submission state (mutated on Remix thread only) ----
        // Field order: 8-byte aligned types first to avoid padding around the bool.
        uint64_t meshHash            = 0;        // == PassKey, used as Remix submission key
        uint64_t materialHash        = 0;        // index into g_materialCache
        bool     submittedToRemix    = false;
        // Merge-instanced expansion (2026-07-03): derived hashes of the extra
        // per-hardware-instance drawables submitted beyond meshHash (instance
        // 0 keeps meshHash). Same lifecycle: released wherever meshHash is
        // released (sweep eviction, ClearDrawableMap). Empty for plain shapes.
        std::vector<uint64_t> extraMeshHashes;
        // Set of texture hashes this drawable references. NOT the refcount owner —
        // the canonical refcount tracking lives in g_drawables[meshHash].textureHashes
        // inside remix_renderer.cpp. This field is populated by the resolver as a
        // secondary record; ReleaseDrawable does not consult it.
        std::unordered_set<uint64_t> textureHashes;

        // Last resolver gate that returned false. Snapshotted from
        // Resolvers::Trace::LastStep() after each resolver call when
        // submittedToRemix is still false. 0 (kIdle) means "never been resolved"
        // (resolver loop's freshness gate skipped this entry, or it's brand-new).
        // Used by the sweep stats to break down `pending` by gate.
        int lastFailedResolverStep   = 0;

        // ---- Resolve retry backoff (2026-07-02) ----
        // Every failed resolve used to be retried on EVERY tick for as long
        // as the entry stayed inside the retry window -- with a persistent
        // failure class (post-load submitFailed storm) that meant thousands
        // of full parse+extract attempts per frame on the game thread
        // (measured 30-62ms/frame Tick, game fps collapse, user-visible
        // freezes). Failures now schedule their next attempt: the first 4
        // retries stay at 1-frame spacing (async texture readback completes
        // within a few ticks), then the delay doubles per attempt up to a
        // 512-frame cap, so a broken entry costs ~one resolve per 8.5s
        // instead of 60/s. Crash-caught resolves get a flat 120-frame delay.
        uint32_t resolveAttempts     = 0;
        uint64_t nextRetryFrame      = 0;

        // ---- Live transform (animated statics) ----
        // Refreshed on every hook fire from BSGeometry::m_worldTransform
        // (offset 0x70 on NiAVObject). The engine evaluates scene-graph
        // controllers (NiTransformController etc.) before GetRenderPasses
        // fires, so this reflects the current animated pose. OnFrame
        // applies it to DrawInstance so animated statics (doors, gates,
        // machinery) render at their live pose instead of the captured-
        // at-resolver-time baked transform. Layout matches Remix's row-
        // major 3x4 (Beth->Remix coord swap already applied).
        float liveWorldTransform[3][4] = {};
        bool  liveTransformValid       = false;

        // Set by the fire hook when liveWorldTransform actually CHANGED
        // (bitwise) since the last DrainDirtyPoses; the key is queued once
        // on the dirty list. Cleared when drained. Static geometry never
        // sets this, so per-frame pose propagation is O(animating objects)
        // instead of O(entire map).
        bool  poseDirty                = false;

        // Set by the lighting resolver when this drawable was identified as
        // a worldspace LOD chunk (parent chain "chunk"/level or "obj").
        // Tick maintains g_lodChunkKeys from it so SnapshotLodChunkAges can
        // walk only the ~dozens of chunk entries instead of the whole map.
        bool  isLODChunk               = false;

        // ---- Merge-instanced capture upgrade (2026-07-04) ----
        // Set by the lighting resolver when a multi-segment merge shape had
        // to submit with a fallback partition because DrawCapture starved:
        // run-4 ground truth is that the engine only issues the chunk draws
        // for clusters it is actually rendering (visible), while the record
        // SRV is bound every frame regardless -- so a cluster off-screen at
        // resolve time can never capture then, but will capture the moment
        // the player looks at it. Tick keeps a background watch alive from
        // these fields (pointer IDENTITIES only, never dereferenced) and,
        // when the capture completes, releases the fallback submission and
        // re-resolves so the engine-exact baked geometry replaces it.
        bool     mergeCaptureUpgradePending = false;
        void*    mergeWatchBuf              = nullptr;
        void*    mergeWatchSrv              = nullptr;
        uint32_t mergeWatchRecordCount      = 0;
        uint32_t mergeWatchSegTris[4]       = {};
    };

    // Convert a Bethesda NiTransform (engine row-vector convention: world =
    // v * M + t) into a Remix-friendly column-vector row-major 3x4:
    // linear = P * M^T where P is the Beth->Remix X/Y mirror, translation
    // x/y swapped, scale applied. See the definition's comment block for
    // the derivation (anchored by the camera path). Output is
    // mesh.worldTransform[3][4].
    void BuildRemixTransform(const NiTransform& niXf, float out[3][4]);

    // Recover the MSVC-mangled class name of a NiAVObject (or any C++ object
    // with a vtable in the loaded Fallout4.exe module). Walks
    // vtable[-1] -> COL -> TypeDescriptor RVA -> module_base + RVA + 0x10 =
    // mangled string, strips ".?AV"/".?AU" prefix, trims at "@@". SEH-guarded
    // so a stale vtable pointer doesn't crash the caller. Writes "?" on fault
    // or empty/null input. Requires Install() to have run (sets module base).
    void GetLeafClassName(void* obj, char* out, size_t outSize);

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

    // Suspend (true) / resume (false) the resolve loop across a save-game
    // load. Set from the F4SE PreLoadGame / PostLoadGame handlers. The
    // destination cell fires GetRenderPasses DURING the load screen while
    // the engine is still building/freeing that world on its loader thread;
    // resolving against that half-built state caused parse_start access
    // violations that permanently blacklisted the player's own cell
    // (2026-07-02: 37 "CRASH CAUGHT ... skipping permanently" during one
    // load = the "area I'm standing in never loads" report). While active,
    // Tick still runs its sweep + stats; only resolves are skipped. A
    // 3600-frame failsafe clears a stuck flag (PostLoadGame never firing,
    // e.g. load aborted to main menu) so the world can't stay empty.
    void SetLoadingScreenActive(bool active);

    // Aggregate flag-bit counters over the set of drawables that pass the
    // active filter in SnapshotActiveDrawables. Diagnostic-only -- helps
    // tell whether the engine signals LOD-vs-full-detail visibility via
    // NiAVObject::flags before we commit to a flag-based draw filter.
    struct ActiveFlagStats {
        uint32_t total          = 0;
        uint32_t isLod          = 0;  // kFlagIsMeshLOD     (1 << 12)
        uint32_t fadedIn        = 0;  // kFlagFadedIn       (1LL << 37)
        uint32_t notVisible     = 0;  // kFlagNotVisible    (1LL << 39)
        uint32_t lodFadingOut   = 0;  // kFlagLODFadingOut  (1LL << 36)
        uint32_t forcedFadeOut  = 0;  // kFlagForcedFadeOut (1LL << 38)
    };

    // Build the set of submitted drawable hashes whose state.lastSeenFrame
    // is within `maxAge` of `currentFrame`. Caller passes an empty set;
    // function locks g_drawableMutex briefly and fills it. Used by OnFrame
    // to skip drawing drawables the engine stopped firing for (LOD swaps,
    // off-frustum culling) without evicting their cached mesh handles.
    //
    // If `stats` is non-null, populates per-flag counters over the same set
    // (only counts drawables that pass the active-age filter).
    void SnapshotActiveDrawables(uint64_t currentFrame,
                                 uint64_t maxAge,
                                 std::unordered_set<uint64_t>& out,
                                 ActiveFlagStats* stats = nullptr,
                                 std::unordered_map<uint64_t, std::array<float, 12>>* livePoses = nullptr);

    // Drain poses that changed since the last call into `out` (appends;
    // caller clears). O(changed-this-frame) instead of O(map): the fire hook
    // queues a key only when the captured world transform differs bitwise
    // from the stored one, i.e. animated statics mid-motion. Replaces the
    // per-frame full-map SnapshotActiveDrawables walk in OnFrame (measured
    // 4-5ms/frame in dense scenes); SnapshotActiveDrawables remains for
    // periodic diagnostics.
    void DrainDirtyPoses(std::unordered_map<uint64_t, std::array<float, 12>>& out);

    // Fill `out` with key -> (currentFrame - lastSeenFrame) for every tracked
    // worldspace LOD chunk drawable. O(chunks), called once per Remix frame
    // by OnFrame's stale-chunk filter: the engine fires GetRenderPasses every
    // frame for geometry that survives its culling, so a chunk whose age
    // grows is one the engine hid (its cells attached at full detail) and
    // must not be drawn over the streamed-in buildings. Also returns the
    // cumulative fire count so the caller can detect "scene not rendering"
    // states (pause menu / main menu) where every drawable stops firing and
    // staleness is meaningless.
    uint64_t SnapshotLodChunkAges(uint64_t currentFrame,
                                  std::unordered_map<uint64_t, uint64_t>& out);

    // Cumulative game-thread perf counters; consumers diff across reporting
    // windows. fires/fireNs cover the GetRenderPasses detour body (our
    // overhead only, not the engine's original). ticks/tickNs cover
    // Tick() -- one tick per hkPresent, so `ticks` doubles as a game-frame
    // counter for normalization.
    struct PerfCounters {
        uint64_t fires  = 0;
        uint64_t fireNs = 0;
        uint64_t ticks  = 0;
        uint64_t tickNs = 0;
    };
    PerfCounters GetPerfCounters();
}
