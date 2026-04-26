#pragma once

#include "camera.h"
#include "scene_extractor.h"
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

// Tracks a skinned mesh that has been created in Remix
struct SkinnedMeshInstance {
    remixapi_MeshHandle      meshHandle = nullptr;
    remixapi_MaterialHandle  materialHandle = nullptr;
    uint64_t                 meshHash = 0;
    uint32_t                 boneCount = 0;
    uint32_t                 ownerFormID = 0;
    bool                     isValid = false;
    uint64_t                 materialHash = 0;       // Index into g_materialCache (LRU)
    std::unordered_set<uint64_t> textureHashes;      // Textures used by this mesh's material
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

    bool Init();
    void OnFrame(const CameraState& cam,
                 const std::vector<ExtractedSkinnedMesh>& skinnedMeshBoneData,
                 const OverlayData& overlay = {});
    void Shutdown();

    // Upload textures, create materials, and load meshes for a specific cell.
    // Called on the remix thread.
    void LoadCellScene(uint32_t cellFormID, ExtractionResult&& result);

    // Destroy all Remix handles for a specific cell.
    void UnloadCell(uint32_t cellFormID);

    // Destroy all Remix handles for all cells.
    void UnloadAllCells();
}
