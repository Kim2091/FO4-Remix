#include "skinning.h"
#include "scene_extractor.h"
#include "config.h"

#include "f4se_common/f4se_version.h"
#include "f4se_common/Relocation.h"
#include "f4se/PluginAPI.h"
#include "f4se/NiNodes.h"
#include "f4se/NiObjects.h"
#include "f4se/NiTypes.h"
#include "f4se/BSGeometry.h"
#include "f4se/GameTypes.h"
#include "f4se/NiProperties.h"
#include "f4se/NiMaterials.h"
#include "f4se/NiTextures.h"

#include <d3d11.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// Step 3: Read BSSkin data from a BSGeometry node (raw pointer access)
// ---------------------------------------------------------------------------
static bool ReadSkinData(uintptr_t bsGeometryPtr, ExtractedSkinnedMesh& out) {
    // 1. Read BSSkin::Instance pointer from BSGeometry+0x140
    uintptr_t skinInstPtr = *reinterpret_cast<uintptr_t*>(bsGeometryPtr + 0x140);
    if (skinInstPtr == 0) {
        _MESSAGE("FO4RemixPlugin: [SKINNING] skinInstance is null at BSGeometry 0x%p", (void*)bsGeometryPtr);
        return false;
    }

    // 2. Read bone count from boneNodes BSTArray at skinInst+0x10
    //    BSTArray: data* at +0x00, capacity(uint32) at +0x08, count(uint32) at +0x10
    uintptr_t boneNodesArrayPtr = *reinterpret_cast<uintptr_t*>(skinInstPtr + 0x10);
    uint32_t boneCount = *reinterpret_cast<uint32_t*>(skinInstPtr + 0x10 + 0x10);

    if (boneCount == 0) {
        _MESSAGE("FO4RemixPlugin: [SKINNING] boneCount is 0 at skinInst 0x%p", (void*)skinInstPtr);
        return false;
    }
    if (boneCount > kMaxBonesPerSkeleton) {
        _MESSAGE("FO4RemixPlugin: [SKINNING] boneCount %u exceeds max %u at skinInst 0x%p",
                 boneCount, kMaxBonesPerSkeleton, (void*)skinInstPtr);
        return false;
    }
    if (boneNodesArrayPtr == 0) {
        _MESSAGE("FO4RemixPlugin: [SKINNING] boneNodesArray is null at skinInst 0x%p", (void*)skinInstPtr);
        return false;
    }

    // 3. Read boneData pointer from skinInst+0x40
    uintptr_t boneDataPtr = *reinterpret_cast<uintptr_t*>(skinInstPtr + 0x40);
    if (boneDataPtr == 0) {
        _MESSAGE("FO4RemixPlugin: [SKINNING] boneData is null at skinInst 0x%p", (void*)skinInstPtr);
        return false;
    }

    // 4. Read skeletonRoot pointer from skinInst+0x48
    uintptr_t skelRootPtr = *reinterpret_cast<uintptr_t*>(skinInstPtr + 0x48);
    out.skeletonRootPtr = skelRootPtr;

    // 5. Read inverse bind poses from boneData+0x10 NiTArray
    //    Array data pointer at boneData+0x10+0x00
    //    Each entry is 0x50 bytes: 0x10 NiBound + 0x40 NiTransform
    uintptr_t invBindArrayPtr = *reinterpret_cast<uintptr_t*>(boneDataPtr + 0x10);
    if (invBindArrayPtr == 0) {
        _MESSAGE("FO4RemixPlugin: [SKINNING] invBindArray is null at boneData 0x%p", (void*)boneDataPtr);
        return false;
    }

    out.boneCount = boneCount;
    out.inverseBindPoses.resize(boneCount);
    out.boneNodePtrs.resize(boneCount);
    out.boneWorldTransformPtrs.resize(boneCount);

    // 6. Read boneWorldTransforms array at skinInst+0x28 (BSTArray)
    //    This array ALWAYS has valid pointers for every bone, even when boneNodes has nulls.
    //    For bones with NiNodes: points to NiAVObject+0x70 (the worldTransform)
    //    For bones without NiNodes (BSFlattenedBoneTree): points to the flat bone array entry
    uintptr_t boneWorldTransformArrayPtr = *reinterpret_cast<uintptr_t*>(skinInstPtr + 0x28);

    uint32_t nullBoneCount = 0;
    uint32_t nullTransformCount = 0;
    for (uint32_t i = 0; i < boneCount; i++) {
        // Inverse bind pose: skip 0x10 NiBound, read 0x40 NiTransform
        uintptr_t entryPtr = invBindArrayPtr + i * 0x50 + 0x10;
        memcpy(&out.inverseBindPoses[i], reinterpret_cast<void*>(entryPtr), sizeof(NiTransformPadded));

        // Bone node pointer (may be null for BSFlattenedBoneTree bones)
        uintptr_t boneNodePtr = reinterpret_cast<uintptr_t*>(boneNodesArrayPtr)[i];
        out.boneNodePtrs[i] = boneNodePtr;
        if (boneNodePtr == 0) nullBoneCount++;

        // Bone world transform pointer (always valid)
        uintptr_t transformPtr = boneWorldTransformArrayPtr
            ? reinterpret_cast<uintptr_t*>(boneWorldTransformArrayPtr)[i]
            : 0;
        out.boneWorldTransformPtrs[i] = transformPtr;
        if (transformPtr == 0) nullTransformCount++;
    }

    // Pre-allocate per-frame transform storage
    out.currentBoneTransforms.resize(boneCount);

    _MESSAGE("FO4RemixPlugin: [SKINNING] ReadSkinData OK: bones=%u skelRoot=0x%p nullNodes=%u nullTransforms=%u",
             boneCount, (void*)skelRootPtr, nullBoneCount, nullTransformCount);

    // Log first 3 bones for debug
    for (uint32_t i = 0; i < boneCount && i < 3; i++) {
        const auto& ib = out.inverseBindPoses[i];
        _MESSAGE("FO4RemixPlugin: [SKINNING]   Bone[%u]: node=0x%p invBind rot=[%.3f,%.3f,%.3f / %.3f,%.3f,%.3f / %.3f,%.3f,%.3f] "
                 "trans=[%.1f,%.1f,%.1f] scale=%.3f",
                 i, (void*)out.boneNodePtrs[i],
                 ib.rot[0][0], ib.rot[0][1], ib.rot[0][2],
                 ib.rot[1][0], ib.rot[1][1], ib.rot[1][2],
                 ib.rot[2][0], ib.rot[2][1], ib.rot[2][2],
                 ib.translate[0], ib.translate[1], ib.translate[2], ib.scale);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Step 5: Bone transform computation helpers
// ---------------------------------------------------------------------------

// Read an NiAVObject's world transform from game memory (at +0x70)
static NiTransformPadded ReadWorldTransform(uintptr_t niAVObjectPtr) {
    NiTransformPadded t;
    memcpy(&t, reinterpret_cast<void*>(niAVObjectPtr + 0x70), sizeof(NiTransformPadded));
    return t;
}

// Compute: result = boneWorld * invBind  (affine 3x4 matrix multiply)
// Both inputs are NiTransformPadded; output is a 3x4 row-major matrix for Remix.
static void ComputeBoneMatrix(
    const NiTransformPadded& boneWorld,
    const NiTransformPadded& invBind,
    float outMatrix[3][4])
{
    // Build effective 3x3 for boneWorld: rot[r][c] * scale
    float A[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            A[r][c] = boneWorld.rot[r][c] * boneWorld.scale;

    // Build effective 3x3 for invBind: rot[r][c] * scale
    float B[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            B[r][c] = invBind.rot[r][c] * invBind.scale;

    // Result rotation = A * B (3x3 matrix multiply)
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            outMatrix[r][c] = A[r][0] * B[0][c]
                            + A[r][1] * B[1][c]
                            + A[r][2] * B[2][c];
        }
    }

    // Result translation = A * invBind.translate + boneWorld.translate
    for (int r = 0; r < 3; r++) {
        outMatrix[r][3] = A[r][0] * invBind.translate[0]
                        + A[r][1] * invBind.translate[1]
                        + A[r][2] * invBind.translate[2]
                        + boneWorld.translate[r];
    }
}

// Apply the same coordinate system swap as static meshes and the camera.
// Both use a column swap on rotation (R*S) and component swap on translation (S*T).
// This must match exactly or skinned meshes will rotate incorrectly relative to the world.
static void ApplyCoordinateSwap(float matrix[3][4]) {
    // Swap COLUMNS 0 and 1 in the 3x3 rotation part (= right-multiply by swap matrix S).
    // This matches the static mesh path: worldTransform[r][0] = rot[r][1], [r][1] = rot[r][0].
    for (int r = 0; r < 3; r++) {
        std::swap(matrix[r][0], matrix[r][1]);
    }
    // Swap X/Y in translation, same as static meshes: pos.y -> [0][3], pos.x -> [1][3].
    std::swap(matrix[0][3], matrix[1][3]);
}

// Diagnostic: dump bone 0 matrices for the first skinned mesh, then auto-disable.
// Fires every ~120 frames while logBoneDiag is true, so you can capture multiple orientations.
static void DumpBoneDiagnostic(const NiTransformPadded& boneWorld,
                                const NiTransformPadded& invBind,
                                const float preSwap[3][4],
                                const float postSwap[3][4]) {
    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] ======== Bone 0 Diagnostic ========");
    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] boneWorld rot:");
    for (int r = 0; r < 3; r++)
        _MESSAGE("FO4RemixPlugin: [BONE_DIAG]   [%d] %+.6f %+.6f %+.6f", r,
                 boneWorld.rot[r][0], boneWorld.rot[r][1], boneWorld.rot[r][2]);
    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] boneWorld translate: %+.3f %+.3f %+.3f  scale: %.4f",
             boneWorld.translate[0], boneWorld.translate[1], boneWorld.translate[2], boneWorld.scale);

    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] invBind rot:");
    for (int r = 0; r < 3; r++)
        _MESSAGE("FO4RemixPlugin: [BONE_DIAG]   [%d] %+.6f %+.6f %+.6f", r,
                 invBind.rot[r][0], invBind.rot[r][1], invBind.rot[r][2]);
    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] invBind translate: %+.3f %+.3f %+.3f  scale: %.4f",
             invBind.translate[0], invBind.translate[1], invBind.translate[2], invBind.scale);

    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] computed (pre-swap):");
    for (int r = 0; r < 3; r++)
        _MESSAGE("FO4RemixPlugin: [BONE_DIAG]   [%d] %+.6f %+.6f %+.6f | %+.3f", r,
                 preSwap[r][0], preSwap[r][1], preSwap[r][2], preSwap[r][3]);

    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] final (post-swap):");
    for (int r = 0; r < 3; r++)
        _MESSAGE("FO4RemixPlugin: [BONE_DIAG]   [%d] %+.6f %+.6f %+.6f | %+.3f", r,
                 postSwap[r][0], postSwap[r][1], postSwap[r][2], postSwap[r][3]);
    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] ====================================");
}

// Called every frame to update bone transforms for all tracked skinned meshes.
void Skinning::UpdateBoneTransforms(std::vector<ExtractedSkinnedMesh>& skinnedMeshes) {
    static uint32_t s_diagFrameCounter = 0;
    bool doDiag = g_config.logBoneDiag && !skinnedMeshes.empty();
    bool diagThisFrame = doDiag && (s_diagFrameCounter++ % 120 == 0);

    for (auto& sm : skinnedMeshes) {
        // Safety: validate array sizes match boneCount
        if (sm.boneWorldTransformPtrs.size() < sm.boneCount ||
            sm.inverseBindPoses.size() < sm.boneCount ||
            sm.currentBoneTransforms.size() < sm.boneCount) {
            continue;
        }

        if (sm.boneCount == 0 || sm.boneCount > kMaxBonesPerSkeleton) {
            continue;
        }

        // Skeleton root validity check
        if (sm.skeletonRootPtr != 0) {
            __try {
                volatile uint8_t probe = *reinterpret_cast<uint8_t*>(sm.skeletonRootPtr);
                (void)probe;
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
        }

        for (uint32_t i = 0; i < sm.boneCount; i++) {
            float boneMatrix[3][4];

            // Use boneWorldTransformPtrs -- always valid for every bone,
            // even when boneNodePtrs[i] is null (BSFlattenedBoneTree).
            // Each pointer points directly to a NiTransformPadded (0x40 bytes).
            uintptr_t transformPtr = sm.boneWorldTransformPtrs[i];

            if (transformPtr == 0) {
                // Fallback: identity (should never happen with boneWorldTransforms)
                memset(boneMatrix, 0, sizeof(boneMatrix));
                boneMatrix[0][0] = 1.0f;
                boneMatrix[1][1] = 1.0f;
                boneMatrix[2][2] = 1.0f;
            } else {
                __try {
                    // Read the bone's current world transform directly
                    NiTransformPadded boneWorld;
                    memcpy(&boneWorld, reinterpret_cast<void*>(transformPtr), sizeof(NiTransformPadded));

                    ComputeBoneMatrix(boneWorld, sm.inverseBindPoses[i], boneMatrix);

                    // Diagnostic: capture pre-swap matrix for bone 0 of first mesh
                    if (diagThisFrame && i == 0 && &sm == &skinnedMeshes[0]) {
                        float preSwap[3][4];
                        memcpy(preSwap, boneMatrix, sizeof(preSwap));
                        ApplyCoordinateSwap(boneMatrix);
                        DumpBoneDiagnostic(boneWorld, sm.inverseBindPoses[0], preSwap, boneMatrix);
                    } else {
                        ApplyCoordinateSwap(boneMatrix);
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    memset(boneMatrix, 0, sizeof(boneMatrix));
                    boneMatrix[0][0] = 1.0f;
                    boneMatrix[1][1] = 1.0f;
                    boneMatrix[2][2] = 1.0f;
                    sm.boneWorldTransformPtrs[i] = 0;
                }
            }

            memcpy(sm.currentBoneTransforms[i].data(), boneMatrix, 12 * sizeof(float));
        }
    }
}

// Helper: dump BSDynamicTriShape memory layout (SEH-safe, no C++ objects)
static void DumpDynamicShapeLayout(uintptr_t shapeAddr, uint32_t szVertex) {
    _MESSAGE("FO4RemixPlugin: [DYNAMIC] shape=0x%p szVertex=%u probing offsets 0x170-0x1A8:",
             (void*)shapeAddr, szVertex);
    __try {
        for (uint32_t off = 0x170; off <= 0x1A8; off += 8) {
            uintptr_t val = *reinterpret_cast<uintptr_t*>(shapeAddr + off);
            _MESSAGE("FO4RemixPlugin: [DYNAMIC]   +0x%03X = 0x%016llX", off, val);
        }
        for (uint32_t off = 0x170; off <= 0x1A0; off += 4) {
            uint32_t val32 = *reinterpret_cast<uint32_t*>(shapeAddr + off);
            _MESSAGE("FO4RemixPlugin: [DYNAMIC]   +0x%03X (u32) = %u (0x%08X)", off, val32, val32);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        _MESSAGE("FO4RemixPlugin: [DYNAMIC]   exception reading shape memory");
    }
}

static void DumpDynamicVertexData(uint8_t* dynVerts, uint32_t posStride) {
    _MESSAGE("FO4RemixPlugin: [DYNAMIC] dynVerts=0x%p stride=%u first 16 bytes:", dynVerts, posStride);
    __try {
        _MESSAGE("FO4RemixPlugin: [DYNAMIC]   %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                 dynVerts[0], dynVerts[1], dynVerts[2], dynVerts[3],
                 dynVerts[4], dynVerts[5], dynVerts[6], dynVerts[7],
                 dynVerts[8], dynVerts[9], dynVerts[10], dynVerts[11],
                 dynVerts[12], dynVerts[13], dynVerts[14], dynVerts[15]);
        uint16_t* asHalf = reinterpret_cast<uint16_t*>(dynVerts);
        _MESSAGE("FO4RemixPlugin: [DYNAMIC]   as half: %.3f %.3f %.3f %.3f",
                 HalfToFloat(asHalf[0]), HalfToFloat(asHalf[1]),
                 HalfToFloat(asHalf[2]), HalfToFloat(asHalf[3]));
        float* asFloat = reinterpret_cast<float*>(dynVerts);
        _MESSAGE("FO4RemixPlugin: [DYNAMIC]   as f32:  %.3f %.3f %.3f %.3f",
                 asFloat[0], asFloat[1], asFloat[2], asFloat[3]);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        _MESSAGE("FO4RemixPlugin: [DYNAMIC]   exception reading dynVerts data");
    }
}

// ---------------------------------------------------------------------------
// Step 4: Extract a skinned BSTriShape -- base mesh + blend weights/indices + BSSkin
// ---------------------------------------------------------------------------
bool Skinning::ExtractSkinnedTriShape(BSTriShape* shape, uint64_t baseHash,
                                   std::vector<ExtractedSkinnedMesh>& out,
                                   ID3D11Device* device,
                                   std::vector<ExtractedTexture>& newTextures,
                                   uint32_t ownerFormID)
{
    // ---- Parse common vertex/index data ----
    ParsedGeometry geo;
    if (!SceneExtractor::ParseShapeGeometry(shape, geo))
        return false;

    // Dump BSDynamicTriShape memory layout (first dynamic skinned mesh only)
    if (geo.isDynamic) {
        uintptr_t shapeAddr = reinterpret_cast<uintptr_t>(shape);
        uint32_t szVertex = (geo.vertexDesc >> 4) & 0xF;

        static bool s_dumpedOnce = false;
        if (!s_dumpedOnce) {
            s_dumpedOnce = true;
            DumpDynamicShapeLayout(shapeAddr, szVertex);
        }

        uint8_t* dynVerts = *reinterpret_cast<uint8_t**>(shapeAddr + 0x180);
        if (dynVerts) {
            uint32_t posStride = szVertex * 4;
            DumpDynamicVertexData(dynVerts, posStride);
        }
    }

    // Compute blend weight/index offsets
    uint64_t desc = geo.vertexDesc;
    uint32_t blendWeightOffset = (uint32_t)((desc >> 26) & 0x3C);
    uint32_t blendIndexOffset  = blendWeightOffset + 8;

    const char* shapeName = shape->m_name.c_str();

    _MESSAGE("FO4RemixPlugin: [SKINNING] Extracting skinned shape \"%s\" verts=%u tris=%u bones offset: weight=%u index=%u dynamic=%d",
             shapeName ? shapeName : "<null>",
             shape->numVertices, shape->numTriangles,
             blendWeightOffset, blendIndexOffset, geo.isDynamic);

    ExtractedSkinnedMesh sm;
    sm.ownerFormID = ownerFormID;
    sm.vertexCount = shape->numVertices;
    sm.indexCount = shape->numTriangles * 3;

    // Generate a unique mesh hash (include ownerFormID for per-REFR uniqueness)
    sm.hash = FnvHashCombine(baseHash, FnvHash(shapeName ? shapeName : ""));
    sm.hash = FnvHashCombine(sm.hash, (uint64_t)0x534B494EULL); // "SKIN" tag

    // ---- Copy vertex/index data from parsed geometry ----
    sm.vertices = std::move(geo.vertices);
    sm.indices  = std::move(geo.indices);

    // Compute skinned mesh obj-space bounds
    float sMinPos[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float sMaxPos[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (uint16_t i = 0; i < shape->numVertices; i++) {
        const auto& pos = sm.vertices[i].position;
        for (int j = 0; j < 3; j++) {
            if (pos[j] < sMinPos[j]) sMinPos[j] = pos[j];
            if (pos[j] > sMaxPos[j]) sMaxPos[j] = pos[j];
        }
    }
    _MESSAGE("FO4RemixPlugin: [SKINNING] Mesh \"%s\" obj-space bounds: (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) extent=(%.1f,%.1f,%.1f)",
             shapeName ? shapeName : "<null>",
             sMinPos[0], sMinPos[1], sMinPos[2],
             sMaxPos[0], sMaxPos[1], sMaxPos[2],
             sMaxPos[0]-sMinPos[0], sMaxPos[1]-sMinPos[1], sMaxPos[2]-sMinPos[2]);

    // ---- Extract blend weights and blend indices ----
    sm.blendWeights.resize(sm.vertexCount * kBonesPerVertex);
    sm.blendIndices.resize(sm.vertexCount * kBonesPerVertex);

    float minWeightSum = FLT_MAX, maxWeightSum = -FLT_MAX;
    double totalWeightSum = 0.0;
    uint32_t badWeightCount = 0;
    uint32_t maxBoneIdx = 0;

    for (uint32_t i = 0; i < sm.vertexCount; i++) {
        uint32_t base = i * kBonesPerVertex;
        uint8_t* v = geo.vbData + (uint32_t)i * geo.vertexSize;

        // Read blend weights (HALF4)
        const uint16_t* hw = reinterpret_cast<const uint16_t*>(v + blendWeightOffset);
        float w0 = HalfToFloat(hw[0]);
        float w1 = HalfToFloat(hw[1]);
        float w2 = HalfToFloat(hw[2]);
        float w3 = 1.0f - w0 - w1 - w2;  // 4th weight is implicit
        if (w3 < 0.0f) w3 = 0.0f;

        sm.blendWeights[base + 0] = w0;
        sm.blendWeights[base + 1] = w1;
        sm.blendWeights[base + 2] = w2;
        sm.blendWeights[base + 3] = w3;

        // Validate weight sum
        float wSum = w0 + w1 + w2 + w3;
        if (wSum < minWeightSum) minWeightSum = wSum;
        if (wSum > maxWeightSum) maxWeightSum = wSum;
        totalWeightSum += wSum;
        if (fabsf(wSum - 1.0f) > 0.01f) badWeightCount++;

        // Read blend indices (R8G8B8A8)
        const uint8_t* bi = v + blendIndexOffset;
        sm.blendIndices[base + 0] = bi[0];
        sm.blendIndices[base + 1] = bi[1];
        sm.blendIndices[base + 2] = bi[2];
        sm.blendIndices[base + 3] = bi[3];

        for (int j = 0; j < 4; j++) {
            if (bi[j] > maxBoneIdx) maxBoneIdx = bi[j];
        }
    }

    _MESSAGE("FO4RemixPlugin: [SKINNING] Vertex weight stats: min_sum=%.4f max_sum=%.4f avg_sum=%.4f bad_count=%u/%u",
             minWeightSum, maxWeightSum, (float)(totalWeightSum / sm.vertexCount),
             badWeightCount, sm.vertexCount);
    _MESSAGE("FO4RemixPlugin: [SKINNING] Bone index range: [0, %u]", maxBoneIdx);

    // ---- Step 3: Read BSSkin data ----
    if (!ReadSkinData(reinterpret_cast<uintptr_t>(shape), sm)) {
        _MESSAGE("FO4RemixPlugin: [SKINNING] ReadSkinData failed for \"%s\"",
                 shapeName ? shapeName : "<null>");
        return false;
    }

    // Validate bone indices against actual bone count
    for (uint32_t i = 0; i < sm.vertexCount * kBonesPerVertex; i++) {
        if (sm.blendIndices[i] >= sm.boneCount) {
            _MESSAGE("FO4RemixPlugin: [SKINNING] Bone index %u >= boneCount %u, clamping to 0",
                     sm.blendIndices[i], sm.boneCount);
            sm.blendIndices[i] = 0;
        }
    }

    // ---- Extract textures ----
    BSLightingShaderMaterialBase* lightingMat = SceneExtractor::GetLightingMaterial(shape);
    sm.diffuseTextureHash   = lightingMat ? SceneExtractor::ExtractMaterialTexture(lightingMat->spDiffuseTexture, "diffuse", device, newTextures) : 0;
    sm.normalTextureHash    = lightingMat ? SceneExtractor::ExtractMaterialTexture(lightingMat->spNormalTexture, "normal", device, newTextures, TexturePostProcess::Octahedral) : 0;
    sm.roughnessTextureHash = lightingMat ? SceneExtractor::ExtractMaterialTexture(lightingMat->spSmoothnessSpecMaskTexture, "roughness", device, newTextures, TexturePostProcess::InvertRGB) : 0;

    // Extract emissive data (glow map texture + emissive color/scale)
    SceneExtractor::ExtractEmissiveData(shape, lightingMat, device, newTextures,
                        sm.emissiveTextureHash, sm.emissiveColorR, sm.emissiveColorG,
                        sm.emissiveColorB, sm.emissiveIntensity);

    // ---- Extract alpha test state ----
    sm.alphaTestEnabled = false;
    sm.alphaTestType = 7;
    sm.alphaTestRef = 128;
    NiProperty* alphaPropRaw = shape->effectState;
    if (alphaPropRaw) {
        NiAlphaProperty* alphaProp = static_cast<NiAlphaProperty*>(alphaPropRaw);
        bool testEnabled = (alphaProp->alphaFlags >> 9) & 1;
        if (testEnabled) {
            int niTestFunc = (alphaProp->alphaFlags >> 10) & 7;
            static const int niToVk[] = { 7, 1, 2, 3, 4, 5, 6, 0 };
            sm.alphaTestEnabled = true;
            sm.alphaTestType = niToVk[niTestFunc];
            sm.alphaTestRef = alphaProp->alphaThreshold;
        }
    }

    _MESSAGE("FO4RemixPlugin: [SKINNING] Extracted skinned mesh: hash=0x%016llX owner=0x%08X bones=%u vertices=%u indices=%u",
             (unsigned long long)sm.hash, ownerFormID, sm.boneCount, sm.vertexCount, sm.indexCount);

    out.push_back(std::move(sm));
    return true;
}
