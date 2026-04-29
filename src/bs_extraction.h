#pragma once

#include "remix/remix_c.h"
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

    // Worldspace LOD chunk metadata (2026-04-28). When isLODChunk is true,
    // OnFrame applies a spatial filter: skip drawing if the player's world
    // position is INSIDE the chunk's coverage area (the in-cell static refs
    // are already rendering that region with full detail). chunkOriginXY
    // is the chunk's pivot in raw Beth coords; chunkExtent is its side
    // length in Beth units. Set by the resolver from the parent NiNode chain
    // (parent1.name == "chunk" with parent2.name == "4|8|16|32" for terrain
    // LOD; parent2.name == "obj" for object LOD).
    bool  isLODChunk    = false;
    float chunkOriginX  = 0.0f;
    float chunkOriginY  = 0.0f;
    float chunkExtent   = 0.0f;
};

struct CellInfo {
    uintptr_t cellPtr;
    uint32_t formID;
};

// ---------------------------------------------------------------------------
// Texture post-processing modes (shared between bs_extraction and skinning)
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
struct BSGeometry;
struct BSLightingShaderMaterialBase;
struct NiTexture;

namespace BsExtraction {
    // Returns the player's current parent cell pointer, or 0 if unavailable.
    // Cheap enough to call every frame for cell-change detection.
    uintptr_t GetPlayerCellPtr();

    // Reads the player's world position (TESObjectREFR::pos at +0xD0).
    // Outputs are left unchanged (default 0) when player is unavailable.
    void GetPlayerPosition(float& outX, float& outY, float& outZ);

    // Lightweight check: player exists, parentCell loaded, cell has objects,
    // and player's 3D root node is present.  Cheap enough to call every frame.
    bool IsPlayerCellReady();

    // Returns all cells currently loaded by the engine (from DataHandler::cellList).
    std::vector<CellInfo> GetLoadedCells();

    // Drop the internal texture cache (call on cell change if desired).
    void ClearTextureCache();

    // --- Shared helper functions (used by both bs_extraction.cpp and skinning.cpp) ---

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

    // Read alpha-test + alpha-blend state from the geometry's NiAlphaProperty
    // (effectState slot) and write it into mesh.alphaTest* / alphaBlend* fields.
    // Defaults: alphaTestEnabled=false, alphaTestType=7 (Always), alphaTestRef=128,
    // alphaBlendEnabled=false. Geo may be any BSGeometry-derived shape.
    void ExtractAlphaState(BSGeometry* geo, ExtractedMesh& mesh);
}
