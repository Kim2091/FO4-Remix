#pragma once

#include "remix/remix_c.h"
#include <vector>
#include <array>
#include <cstdint>

// ---------------------------------------------------------------------------
// Skinning constants
// ---------------------------------------------------------------------------
inline constexpr uint32_t kMaxBonesPerSkeleton = 128;  // Remix API has no bone limit; game meshes go up to ~117
inline constexpr uint32_t kBonesPerVertex = 4;          // Always 4 in FO4
inline constexpr uint64_t kVertexFlag_Skinned = 0x4000000000000ULL; // bit 50

// ---------------------------------------------------------------------------
// NiTransformPadded -- matches the verified 0x40 byte engine layout
// ---------------------------------------------------------------------------
struct NiTransformPadded {
    float rot[3][4];     // 3 rows x 4 floats (4th is SIMD padding), 48 bytes
    float translate[3];  // xyz, 12 bytes
    float scale;         // uniform scale, 4 bytes
};
static_assert(sizeof(NiTransformPadded) == 0x40, "NiTransformPadded must be 64 bytes");

// ---------------------------------------------------------------------------
// ExtractedSkinnedMesh -- skinned mesh data extracted from BSTriShape + BSSkin
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
    bool              alphaBlendEnabled = false;  // mirrors ExtractedMesh field
    uint32_t          srcColorBlendFactor = 1;    // VK_BLEND_FACTOR_ONE
    uint32_t          dstColorBlendFactor = 0;    // VK_BLEND_FACTOR_ZERO

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

// Forward declarations for types used in Skinning API
struct BSTriShape;
struct ExtractedTexture;
struct ID3D11Device;

namespace Skinning {
    // Called every frame from the game thread to update bone transforms
    // for all tracked skinned meshes.  Reads bone world transforms from
    // live game memory and computes final bone matrices.
    void UpdateBoneTransforms(std::vector<ExtractedSkinnedMesh>& skinnedMeshes);

    // Extract a skinned BSTriShape. Called from WalkNode in scene_extractor.cpp.
    bool ExtractSkinnedTriShape(BSTriShape* shape, uint64_t baseHash,
                                std::vector<ExtractedSkinnedMesh>& out,
                                ID3D11Device* device,
                                std::vector<ExtractedTexture>& newTextures,
                                uint32_t ownerFormID);
}
