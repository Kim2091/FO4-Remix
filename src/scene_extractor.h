#pragma once

#include "remix/remix_c.h"
#include "light_extractor.h"
#include <vector>
#include <array>
#include <cstdint>
#include <d3d11.h>

struct ExtractedTexture {
    uint64_t hash;
    uint32_t width;
    uint32_t height;
    DXGI_FORMAT dxgiFormat;
    std::vector<uint8_t> pixels;  // Raw mip 0 data (may be block-compressed)
};

struct ExtractedMesh {
    uint64_t hash;
    std::vector<remixapi_HardcodedVertex> vertices;
    std::vector<uint32_t> indices;
    float worldTransform[3][4]; // row-major 3x4 for remixapi_Transform
    uint64_t diffuseTextureHash;    // 0 = no texture
    uint64_t normalTextureHash;     // 0 = no texture
    uint64_t roughnessTextureHash;  // 0 = no texture (FO4 smoothness/spec mask)
    bool alphaTestEnabled;          // true if NiAlphaProperty has alpha test
    int alphaTestType;              // Remix/VkCompareOp value (7 = Always = no test)
    uint8_t alphaTestRef;           // Alpha reference value (0-255)

    // Skinning data (empty if not skinned)
    uint32_t bonesPerVertex = 0;              // 0 = not skinned, typically 4
    std::vector<float> blendWeights;          // bonesPerVertex floats per vertex
    std::vector<uint32_t> blendIndices;       // bonesPerVertex uint32s per vertex
    uint32_t boneCount = 0;                   // number of bones in the skeleton
    // Inverse bind-pose transforms — cached for per-frame bone matrix computation.
    // Each is a 3x4 row-major matrix (same layout as remixapi_Transform::matrix),
    // already in Remix coordinate space (X/Y swapped).
    std::vector<std::array<float, 12>> inverseBindPose;
};

struct ExtractionResult {
    std::vector<ExtractedMesh> meshes;
    std::vector<ExtractedTexture> textures;  // Unique textures only
    std::vector<ExtractedLight> lights;      // Placed lights from the cell
};

struct CellInfo {
    uintptr_t cellPtr;
    uint32_t formID;
};

namespace SceneExtractor {
    // Returns the player's current parent cell pointer, or 0 if unavailable.
    // Cheap enough to call every frame for cell-change detection.
    uintptr_t GetPlayerCellPtr();

    // Lightweight check: player exists, parentCell loaded, cell has objects,
    // and player's 3D root node is present.  Cheap enough to call every frame.
    bool IsPlayerCellReady();

    // Extract all BSTriShape meshes and their diffuse textures from the
    // player's current cell.  Must be called on the main thread.
    // |device| is used for GPU texture readback (staging copies).
    ExtractionResult ExtractPlayerCell(ID3D11Device* device);

    // Returns all cells currently loaded by the engine (from DataHandler::cellList).
    std::vector<CellInfo> GetLoadedCells();

    // Extract all geometry from a specific cell. Must be called on the main thread.
    ExtractionResult ExtractCell(uintptr_t cellPtr, ID3D11Device* device);

    // Drop the internal texture cache (call on cell change if desired).
    void ClearTextureCache();
}
