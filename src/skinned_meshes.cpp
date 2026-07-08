#include "skinned_meshes.h"
#include "remix_renderer.h"

#include "f4se/PluginAPI.h"   // _MESSAGE
#include "f4se/NiTypes.h"     // NiTransform
#include "f4se/BSGeometry.h"  // BSTriShape

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t kMaxBones            = 256;  // REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT
constexpr uint32_t kMaxFaultsBeforeDrop = 8;
constexpr int      kLogCap              = 24;

// Raw offsets (F4SE BSSkin.h / BSGeometry.h, STATIC_ASSERT-anchored):
//   BSGeometry::skinInstance                    +0x140
//   BSSkin::Instance::bones (tArray)            +0x10  (count at +0x20)
//   BSSkin::Instance::worldTransforms (tArray)  +0x28  (count at +0x38)
//   BSSkin::Instance::boneData                  +0x40
//   BSSkin::BoneData::transforms (tArray)       +0x10  (count at +0x20)
//   BoneData entry stride 0x50: NiBound 0x10, then NiTransform 0x40
constexpr uintptr_t kOffSkinInstance   = 0x140;
constexpr uintptr_t kOffBonesArr       = 0x10;
constexpr uintptr_t kOffWorldXfArr     = 0x28;
constexpr uintptr_t kOffBoneData       = 0x40;
constexpr uintptr_t kOffBoneDataArr    = 0x10;
constexpr uintptr_t kTArrCountOff      = 0x10;   // tArray: entries +0, count +0x10
constexpr uintptr_t kBoneEntryStride   = 0x50;
constexpr uintptr_t kBoneEntryXfOff    = 0x10;

// POD mirror of NiTransform (NiMatrix43 0x30 + NiPoint3 0x0C + scale 0x04).
// Used instead of the F4SE class because the minimal f4se lib doesn't
// compile NiPoint3's out-of-line default ctor (vector::resize needs it).
struct XfPod {
    float rot[3][4];  // row-vector rotation rows; 4th column is pad
    float pos[3];
    float scale;
};
static_assert(sizeof(XfPod) == 0x40, "must mirror NiTransform layout");

struct Entry {
    std::vector<uintptr_t> boneXfPtrs;  // -> live NiTransform (0x40) per bone
    std::vector<XfPod>     invBinds;    // copied at registration
    std::vector<uint8_t>   nodeNull;    // bones[i] NiNode* was null (flattened-tree bone)
    uint32_t faults = 0;
};

// Registry is game-thread-owned (resolver + Tick), but ReleaseDrawable call
// sites span sweep paths -- keep it mutex-guarded regardless; contention is
// negligible at actor counts.
std::mutex g_mx;
std::unordered_map<uint64_t, Entry> g_entries;

std::atomic<int> g_regLogs{0};
std::atomic<int> g_dropLogs{0};

// SEH-guarded read: engine pointer chains can go stale between frames.
// POD-only locals (SEH cannot coexist with C++ unwinding in one function).
bool PeekBytes(uintptr_t src, void* dst, size_t n) {
    __try {
        memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (1) {
        return false;
    }
}

// Cheap plausibility gate for a bone world transform read from a raw
// pointer: freed-then-reused memory usually fails these long before the
// composed matrix reaches the screen as an exploded actor.
bool BoneWorldPlausible(const XfPod& bw) {
    if (!(bw.scale > 1.0e-4f && bw.scale < 1.0e3f)) return false;
    for (int k = 0; k < 3; ++k)
        if (!(bw.pos[k] > -1.0e7f && bw.pos[k] < 1.0e7f)) return false;
    // Rotation rows of a scaled rotation stay bounded.
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            if (!(bw.rot[r][c] > -4.0f && bw.rot[r][c] < 4.0f)) return false;
    return true;
}

// Compose one bone matrix: row-vector composition (v * IB, then * BW),
// scales folded in, output TRANSPOSED into remixapi's column-vector
// row-major 3x4. See the header block comment for the derivation.
void ComposeBoneTransform(const XfPod& ib, const XfPod& bw,
                          remixapi_Transform& out) {
    float A[3][3], B[3][3];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            A[r][c] = ib.rot[r][c] * ib.scale;
            B[r][c] = bw.rot[r][c] * bw.scale;
        }
    }
    const float* ibt = ib.pos;
    const float* bwt = bw.pos;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            // C = A*B (row-vector); out[r][c] = C[c][r] (transpose).
            out.matrix[r][c] = A[c][0] * B[0][r]
                             + A[c][1] * B[1][r]
                             + A[c][2] * B[2][r];
        }
        // t = ib_t*B + bw_t (row-vector); no transpose for translations.
        out.matrix[r][3] = ibt[0] * B[0][r]
                         + ibt[1] * B[1][r]
                         + ibt[2] * B[2][r]
                         + bwt[r];
    }
}

} // namespace

bool SkinnedMeshes::Register(uint64_t drawableHash, BSTriShape* shape,
                             uint32_t& outBoneCount) {
    outBoneCount = 0;
    if (!shape) return false;
    const uintptr_t shapeAddr = reinterpret_cast<uintptr_t>(shape);

    uintptr_t skinInst = 0;
    if (!PeekBytes(shapeAddr + kOffSkinInstance, &skinInst, 8) || !skinInst)
        return false;

    // Bone count: the VERTEX BUFFER's u8 indices were baked against the
    // NIF's skin bone list, which is BoneData::transforms -- so ITS count is
    // authoritative. The NiNode bones tArray can run SHORTER (bones without
    // scene-graph nodes); sizing by it clamped real indices to bone 0 and
    // yanked those vertices toward the skeleton root (the "shredded suit"
    // spikes, 2026-07-08). worldTransforms must cover the same range (its
    // pointers are valid even for node-less bones, F4SE BSSkin.h).
    uintptr_t boneData = 0;
    if (!PeekBytes(skinInst + kOffBoneData, &boneData, 8) || !boneData) return false;
    uintptr_t btArr = 0;
    uint32_t btCount = 0;
    if (!PeekBytes(boneData + kOffBoneDataArr, &btArr, 8) || !btArr) return false;
    if (!PeekBytes(boneData + kOffBoneDataArr + kTArrCountOff, &btCount, 4))
        return false;

    uintptr_t xfArr = 0;
    uint32_t xfCount = 0;
    if (!PeekBytes(skinInst + kOffWorldXfArr, &xfArr, 8) || !xfArr) return false;
    if (!PeekBytes(skinInst + kOffWorldXfArr + kTArrCountOff, &xfCount, 4))
        return false;

    uint32_t nodeCount = 0;
    PeekBytes(skinInst + kOffBonesArr + kTArrCountOff, &nodeCount, 4);

    const uint32_t boneCount = btCount < xfCount ? btCount : xfCount;
    if (boneCount == 0) return false;
    if (boneCount > kMaxBones) {
        if (g_regLogs.fetch_add(1) < kLogCap) {
            _MESSAGE("FO4RemixPlugin: [Skinning] shape \"%s\" boneCount=%u exceeds "
                     "remixapi cap %u -- skipped",
                     shape->m_name.c_str() ? shape->m_name.c_str() : "",
                     boneCount, kMaxBones);
        }
        return false;
    }
    if (nodeCount != boneCount && g_regLogs.fetch_add(1) < kLogCap) {
        _MESSAGE("FO4RemixPlugin: [Skinning] shape \"%s\" count mismatch: "
                 "boneData=%u worldXf=%u nodes=%u -- using %u",
                 shape->m_name.c_str() ? shape->m_name.c_str() : "",
                 btCount, xfCount, nodeCount, boneCount);
    }

    Entry e;
    e.boneXfPtrs.resize(boneCount);
    e.invBinds.resize(boneCount);
    e.nodeNull.assign(boneCount, 1);
    if (!PeekBytes(xfArr, e.boneXfPtrs.data(), (size_t)boneCount * 8))
        return false;
    // Which bones lack scene-graph NiNodes (BSFlattenedBoneTree bones):
    // their worldTransform pointers target flat-tree entries rather than
    // NiAVObject+0x70 -- tracked for the [SkinDiag] per-bone dump.
    {
        uintptr_t nodesArr = 0;
        if (PeekBytes(skinInst + kOffBonesArr, &nodesArr, 8) && nodesArr) {
            const uint32_t nRead = nodeCount < boneCount ? nodeCount : boneCount;
            std::vector<uintptr_t> nodes(nRead, 0);
            if (nRead && PeekBytes(nodesArr, nodes.data(), (size_t)nRead * 8)) {
                for (uint32_t i = 0; i < nRead; ++i)
                    e.nodeNull[i] = nodes[i] ? 0 : 1;
            }
        }
    }
    for (uint32_t i = 0; i < boneCount; ++i) {
        if (!e.boneXfPtrs[i]) return false;
        if (!PeekBytes(btArr + (uintptr_t)i * kBoneEntryStride + kBoneEntryXfOff,
                       &e.invBinds[i], sizeof(XfPod)))
            return false;
    }

    outBoneCount = boneCount;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_entries[drawableHash] = std::move(e);
    }
    if (g_regLogs.fetch_add(1) < kLogCap) {
        _MESSAGE("FO4RemixPlugin: [Skinning] registered hash=%016llX shape=\"%s\" bones=%u",
                 (unsigned long long)drawableHash,
                 shape->m_name.c_str() ? shape->m_name.c_str() : "", boneCount);
    }
    return true;
}

void SkinnedMeshes::OnDrawableReleased(uint64_t drawableHash) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_entries.erase(drawableHash);
}

void SkinnedMeshes::Reset() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_entries.clear();
}

void SkinnedMeshes::UpdateAndQueue() {
    std::unordered_map<uint64_t, std::vector<remixapi_Transform>> queued;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_entries.empty()) return;
        queued.reserve(g_entries.size());
        // One-shot per-bone dump ([SkinDiag]) for the first big humanoid
        // skin instance: separates "subset of bones read garbage" (the
        // flattened-bone-tree layout suspicion behind the spiky humans)
        // from "weights wrong" -- garbage bones show non-orthonormal
        // rotations (rotDet far from 1) or off-actor translations, and the
        // nodeNull flag says whether they correlate with node-less bones.
        static std::atomic<int> s_boneDump{0};

        for (auto it = g_entries.begin(); it != g_entries.end();) {
            Entry& e = it->second;
            const uint32_t n = (uint32_t)e.boneXfPtrs.size();
            const bool dumpThis =
                n > 40 && s_boneDump.load(std::memory_order_relaxed) == 0 &&
                s_boneDump.exchange(1, std::memory_order_relaxed) == 0;
            if (dumpThis) {
                _MESSAGE("FO4RemixPlugin: [SkinDiag] ==== hash=%016llX bones=%u ====",
                         (unsigned long long)it->first, n);
            }
            std::vector<remixapi_Transform> mats(n);
            bool ok = true;
            for (uint32_t i = 0; i < n; ++i) {
                XfPod bw;
                if (!PeekBytes(e.boneXfPtrs[i], &bw, sizeof(bw)) ||
                    !BoneWorldPlausible(bw)) {
                    if (dumpThis) {
                        _MESSAGE("FO4RemixPlugin: [SkinDiag] bone %3u nodeNull=%u "
                                 "ptr=%p READ-FAIL/IMPLAUSIBLE", i,
                                 (unsigned)e.nodeNull[i], (void*)e.boneXfPtrs[i]);
                        continue;  // keep dumping the rest of the skeleton
                    }
                    ok = false;
                    break;
                }
                if (dumpThis) {
                    const float det =
                        bw.rot[0][0] * (bw.rot[1][1] * bw.rot[2][2] - bw.rot[1][2] * bw.rot[2][1])
                      - bw.rot[0][1] * (bw.rot[1][0] * bw.rot[2][2] - bw.rot[1][2] * bw.rot[2][0])
                      + bw.rot[0][2] * (bw.rot[1][0] * bw.rot[2][1] - bw.rot[1][1] * bw.rot[2][0]);
                    _MESSAGE("FO4RemixPlugin: [SkinDiag] bone %3u nodeNull=%u ptr=%p "
                             "pos=(%.1f,%.1f,%.1f) scale=%.3f rotDet=%.4f",
                             i, (unsigned)e.nodeNull[i], (void*)e.boneXfPtrs[i],
                             bw.pos[0], bw.pos[1], bw.pos[2], bw.scale, det);
                }
                ComposeBoneTransform(e.invBinds[i], bw, mats[i]);
            }
            if (dumpThis) {
                // Dump frame: don't let a mid-dump fault skew the fault
                // counter or drop the entry; resume normal operation next
                // Tick.
                ++it;
                continue;
            }
            if (!ok) {
                // Transient mid-update state is normal; a persistently
                // faulting entry means the skeleton is gone -- drop it (the
                // drawable keeps its last queued pose until released).
                if (++e.faults >= kMaxFaultsBeforeDrop) {
                    if (g_dropLogs.fetch_add(1) < kLogCap) {
                        _MESSAGE("FO4RemixPlugin: [Skinning] dropping hash=%016llX "
                                 "after %u consecutive bone-read faults",
                                 (unsigned long long)it->first, e.faults);
                    }
                    it = g_entries.erase(it);
                    continue;
                }
                ++it;
                continue;
            }
            e.faults = 0;
            queued.emplace(it->first, std::move(mats));
            ++it;
        }
    }
    if (!queued.empty()) {
        RemixRenderer::QueueBoneTransforms(std::move(queued));
    }
}
