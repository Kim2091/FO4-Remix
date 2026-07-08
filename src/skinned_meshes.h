#pragma once

#include <cstdint>

struct BSTriShape;

// ---------------------------------------------------------------------------
// Skinned-mesh bone tracking (2026-07-08). Game-thread module: the lighting
// resolver registers a skinned drawable's bone sources (per-bone world-
// transform POINTERS from BSSkin::Instance::worldTransforms at +0x28 --
// valid even for BSFlattenedBoneTree bones whose NiNode slot is null --
// plus copied inverse bind poses from BSSkin::BoneData), and UpdateAndQueue,
// called once per SemanticCapture::Tick, reads the live bone world
// transforms, composes bind->world matrices, and queues them to
// RemixRenderer for the per-instance remixapi_InstanceInfoBoneTransformsEXT
// chain. The runtime GPU-skins the mesh in object space and re-skins when
// the bone set changes (boneHash-keyed, rtx_scene_manager.cpp:351).
//
// Composition math (conventions proven by the camera/merge DXBC work):
// engine NiTransforms are ROW-VECTOR maps (v' = v*M + t). Skinning applies
// the inverse bind first, then the bone world transform:
//   v_world = (v_bind * IB + ib_t) * BW + bw_t
//           = v_bind * (IB*BW) + (ib_t*BW + bw_t)
// remixapi_Transform is COLUMN-VECTOR row-major 3x4, so the linear part is
// transposed on output. The retired pre-1B skinning module composed these
// as column-vector matrices and applied the X/Y coordinate swap on the
// wrong side -- both errors are invisible at bind pose (IB*BW == identity
// there under ANY composition order, which is why T-poses always looked
// right) and wrong the moment bones animate. Bone matrices stay in Beth
// space (det > 0); the Beth->Remix mirror P lives in the instance
// transform so the runtime's mirrored-facing flip fires (same mechanism as
// the BatchedMirrorBase fix, b112e08).
//
// The engine's own skinning constant buffers are camera-relative (posAdjust
// precision trick) -- irrelevant here because matrices are built from the
// scene graph, not captured from shader constants.
// ---------------------------------------------------------------------------
namespace SkinnedMeshes {
    // Register (or refresh) the bone sources for a skinned drawable. Reads
    // BSGeometry::skinInstance (+0x140); every pointer hop is SEH-guarded.
    // Returns false when the skin instance isn't ready or fails validation
    // -- the caller retries next tick like any not-yet-ready resource.
    // outBoneCount receives the bone count for blend-index validation
    // (always <= 256, the remixapi bones-per-instance cap).
    // outFailReason (optional) receives a static string naming the failed
    // step on a false return -- the missing-heads investigation needs
    // Register's silent failure paths attributable per shape ([HeadDiag]).
    bool Register(uint64_t drawableHash, BSTriShape* shape, uint32_t& outBoneCount,
                  const char** outFailReason = nullptr);

    // [HeadDiag] one-shot dump of a registered drawable's bone entry: per
    // bone the LIVE world transform (pos/scale/rotDet), whether its NiNode
    // slot was null, and the inverse-bind translation/det. Separates "face
    // bone transforms read garbage" from "positions/weights wrong" for the
    // facegen corruption hunt. Capped at 4 shapes per session.
    void LogBones(uint64_t drawableHash, const char* label);

    // [FaceAnim] expressions probe (2026-07-08): mark ONE facegen head
    // drawable; UpdateAndQueue then periodically logs a few of its composed
    // bone translations. If they never change during dialogue, the engine
    // animates faces through something other than these bone worlds. First
    // caller wins; capped logging inside.
    void SetFaceProbe(uint64_t drawableHash);

    // Drop a drawable's bone tracking (wired into ReleaseDrawable).
    void OnDrawableReleased(uint64_t drawableHash);

    // Read live bone transforms for every registered drawable and queue the
    // composed matrix sets to RemixRenderer. Game thread, once per Tick.
    void UpdateAndQueue();

    // Drop everything (save load teardown).
    void Reset();
}
