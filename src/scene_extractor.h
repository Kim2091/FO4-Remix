#pragma once

#include "remix/remix_c.h"
#include "light_extractor.h"
#include "skinning.h"
#include <vector>
#include <array>
#include <cstdint>
#include <string>
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
    uint64_t emissiveTextureHash = 0;   // 0 = no glow map (from BSLightingShaderMaterialGlowmap)
    float emissiveColorR = 0.0f;        // Emissive color R from BSLightingShaderProperty (0-1)
    float emissiveColorG = 0.0f;        // Emissive color G
    float emissiveColorB = 0.0f;        // Emissive color B
    float emissiveIntensity = 0.0f;     // fEmitColorScale from BSLightingShaderProperty
    bool alphaTestEnabled = false;  // true if NiAlphaProperty has alpha test
    int alphaTestType = 7;          // Remix/VkCompareOp value (7 = Always = no test)
    uint8_t alphaTestRef = 128;     // Alpha reference value (0-255)

    // Alpha blend state (NiAlphaProperty bits 0, 1-4, 5-8 -> VkBlendFactor).
    // When alphaBlendEnabled is true, Remix applies per-instance blend via
    // InstanceInfoBlendEXT chained at DrawInstance time. Material flips
    // opaqueExt.useDrawCallAlphaState=1 so the instance state wins over
    // the material-level alpha test defaults.
    bool alphaBlendEnabled = false;
    uint32_t srcColorBlendFactor = 1;  // VK_BLEND_FACTOR_ONE
    uint32_t dstColorBlendFactor = 0;  // VK_BLEND_FACTOR_ZERO
};

struct ExtractionResult {
    std::vector<ExtractedMesh> meshes;
    std::vector<ExtractedSkinnedMesh> skinnedMeshes;  // Skinned meshes with blend data
    std::vector<ExtractedTexture> textures;  // Unique textures only
    std::vector<ExtractedLight> lights;      // Placed lights from the cell
};

struct CellInfo {
    uintptr_t cellPtr;
    uint32_t formID;
};

// ---------------------------------------------------------------------------
// Texture post-processing modes (shared between scene_extractor and skinning)
// ---------------------------------------------------------------------------
enum class TexturePostProcess { None, InvertRGB, Octahedral };

// ---------------------------------------------------------------------------
// Common vertex/index extraction result -- shared between static and skinned paths
// ---------------------------------------------------------------------------
struct ParsedGeometry {
    std::vector<remixapi_HardcodedVertex> vertices;
    std::vector<uint32_t> indices;
    uint64_t vertexDesc;
    uint16_t vertexSize;
    uint8_t* vbData;        // raw vertex buffer pointer (for blend weight reading)
    bool isDynamic;
};

// Forward declarations for F4SE types used in shared function signatures
struct BSTriShape;
struct BSLightingShaderMaterialBase;
struct NiTexture;

namespace SceneExtractor {
    // Returns the player's current parent cell pointer, or 0 if unavailable.
    // Cheap enough to call every frame for cell-change detection.
    uintptr_t GetPlayerCellPtr();

    // Reads the player's world position (TESObjectREFR::pos at +0xD0).
    // Outputs are left unchanged (default 0) when player is unavailable.
    void GetPlayerPosition(float& outX, float& outY, float& outZ);

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

    // --- Shared helper functions (used by both scene_extractor.cpp and skinning.cpp) ---

    // Parse vertices and indices from a BSTriShape. Returns false if the shape
    // should be skipped (effect shader, missing data, NaN positions, bad indices).
    bool ParseShapeGeometry(BSTriShape* shape, ParsedGeometry& out, bool logRejections = true);

    // Get the BSLightingShaderMaterialBase from a shape, or nullptr
    BSLightingShaderMaterialBase* GetLightingMaterial(BSTriShape* shape);

    // Generic texture extraction from any NiTexture slot
    uint64_t ExtractMaterialTexture(NiTexture* tex, const char* slotName,
                                    ID3D11Device* device,
                                    std::vector<ExtractedTexture>& newTextures,
                                    TexturePostProcess postProcess = TexturePostProcess::None);

    // Extract emissive data from a shape's shader property and material
    void ExtractEmissiveData(BSTriShape* shape, BSLightingShaderMaterialBase* lightingMat,
                             ID3D11Device* device, std::vector<ExtractedTexture>& newTextures,
                             uint64_t& outTexHash, float& outR, float& outG, float& outB, float& outIntensity);
}
