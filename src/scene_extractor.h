#pragma once

#include "remix/remix_c.h"
#include "light_extractor.h"
#include <vector>
#include <array>
#include <cstdint>
#include <string>
#include <d3d11.h>

// ---------------------------------------------------------------------------
// Skinning constants
// ---------------------------------------------------------------------------
static constexpr uint32_t kMaxBonesPerSkeleton = 128;  // Remix API has no bone limit; game meshes go up to ~117
static constexpr uint32_t kBonesPerVertex = 4;          // Always 4 in FO4
static constexpr uint64_t kVertexFlag_Skinned = 0x4000000000000ULL; // bit 50

// ---------------------------------------------------------------------------
// NiTransformPadded — matches the verified 0x40 byte engine layout
// ---------------------------------------------------------------------------
struct NiTransformPadded {
    float rot[3][4];     // 3 rows x 4 floats (4th is SIMD padding), 48 bytes
    float translate[3];  // xyz, 12 bytes
    float scale;         // uniform scale, 4 bytes
};
static_assert(sizeof(NiTransformPadded) == 0x40, "NiTransformPadded must be 64 bytes");

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
    bool alphaTestEnabled;          // true if NiAlphaProperty has alpha test
    int alphaTestType;              // Remix/VkCompareOp value (7 = Always = no test)
    uint8_t alphaTestRef;           // Alpha reference value (0-255)
};

// ---------------------------------------------------------------------------
// ExtractedSkinnedMesh — skinned mesh data extracted from BSTriShape + BSSkin
// ---------------------------------------------------------------------------
struct ExtractedSkinnedMesh {
    // ---- Base mesh data (same fields as ExtractedMesh) ----
    uint64_t          hash;
    std::vector<remixapi_HardcodedVertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t          vertexCount = 0;
    uint32_t          indexCount = 0;
    uint64_t          diffuseTextureHash = 0;
    uint64_t          normalTextureHash = 0;
    uint64_t          roughnessTextureHash = 0;
    uint64_t          emissiveTextureHash = 0;
    float             emissiveColorR = 0.0f;
    float             emissiveColorG = 0.0f;
    float             emissiveColorB = 0.0f;
    float             emissiveIntensity = 0.0f;
    bool              alphaTestEnabled = false;
    int               alphaTestType = 7;
    uint8_t           alphaTestRef = 128;

    // ---- Skinning data (extracted once) ----
    std::vector<float>    blendWeights;    // flat: kBonesPerVertex * vertexCount floats
    std::vector<uint32_t> blendIndices;    // flat: kBonesPerVertex * vertexCount uint32s
    uint32_t              boneCount = 0;   // number of bones in this skeleton

    // Inverse bind pose transforms -- one per bone, extracted from BSSkin::BoneData.
    std::vector<NiTransformPadded> inverseBindPoses;  // boneCount entries

    // Live bone node pointers -- read each frame for current world transforms.
    std::vector<uintptr_t> boneNodePtrs;  // boneCount entries (NiAVObject*), many may be null

    // Bone world transform pointers -- from BSSkin::Instance+0x28 (boneWorldTransforms array).
    // Unlike boneNodePtrs, these are ALWAYS valid for every bone (BSFlattenedBoneTree).
    // Each points to a NiTransformPadded (0x40 bytes) containing the bone's current world transform.
    std::vector<uintptr_t> boneWorldTransformPtrs;  // boneCount entries, always non-null

    // Skeleton root pointer -- used for validity checking.
    uintptr_t skeletonRootPtr = 0;        // NiNode*

    // Owner reference -- the TESObjectREFR formID that owns this skinned mesh.
    uint32_t ownerFormID = 0;

    // ---- Per-frame computed bone transforms (updated every frame) ----
    std::vector<std::array<float, 12>> currentBoneTransforms;  // boneCount entries
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

    // Called every frame from the game thread to update bone transforms for all
    // tracked skinned meshes.  Reads bone world transforms from live game memory
    // and computes final bone matrices.
    void UpdateSkinnedBoneTransforms(std::vector<ExtractedSkinnedMesh>& skinnedMeshes);
}
