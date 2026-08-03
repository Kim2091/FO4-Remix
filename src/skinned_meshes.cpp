#include "skinned_meshes.h"
#include "remix_renderer.h"
#include "semantic_capture.h"

#include "f4se/PluginAPI.h"   // _MESSAGE
#include "f4se/NiTypes.h"     // NiTransform
#include "f4se/BSGeometry.h"  // BSTriShape
#include "f4se/NiMaterials.h"

#include <algorithm>
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

struct EyeLiveState {
    XfPod   boneWorld{};
    XfPod   shapeWorld{};
    XfPod   parentWorld{};
    float   materialUv[8]{};
    uint8_t extraData[24]{};
    uint32_t technique = 0;
    bool hasBone = false;
    bool hasShape = false;
    bool hasParent = false;
    bool hasMaterialUv = false;
    bool hasTechnique = false;
    bool hasExtraData = false;
};

struct Entry {
    std::vector<uintptr_t> boneXfPtrs;  // -> live NiTransform (0x40) per bone
    std::vector<uintptr_t> boneNodePtrs;
    std::vector<XfPod>     invBinds;    // copied at registration
    std::vector<uint8_t>   nodeNull;    // bones[i] NiNode* was null (flattened-tree bone)
    uint32_t faults = 0;

    // [EyeAnim] first iris only; diagnostics never mutate these pointers.
    bool eyeProbe = false;
    uintptr_t eyeShape = 0;
    uintptr_t eyeParent = 0;
    uintptr_t eyeProperty = 0;
    uintptr_t eyeMaterial = 0;
    uintptr_t eyeCenterExtra = 0;
    uint32_t eyeMaterialType = 0;
    uint32_t eyeTick = 0;
    char eyeLabel[64]{};
    EyeLiveState eyeLast{};
};

// Registry is game-thread-owned (resolver + Tick), but ReleaseDrawable call
// sites span sweep paths -- keep it mutex-guarded regardless; contention is
// negligible at actor counts.
std::mutex g_mx;
std::unordered_map<uint64_t, Entry> g_entries;

std::atomic<int> g_regLogs{0};
std::atomic<int> g_dropLogs{0};
std::atomic<int> g_eyeLogs{0};

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

bool PeekCString(uintptr_t src, char* dst, size_t cap) {
    if (!dst || cap == 0) return false;
    dst[0] = '\0';
    if (!src) return false;
    __try {
        size_t i = 0;
        for (; i + 1 < cap; ++i) {
            const char c = reinterpret_cast<const char*>(src)[i];
            dst[i] = c;
            if (c == '\0') return true;
        }
        dst[i] = '\0';
        return true;
    } __except (1) {
        dst[0] = '\0';
        return false;
    }
}

bool PeekObjectName(uintptr_t obj, char* dst, size_t cap) {
    uintptr_t str = 0;
    return obj && PeekBytes(obj + 0x10, &str, sizeof(str)) &&
           PeekCString(str, dst, cap);
}

uintptr_t FindEyeCenterExtra(uintptr_t shape) {
    uintptr_t obj = shape;
    for (int depth = 0; depth < 4 && obj; ++depth) {
        uintptr_t extras = 0;
        if (PeekBytes(obj + 0x20, &extras, sizeof(extras)) && extras) {
            uintptr_t entries = 0;
            uint32_t count = 0;
            if (PeekBytes(extras, &entries, sizeof(entries)) && entries &&
                PeekBytes(extras + kTArrCountOff, &count, sizeof(count)) &&
                count <= 64) {
                for (uint32_t i = 0; i < count; ++i) {
                    uintptr_t extra = 0;
                    if (!PeekBytes(entries + (uintptr_t)i * 8, &extra,
                                   sizeof(extra)) || !extra) {
                        continue;
                    }
                    char leaf[64] = "";
                    SemanticCapture::GetLeafClassName(
                        reinterpret_cast<void*>(extra), leaf, sizeof(leaf));
                    if (std::strstr(leaf, "BSEyeCenterExtraData"))
                        return extra;
                }
            }
        }
        uintptr_t parent = 0;
        if (!PeekBytes(obj + 0x28, &parent, sizeof(parent))) break;
        obj = parent;
    }
    return 0;
}

EyeLiveState ReadEyeState(const Entry& e) {
    EyeLiveState s{};
    if (!e.boneXfPtrs.empty() && e.boneXfPtrs[0]) {
        s.hasBone = PeekBytes(e.boneXfPtrs[0], &s.boneWorld,
                              sizeof(s.boneWorld));
    }
    if (e.eyeShape) {
        s.hasShape = PeekBytes(e.eyeShape + 0x70, &s.shapeWorld,
                               sizeof(s.shapeWorld));
    }
    if (e.eyeParent) {
        s.hasParent = PeekBytes(e.eyeParent + 0x70, &s.parentWorld,
                                sizeof(s.parentWorld));
    }
    if (e.eyeMaterial) {
        s.hasMaterialUv = PeekBytes(e.eyeMaterial + 0x0C, s.materialUv,
                                    sizeof(s.materialUv));
    }
    if (e.eyeProperty) {
        s.hasTechnique = PeekBytes(e.eyeProperty + 0xD8, &s.technique,
                                   sizeof(s.technique));
    }
    if (e.eyeCenterExtra) {
        // NiExtraData ends at +0x18; the payload immediately follows.
        s.hasExtraData = PeekBytes(e.eyeCenterExtra + 0x18, s.extraData,
                                   sizeof(s.extraData));
    }
    return s;
}

float XfDelta(const XfPod& a, const XfPod& b) {
    float d = 0.0f;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            d = (std::max)(d, std::fabs(a.rot[r][c] - b.rot[r][c]));
    for (int i = 0; i < 3; ++i)
        d = (std::max)(d, std::fabs(a.pos[i] - b.pos[i]));
    return (std::max)(d, std::fabs(a.scale - b.scale));
}

float UvDelta(const float a[8], const float b[8]) {
    float d = 0.0f;
    for (int i = 0; i < 8; ++i)
        d = (std::max)(d, std::fabs(a[i] - b[i]));
    return d;
}

void LogEyeState(uint64_t hash, const Entry& e, const char* phase,
                 const EyeLiveState& s) {
    if (g_eyeLogs.fetch_add(1, std::memory_order_relaxed) >= 64) return;
    float extra[6]{};
    std::memcpy(extra, s.extraData, sizeof(extra));
    _MESSAGE("FO4RemixPlugin: [EyeAnim] %s hash=%016llX shape='%s' "
             "bonePos=(%.3f,%.3f,%.3f) shapePos=(%.3f,%.3f,%.3f) "
             "uvOff0=(%.6f,%.6f) uvOff1=(%.6f,%.6f) "
             "uvScale0=(%.6f,%.6f) uvScale1=(%.6f,%.6f) tech=%08X "
             "extraF=(%.5f,%.5f,%.5f,%.5f,%.5f,%.5f) "
             "valid=B%d/S%d/P%d/U%d/T%d/E%d",
             phase ? phase : "state", (unsigned long long)hash, e.eyeLabel,
             s.boneWorld.pos[0], s.boneWorld.pos[1], s.boneWorld.pos[2],
             s.shapeWorld.pos[0], s.shapeWorld.pos[1], s.shapeWorld.pos[2],
             s.materialUv[0], s.materialUv[1],
             s.materialUv[2], s.materialUv[3],
             s.materialUv[4], s.materialUv[5],
             s.materialUv[6], s.materialUv[7], s.technique,
             extra[0], extra[1], extra[2], extra[3], extra[4], extra[5],
             s.hasBone ? 1 : 0, s.hasShape ? 1 : 0,
             s.hasParent ? 1 : 0, s.hasMaterialUv ? 1 : 0,
             s.hasTechnique ? 1 : 0, s.hasExtraData ? 1 : 0);
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
                             uint32_t& outBoneCount, const char** outFailReason) {
    outBoneCount = 0;
    auto fail = [&](const char* reason) {
        if (outFailReason) *outFailReason = reason;
        return false;
    };
    if (!shape) return fail("null shape");
    const uintptr_t shapeAddr = reinterpret_cast<uintptr_t>(shape);

    uintptr_t skinInst = 0;
    if (!PeekBytes(shapeAddr + kOffSkinInstance, &skinInst, 8) || !skinInst)
        return fail("skinInstance (+0x140) null/unreadable");

    // Bone count: the VERTEX BUFFER's u8 indices were baked against the
    // NIF's skin bone list, which is BoneData::transforms -- so ITS count is
    // authoritative. The NiNode bones tArray can run SHORTER (bones without
    // scene-graph nodes); sizing by it clamped real indices to bone 0 and
    // yanked those vertices toward the skeleton root (the "shredded suit"
    // spikes, 2026-07-08). worldTransforms must cover the same range (its
    // pointers are valid even for node-less bones, F4SE BSSkin.h).
    uintptr_t boneData = 0;
    if (!PeekBytes(skinInst + kOffBoneData, &boneData, 8) || !boneData)
        return fail("BSSkin::Instance::boneData null/unreadable");
    uintptr_t btArr = 0;
    uint32_t btCount = 0;
    if (!PeekBytes(boneData + kOffBoneDataArr, &btArr, 8) || !btArr)
        return fail("BoneData::transforms array null/unreadable");
    if (!PeekBytes(boneData + kOffBoneDataArr + kTArrCountOff, &btCount, 4))
        return fail("BoneData::transforms count unreadable");

    uintptr_t xfArr = 0;
    uint32_t xfCount = 0;
    if (!PeekBytes(skinInst + kOffWorldXfArr, &xfArr, 8) || !xfArr)
        return fail("worldTransforms array null/unreadable");
    if (!PeekBytes(skinInst + kOffWorldXfArr + kTArrCountOff, &xfCount, 4))
        return fail("worldTransforms count unreadable");

    uint32_t nodeCount = 0;
    PeekBytes(skinInst + kOffBonesArr + kTArrCountOff, &nodeCount, 4);

    const uint32_t boneCount = btCount < xfCount ? btCount : xfCount;
    if (boneCount == 0) return fail("boneCount 0");
    if (boneCount > kMaxBones) {
        if (g_regLogs.fetch_add(1) < kLogCap) {
            _MESSAGE("FO4RemixPlugin: [Skinning] shape \"%s\" boneCount=%u exceeds "
                     "remixapi cap %u -- skipped",
                     shape->m_name.c_str() ? shape->m_name.c_str() : "",
                     boneCount, kMaxBones);
        }
        return fail("boneCount exceeds remixapi cap");
    }
    if (nodeCount != boneCount && g_regLogs.fetch_add(1) < kLogCap) {
        _MESSAGE("FO4RemixPlugin: [Skinning] shape \"%s\" count mismatch: "
                 "boneData=%u worldXf=%u nodes=%u -- using %u",
                 shape->m_name.c_str() ? shape->m_name.c_str() : "",
                 btCount, xfCount, nodeCount, boneCount);
    }

    Entry e;
    e.boneXfPtrs.resize(boneCount);
    e.boneNodePtrs.assign(boneCount, 0);
    e.invBinds.resize(boneCount);
    e.nodeNull.assign(boneCount, 1);
    if (!PeekBytes(xfArr, e.boneXfPtrs.data(), (size_t)boneCount * 8))
        return fail("worldTransforms bulk read failed");
    // Which bones lack scene-graph NiNodes (BSFlattenedBoneTree bones):
    // their worldTransform pointers target flat-tree entries rather than
    // NiAVObject+0x70 -- tracked for the [SkinDiag] per-bone dump.
    {
        uintptr_t nodesArr = 0;
        if (PeekBytes(skinInst + kOffBonesArr, &nodesArr, 8) && nodesArr) {
            const uint32_t nRead = nodeCount < boneCount ? nodeCount : boneCount;
            if (nRead && PeekBytes(nodesArr, e.boneNodePtrs.data(),
                                   (size_t)nRead * 8)) {
                for (uint32_t i = 0; i < nRead; ++i)
                    e.nodeNull[i] = e.boneNodePtrs[i] ? 0 : 1;
            }
        }
    }
    for (uint32_t i = 0; i < boneCount; ++i) {
        if (!e.boneXfPtrs[i]) return fail("null bone world-transform ptr");
        if (!PeekBytes(btArr + (uintptr_t)i * kBoneEntryStride + kBoneEntryXfOff,
                       &e.invBinds[i], sizeof(XfPod)))
            return fail("inverse-bind read failed");
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

void SkinnedMeshes::LogBones(uint64_t drawableHash, const char* label) {
    static std::atomic<int> s_dumps{0};
    if (s_dumps.fetch_add(1, std::memory_order_relaxed) >= 4) return;
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_entries.find(drawableHash);
    if (it == g_entries.end()) {
        _MESSAGE("FO4RemixPlugin: [HeadDiag] bones \"%s\" hash=%016llX: no entry",
                 label ? label : "", (unsigned long long)drawableHash);
        return;
    }
    const Entry& e = it->second;
    const uint32_t n = (uint32_t)e.boneXfPtrs.size();
    _MESSAGE("FO4RemixPlugin: [HeadDiag] bones \"%s\" hash=%016llX n=%u",
             label ? label : "", (unsigned long long)drawableHash, n);
    auto det3 = [](const float r[3][4]) {
        return r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1])
             - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0])
             + r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
    };
    for (uint32_t i = 0; i < n && i < 24; ++i) {
        const XfPod& ib = e.invBinds[i];
        XfPod bw;
        if (!PeekBytes(e.boneXfPtrs[i], &bw, sizeof(bw))) {
            _MESSAGE("FO4RemixPlugin: [HeadDiag]   bone %2u nodeNull=%u ptr=%p "
                     "READ-FAIL | ib pos=(%.1f,%.1f,%.1f) det=%.3f",
                     i, (unsigned)e.nodeNull[i], (void*)e.boneXfPtrs[i],
                     ib.pos[0], ib.pos[1], ib.pos[2], det3(ib.rot));
            continue;
        }
        _MESSAGE("FO4RemixPlugin: [HeadDiag]   bone %2u nodeNull=%u "
                 "bw pos=(%.1f,%.1f,%.1f) scale=%.3f det=%.4f | "
                 "ib pos=(%.1f,%.1f,%.1f) scale=%.3f det=%.4f",
                 i, (unsigned)e.nodeNull[i],
                 bw.pos[0], bw.pos[1], bw.pos[2], bw.scale, det3(bw.rot),
                 ib.pos[0], ib.pos[1], ib.pos[2], ib.scale, det3(ib.rot));
    }
}

// [FaceAnim] probe target (first facegen head registered this session).
static std::atomic<uint64_t> g_faceProbeHash{0};
static std::atomic<uint64_t> g_eyeProbeHash{0};

void SkinnedMeshes::SetFaceProbe(uint64_t drawableHash) {
    uint64_t expected = 0;
    g_faceProbeHash.compare_exchange_strong(expected, drawableHash,
                                            std::memory_order_relaxed);
}

void SkinnedMeshes::SetEyeProbe(uint64_t drawableHash, BSTriShape* shape,
                                void* shaderProperty,
                                BSLightingShaderMaterialBase* material,
                                uint32_t materialType) {
    uint64_t expected = 0;
    if (!g_eyeProbeHash.compare_exchange_strong(
            expected, drawableHash, std::memory_order_relaxed) &&
        expected != drawableHash) {
        return;
    }

    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_entries.find(drawableHash);
    if (it == g_entries.end()) return;

    Entry& e = it->second;
    e.eyeProbe = true;
    e.eyeShape = reinterpret_cast<uintptr_t>(shape);
    e.eyeProperty = reinterpret_cast<uintptr_t>(shaderProperty);
    e.eyeMaterial = reinterpret_cast<uintptr_t>(material);
    e.eyeMaterialType = materialType;
    e.eyeTick = 0;
    e.eyeParent = 0;
    if (e.eyeShape)
        PeekBytes(e.eyeShape + 0x28, &e.eyeParent, sizeof(e.eyeParent));
    e.eyeCenterExtra = FindEyeCenterExtra(e.eyeShape);
    if (!PeekObjectName(e.eyeShape, e.eyeLabel, sizeof(e.eyeLabel)))
        std::strncpy(e.eyeLabel, "<unreadable>", sizeof(e.eyeLabel) - 1);

    if (g_eyeLogs.fetch_add(1, std::memory_order_relaxed) < 64) {
        _MESSAGE("FO4RemixPlugin: [EyeAnim] registered hash=%016llX "
                 "shape='%s' bones=%zu matType=%u shape=%p parent=%p "
                 "property=%p material=%p eyeCenterExtra=%p",
                 (unsigned long long)drawableHash, e.eyeLabel,
                 e.boneXfPtrs.size(), e.eyeMaterialType,
                 (void*)e.eyeShape, (void*)e.eyeParent,
                 (void*)e.eyeProperty, (void*)e.eyeMaterial,
                 (void*)e.eyeCenterExtra);
    }
    for (size_t i = 0; i < e.boneNodePtrs.size() && i < 8; ++i) {
        if (g_eyeLogs.fetch_add(1, std::memory_order_relaxed) >= 64) break;
        char name[64] = "";
        char leaf[64] = "";
        if (e.boneNodePtrs[i]) {
            PeekObjectName(e.boneNodePtrs[i], name, sizeof(name));
            SemanticCapture::GetLeafClassName(
                reinterpret_cast<void*>(e.boneNodePtrs[i]),
                leaf, sizeof(leaf));
        }
        _MESSAGE("FO4RemixPlugin: [EyeAnim] bone[%zu] node=%p null=%u "
                 "name='%s' class=%s worldXf=%p",
                 i, (void*)e.boneNodePtrs[i],
                 i < e.nodeNull.size() ? (unsigned)e.nodeNull[i] : 1u,
                 name, leaf, (void*)e.boneXfPtrs[i]);
    }

    e.eyeLast = ReadEyeState(e);
    LogEyeState(drawableHash, e, "baseline", e.eyeLast);
}

void SkinnedMeshes::OnDrawableReleased(uint64_t drawableHash) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_entries.erase(drawableHash);
    uint64_t expected = drawableHash;
    g_eyeProbeHash.compare_exchange_strong(expected, 0,
                                            std::memory_order_relaxed);
}

bool SkinnedMeshes::HasEntry(uint64_t drawableHash) {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_entries.count(drawableHash) != 0;
}

void SkinnedMeshes::Reset() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_entries.clear();
    g_eyeProbeHash.store(0, std::memory_order_relaxed);
}

void SkinnedMeshes::UpdateAndQueue(const std::unordered_set<uint64_t>* skipHidden) {
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
            // Hidden/stale drawables: the renderer skips their draw, so
            // reading + composing their whole skeleton every Tick is pure
            // waste. Keep the entry (and its fault counter) untouched.
            if (skipHidden && skipHidden->count(it->first)) {
                ++it;
                continue;
            }
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
            // [EyeAnim] Sample the original engine-side drivers at a modest
            // cadence. A changing field identifies where gaze lives; a
            // heartbeat with every field static points us at draw-time
            // shader constants instead of scene/material state.
            if (e.eyeProbe && ++e.eyeTick % 30 == 0) {
                const EyeLiveState now = ReadEyeState(e);
                const EyeLiveState& old = e.eyeLast;
                const float boneDelta =
                    now.hasBone && old.hasBone
                        ? XfDelta(now.boneWorld, old.boneWorld) : 0.0f;
                const float shapeDelta =
                    now.hasShape && old.hasShape
                        ? XfDelta(now.shapeWorld, old.shapeWorld) : 0.0f;
                const float parentDelta =
                    now.hasParent && old.hasParent
                        ? XfDelta(now.parentWorld, old.parentWorld) : 0.0f;
                const float uvDelta =
                    now.hasMaterialUv && old.hasMaterialUv
                        ? UvDelta(now.materialUv, old.materialUv) : 0.0f;
                const bool validityChanged =
                    now.hasBone != old.hasBone ||
                    now.hasShape != old.hasShape ||
                    now.hasParent != old.hasParent ||
                    now.hasMaterialUv != old.hasMaterialUv ||
                    now.hasTechnique != old.hasTechnique ||
                    now.hasExtraData != old.hasExtraData;
                const bool techniqueChanged =
                    now.hasTechnique && old.hasTechnique &&
                    now.technique != old.technique;
                const bool extraChanged =
                    now.hasExtraData && old.hasExtraData &&
                    std::memcmp(now.extraData, old.extraData,
                                sizeof(now.extraData)) != 0;
                const bool changed = validityChanged || techniqueChanged ||
                    extraChanged || boneDelta > 1.0e-5f ||
                    shapeDelta > 1.0e-5f || parentDelta > 1.0e-5f ||
                    uvDelta > 1.0e-6f;
                const bool heartbeat = e.eyeTick % 180 == 0;
                if ((changed || heartbeat) &&
                    g_eyeLogs.fetch_add(1, std::memory_order_relaxed) < 64) {
                    _MESSAGE("FO4RemixPlugin: [EyeAnim] sample hash=%016llX "
                             "tick=%u changed=%d boneD=%.7f shapeD=%.7f "
                             "parentD=%.7f uvD=%.8f techChanged=%d "
                             "extraChanged=%d validityChanged=%d",
                             (unsigned long long)it->first, e.eyeTick,
                             changed ? 1 : 0, boneDelta, shapeDelta,
                             parentDelta, uvDelta,
                             techniqueChanged ? 1 : 0,
                             extraChanged ? 1 : 0,
                             validityChanged ? 1 : 0);
                    LogEyeState(it->first, e,
                                changed ? "changed" : "heartbeat", now);
                }
                e.eyeLast = now;
            }
            // [FaceAnim] expressions probe: periodic composed-translation
            // sample for the marked facegen head. Motion here during
            // dialogue proves the engine drives expressions through these
            // bone worlds (and our pipeline should show them); a static
            // readout means facial animation lives elsewhere.
            if (it->first == g_faceProbeHash.load(std::memory_order_relaxed) &&
                n >= 7) {
                static uint32_t s_probeTick = 0;
                static std::atomic<int> s_probeLogs{0};
                if (++s_probeTick % 90 == 0 &&
                    s_probeLogs.fetch_add(1, std::memory_order_relaxed) < 30) {
                    const auto& m0 = mats[0].matrix;
                    const auto& m4 = mats[4].matrix;
                    const auto& m6 = mats[6].matrix;
                    _MESSAGE("FO4RemixPlugin: [FaceAnim] hash=%016llX "
                             "b0t=(%.2f,%.2f,%.2f) b4t=(%.2f,%.2f,%.2f) "
                             "b6t=(%.2f,%.2f,%.2f)",
                             (unsigned long long)it->first,
                             m0[0][3], m0[1][3], m0[2][3],
                             m4[0][3], m4[1][3], m4[2][3],
                             m6[0][3], m6[1][3], m6[2][3]);
                }
            }
            queued.emplace(it->first, std::move(mats));
            ++it;
        }
    }
    if (!queued.empty()) {
        RemixRenderer::QueueBoneTransforms(std::move(queued));
    }
}
