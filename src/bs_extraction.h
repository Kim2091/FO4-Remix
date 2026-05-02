#pragma once

#include "remix/remix_c.h"
#include <vector>
#include <array>
#include <cstdint>
#include <string>
#include <d3d11.h>

struct ExtractedTexture {
    uint64_t hash;
    uint32_t width;            // Mip 0 dimensions
    uint32_t height;
    DXGI_FORMAT dxgiFormat;
    uint32_t mipLevels = 1;    // Number of mip levels packed in `pixels`. >=1.
    std::vector<uint8_t> pixels;  // Tightly packed mip chain: mip0, mip1, ... mipN-1.
                                  // Each mip's dimensions are max(1, w>>i) and max(1, h>>i).
                                  // Format is uniform across all mips (post-decompression
                                  // and post-process this is RGBA8 for BC source textures).
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

    // Decal tag (set by lighting_static resolver from BSLightingShaderProperty
    // shader-flag bit). When true, OnFrame ORs REMIXAPI_INSTANCE_CATEGORY_BIT_
    // DECAL_STATIC into the instance categoryFlags so dxvk-remix applies decal
    // depth-offset Z-fight prevention.
    bool isDecal = false;

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

    // Water tag (2026-04-29). When true, SubmitDrawable builds the material
    // with MaterialInfoTranslucentEXT instead of MaterialInfoOpaqueEXT, so
    // the path tracer treats the surface as a refractive medium (Fresnel
    // reflections + transmittance through to whatever's below). Set only by
    // the water resolver; the renderer is otherwise shader-agnostic.
    // waterTransmittance{R,G,B} are pulled from BSWaterShaderMaterial's
    // kDeepColor at extraction time and become the transmittanceColor of
    // the translucent material -- this lets per-worldspace water tinting
    // (Far Harbor swamp green vs Sanctuary blue vs Glowing Sea sludge)
    // come through automatically from the .esm-authored color slots.
    bool  isWater              = false;
    float waterTransmittanceR  = 0.0f;
    float waterTransmittanceG  = 0.0f;
    float waterTransmittanceB  = 0.0f;
};

struct CellInfo {
    uintptr_t cellPtr;
    uint32_t formID;
};

// ---------------------------------------------------------------------------
// Texture post-processing modes (shared between bs_extraction and skinning)
// ---------------------------------------------------------------------------
// Texture post-processing modes applied during ExtractMaterialTexture.
//
// - None: pass-through, preserve source format (BC1/BC3/BC7 stay compressed,
//   RGBA8 stays uncompressed). Used for plain diffuse textures.
//
// - InvertRGB: smoothness->roughness conversion. RGB channels are inverted so
//   smoothness=1 (mirror) becomes roughness=0. Decompresses BC1/BC3/BC5 to
//   RGBA8 first; BC7/other passes through unchanged. Used for the
//   spSmoothnessSpecMaskTexture slot.
//
// - Octahedral: tangent-space normal -> hemispherical octahedral encoding.
//   Decompresses BC1/BC3/BC5 to RGBA8 first; BC7/other passes through.
//   Used for the spNormalTexture slot.
//
// - DiffuseAlphaFromLuminance: ONLY for BC1 source format -- decompress to
//   RGBA8 and synthesize alpha = max(R, G, B) so the cutout regions of an
//   atlas (near-black RGB) become transparent. Bethesda's LOD foliage
//   atlases (e.g. Commonwealth.Objects.DDS) are stored as BC1 with no
//   alpha channel; vanilla DX11's rasterizer hides this because cutout
//   regions render as dark blobs that blend into the distance, but Remix's
//   path tracer applies our converted-from-smoothness roughness map at
//   those pixels and produces mirror-reflective rectangles where vanilla
//   showed dark. Synthesizing alpha from luminance gives the path tracer a
//   real alpha channel to test against.
//   Non-BC1 inputs (BC3/BC7/RGBA8) are passed through unchanged because
//   they already have a usable alpha channel. Used for diffuse on
//   alpha-tested or alpha-blended geometry.
// (See full comment block above.)
//
// - DiffuseAlphaFromLuminanceForceBC3: like DiffuseAlphaFromLuminance but
//   also overrides BC3.a on BC3 inputs. Used for decal-tagged surfaces
//   specifically, where BGS sometimes packs non-cutout data in the BC3
//   alpha channel so the authored alpha doesn't behave as a clean mask.
enum class TexturePostProcess { None, InvertRGB, Octahedral, DiffuseAlphaFromLuminance, DiffuseAlphaFromLuminanceForceBC3 };

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

    // Generic texture extraction from any NiTexture slot.
    //
    // minRoughness: when non-zero AND postProcess == InvertRGB, clamps the
    // resulting RGB channels (the roughness output) to >= this value. Used
    // for decal surfaces, where Bethesda's smoothness map is often set to
    // "very smooth" which after InvertRGB becomes roughness near 0 (mirror)
    // -- vanilla DX11 hides this with specular highlights, but the path
    // tracer treats it as a literal mirror. Clamping to ~30% roughness
    // keeps glossy variation while preventing decals from being more
    // reflective than a polished surface should be.
    uint64_t ExtractMaterialTexture(NiTexture* tex, const char* slotName,
                                    ID3D11Device* device,
                                    std::vector<ExtractedTexture>& newTextures,
                                    TexturePostProcess postProcess = TexturePostProcess::None,
                                    uint8_t minRoughness = 0);

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
