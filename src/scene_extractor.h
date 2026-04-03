#pragma once

#include "remix/remix_c.h"
#include "light_extractor.h"
#include <vector>
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
    uint64_t diffuseTextureHash;  // 0 = no texture
    uint64_t normalTextureHash;   // 0 = no texture (future)
};

struct ExtractionResult {
    std::vector<ExtractedMesh> meshes;
    std::vector<ExtractedTexture> textures;  // Unique textures only
    std::vector<ExtractedLight> lights;      // Placed lights from the cell
};

namespace SceneExtractor {
    // Lightweight check: player exists, parentCell loaded, cell has objects,
    // and player's 3D root node is present.  Cheap enough to call every frame.
    bool IsPlayerCellReady();

    // Extract all BSTriShape meshes and their diffuse textures from the
    // player's current cell.  Must be called on the main thread.
    // |device| is used for GPU texture readback (staging copies).
    ExtractionResult ExtractPlayerCell(ID3D11Device* device);

    // Drop the internal texture cache (call on cell change if desired).
    void ClearTextureCache();
}
