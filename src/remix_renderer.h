#pragma once

#include "camera.h"
#include "bs_extraction.h"
#include <vector>
#include <cstdint>
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
}
