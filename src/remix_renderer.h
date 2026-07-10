#pragma once

#include "camera.h"
#include "bs_extraction.h"
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

struct OverlayData {
    std::vector<uint8_t> pixels;  // tightly packed RGBA/BGRA, 4 bpp
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t dxgiFormat = 0;      // DXGI_FORMAT of the captured backbuffer
    bool valid = false;
};

namespace RemixRenderer {
    struct VramStats {
        uint64_t totalAllocatedBytes              = 0;
        uint64_t totalUsedBytes                   = 0;
        uint64_t poolRetainedBytes                = 0;
        uint64_t usedReplacementGeometryBytes     = 0;
        uint64_t usedBufferBytes                  = 0;
        uint64_t usedAccelerationStructureBytes   = 0;
        uint64_t usedOpacityMicromapBytes         = 0;
        uint64_t usedMaterialTextureBytes         = 0;  // <-- the field SweepStale* reads
        uint64_t usedRenderTargetBytes            = 0;
        uint64_t driverAllocatedBytes             = 0;
        uint64_t driverBudgetBytes                = 0;
        uint32_t forkTextureCacheCount            = 0;
    };
    bool GetVramStats(VramStats* out);

    struct StaleMaterialSweepResult {
        uint32_t materialCacheCount = 0;
        uint32_t staleMaterialCount = 0;
        uint32_t cellsEvicted       = 0;  // Always 0 post-Phase-1B (cells retired). Kept for ABI compat with the periodic stats logger.
    };
    StaleMaterialSweepResult SweepStaleMaterials(uint64_t currentFrameIndex,
                                                 uint64_t ttlFrames,
                                                 uint64_t budgetBytes,
                                                 uint64_t currentMaterialTexBytes);

    struct StaleTextureSweepResult {
        uint32_t textureHandleCount     = 0;
        uint32_t staleTextureCount      = 0;
        uint32_t cellsEvicted           = 0;  // Always 0 post-Phase-1B (cells retired). Kept for ABI compat with the periodic stats logger.
        uint32_t budgetEvictions        = 0;
        uint32_t orphanTexturesDestroyed = 0;
    };
    StaleTextureSweepResult SweepStaleTextures(uint64_t currentFrameIndex,
                                               uint64_t ttlFrames,
                                               uint64_t budgetBytes,
                                               uint64_t currentMaterialTexBytes);

    bool Init();
    void OnFrame(const CameraState& cam,
                 const OverlayData& overlay = {});
    void Shutdown();

    enum class SubmitStatus {
        kSubmitted,   // mesh + material handles created, drawable in g_drawables
        kFailed       // rejected (e.g. mesh creation failed); caller may retry
    };

    // Per-drawable submission, idempotent on `hash`. Walks g_textureHandles +
    // g_materialCache (creating cache entries as needed). Stores the resulting
    // mesh handle + material refcount in g_drawables.
    //
    // Called from semantic_capture's resolve loop on the Remix thread.
    SubmitStatus SubmitDrawable(uint64_t hash,
                                const ExtractedMesh& mesh,
                                const std::vector<ExtractedTexture>& newTextures);

    // Release the drawable identified by hash: destroy its mesh handle,
    // decrement material refcount (destroy when 0, cascading texture refcount
    // decrements). Idempotent on missing hash.
    //
    // Called from semantic_capture's TTL eviction path on the Remix thread.
    void ReleaseDrawable(uint64_t hash);

    // Forward a key/value to Remix's runtime config registry. Takes the
    // recursive Remix-API mutex so concurrent OnFrame draw submissions
    // don't race against the option write. Returns true on success;
    // false if the Remix fork lacks the slot or the value fails to parse.
    //
    // WARNING: blocks on g_remixApiMutex, which OnFrame holds for its entire
    // frame (including the multi-ms Present). Never call from the game thread
    // -- unfair lock acquisition against a ~95% duty-cycle holder starves the
    // caller for seconds (observed as multi-second game freezes that only an
    // alt-tab's scheduling perturbation would break). Game-thread callers must
    // use QueueConfigVariable instead.
    bool SetConfigVariable(const char* key, const char* value);

    // Non-blocking config write for game-thread callers: stores the pair in a
    // small pending map (last write per key wins) under a dedicated lightweight
    // mutex; OnFrame drains the map on the Remix thread while it already holds
    // the Remix-API mutex. Failures (key not registered in the fork) are
    // logged once per key at the drain site, then silenced.
    void QueueConfigVariable(const char* key, const char* value);

    // Queue a full snapshot of the loaded cells' placed lights from the game
    // thread (same never-block-on-the-API-mutex contract as
    // QueueConfigVariable). OnFrame drains it on the Remix thread, diffs by
    // hash against the live light handles, and draws every light each frame.
    void QueueLights(std::vector<ExtractedLight>&& lights);

    // Queue composed bind->world bone matrix sets for skinned drawables from
    // the game thread (SkinnedMeshes::UpdateAndQueue, once per Tick; same
    // never-block-on-the-API-mutex contract as QueueLights). OnFrame drains
    // into the matching DrawableInstances and chains
    // remixapi_InstanceInfoBoneTransformsEXT on their draws.
    void QueueBoneTransforms(
        std::unordered_map<uint64_t, std::vector<remixapi_Transform>>&& bones);

    // True if a Remix-side texture handle currently exists for `hash`.
    // Used by the extraction cache to decide whether a cache hit must
    // re-supply pixel data so SubmitDrawable can recreate a handle that was
    // destroyed (PreLoadGame release wave, orphan LRU sweep). Takes
    // g_renderStateMutex briefly; callers may hold g_drawableMutex
    // (drawable -> renderState is the established lock order).
    bool HasTextureHandle(uint64_t hash);
}
