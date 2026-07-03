#include "lighting_static.h"

#include "../bs_extraction.h"
#include "../semantic_capture.h"
#include "../remix_renderer.h"
#include "../config.h"
#include "../draw_capture.h"
#include "../fo4_diagnostics.h"
#include "f4se/NiObjects.h"
#include "f4se/NiNodes.h"      // NiNode (parent-chain walk in InstDiag)
#include "f4se/NiExtraData.h"  // NiExtraData (InstDiag extra-data peek)
#include "f4se/BSGeometry.h"
#include "f4se/NiMaterials.h"
#include "f4se/PluginAPI.h"  // _MESSAGE

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>
#include <array>
#include <cmath>
#include <cstring>

namespace Resolvers {

// SEH-guarded memory peek for the InstDiag raw-member probe: candidate
// pointers come from raw qword dumps of undeclared engine classes and may
// not be pointers at all. POD-only locals (SEH cannot coexist with C++
// unwinding in one function). __except(1) == EXCEPTION_EXECUTE_HANDLER.
static bool PeekQwordsGuarded(const void* src, uint64_t* dst, int count) {
    __try {
        const uint64_t* s = static_cast<const uint64_t*>(src);
        for (int i = 0; i < count; ++i) {
            dst[i] = s[i];
        }
        return true;
    } __except (1) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Merge-instanced transform stream reader (2026-07-03).
//
// BSMergeInstancedTriShape is a small base mesh hardware-instanced N times;
// rendering it once at the leaf transform (translation-only cluster origin)
// put one unrotated copy at the cluster center -- the "precombined objects
// have wrong transforms" bug (Sanctuary roads / light poles / hedges).
//
// The InstDiag probes (eb9fef8..deecffc, 2026-07-03) pinned the source on
// every sampled shape: shape+0x170 -> buffer wrapper whose first qword is a
// STRUCTURED ID3D11Buffer (bind=SHADER_RESOURCE, misc=BUFFER_STRUCTURED,
// StructureByteStride=80) holding exactly one 80-byte record per hardware
// instance; the wrapper's second member is the paired SRV whose NumElements
// always equals ByteWidth/80 (2..15 on the Sanctuary samples). Record
// layout, content-validated by the v5 run detector (rotation rows with
// exact-0.0f pads, cluster-local translations, phase 0, stride 80):
//   f[0..11]  rotation, three rows of [x y z 0]
//   f[12..14] translation, cluster-local (relative to the leaf transform)
//   f[15]     scale slot (1.0 on every observed record)
//   f[16..19] extra (bounding sphere); unused here
// The vanilla vertex shader fetches records by SV_InstanceID through the
// SRV. The records share the engine's ROW-VECTOR matrix convention
// (world = v * M + t, rows = world images of the local axes -- the same
// convention the camera path in camera.cpp anchors empirically), so the
// rows are used as-is and BuildRemixTransform performs the one and only
// Beth->Remix conversion. [Precombines] MergeInstanceRowVector=0 flips to
// a transposed reading if instanced placements ever come out with negated
// rotations again.
//
// (2026-07-03 note: f[16..19] was hoped to be a bounding sphere usable to
// self-verify the convention; the GPU records show f[16] == 1.0 constant
// and uninitialized junk after -- no ground truth there. The "rotated 90
// degrees" user report was instead traced to BuildRemixTransform applying
// the TRANSPOSED linear map for every rotated drawable; see its comment.)

// Rows at f+a / f+b / f+c: near-equal squared lengths in (1e-4, 1e4)
// (uniform scale folded into the rows is tolerated) and pairwise
// near-orthogonal. NaNs fail the range compares.
static bool RotRowsPlausible(const float* f, int a, int b, int c) {
    const float* r0 = f + a;
    const float* r1 = f + b;
    const float* r2 = f + c;
    const float l0 = r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2];
    const float l1 = r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2];
    const float l2 = r2[0] * r2[0] + r2[1] * r2[1] + r2[2] * r2[2];
    if (!(l0 > 1e-4f && l0 < 1e4f)) return false;
    if (!(l1 > l0 * 0.72f && l1 < l0 * 1.38f)) return false;
    if (!(l2 > l0 * 0.72f && l2 < l0 * 1.38f)) return false;
    const float d01 = r0[0] * r1[0] + r0[1] * r1[1] + r0[2] * r1[2];
    const float d02 = r0[0] * r2[0] + r0[1] * r2[1] + r0[2] * r2[2];
    const float d12 = r1[0] * r2[0] + r1[1] * r2[1] + r1[2] * r2[2];
    const float tol = 0.12f * l0;
    return std::fabs(d01) < tol && std::fabs(d02) < tol && std::fabs(d12) < tol;
}

static bool TransPlausible(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
           std::fabs(x) < 1e6f && std::fabs(y) < 1e6f && std::fabs(z) < 1e6f;
}

// Layout A, 16 floats: rows (0..2)(4..6)(8..10), pads 3/7/11 exactly 0,
// translation 12..14, slot 15 = scale/1.0/radius in [0, 1e5).
static bool ValidRecordA(const float* f) {
    if (f[3] != 0.0f || f[7] != 0.0f || f[11] != 0.0f) return false;
    if (!RotRowsPlausible(f, 0, 4, 8)) return false;
    if (!TransPlausible(f[12], f[13], f[14])) return false;
    return f[15] == 0.0f || (f[15] > 1e-4f && f[15] < 1e5f);
}

// True when p lies inside d3d11.dll's loaded image -- the only vtables we
// dare QueryInterface. Game-exe vtables (0x7FF7A7...) must NOT get a
// virtual slot-0 call (that's usually the scalar dtor).
static bool PtrInD3D11(uint64_t p) {
    static uint64_t base = 0, end = 0;
    if (!base) {
        HMODULE m = GetModuleHandleA("d3d11.dll");
        if (m) {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(m);
            const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
            const IMAGE_NT_HEADERS64* nt =
                reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + dos->e_lfanew);
            base = reinterpret_cast<uint64_t>(b);
            end  = base + nt->OptionalHeader.SizeOfImage;
        } else {
            base = 1;  // sentinel: looked once, d3d11.dll not loaded
        }
    }
    return p >= base && p < end;
}

// POD-only SEH QueryInterface: the object was only vtable-sniffed, so the
// call itself is guarded. Returns 1 and writes *out on S_OK.
static int QiGuarded(void* obj, const GUID* iid, void** out) {
    __try {
        return static_cast<IUnknown*>(obj)->QueryInterface(*iid, out) == S_OK ? 1 : 0;
    } __except (1) {
        return 0;
    }
}

// Copy `bytes` at `srcOff` of a buffer through a staging buffer created on
// the buffer's OWN device (which may differ from the resolver's device).
// Blocking Map -- acceptable because it runs at most once per merge-
// instanced shape at resolve time (the result is baked into the submitted
// drawables); the texture readback in bs_extraction does the same on this
// thread.
static uint32_t ReadbackBufferSlice(ID3D11Buffer* buf, uint32_t srcOff, uint32_t bytes,
                                    std::vector<uint8_t>& out) {
    ID3D11Device* dev = nullptr;
    buf->GetDevice(&dev);
    if (!dev) return 0;
    uint32_t got = 0;
    D3D11_BUFFER_DESC sd = {};
    sd.ByteWidth      = bytes;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* staging = nullptr;
    if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &staging)) && staging) {
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        if (ctx) {
            D3D11_BOX box = { srcOff, 0, 0, srcOff + bytes, 1, 1 };
            ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, buf, 0, &box);
            D3D11_MAPPED_SUBRESOURCE ms = {};
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &ms))) {
                out.resize(bytes);
                std::memcpy(out.data(), ms.pData, bytes);
                ctx->Unmap(staging, 0);
                got = bytes;
            }
            ctx->Release();
        }
        staging->Release();
    }
    dev->Release();
    return got;
}

static bool HeapLikePtr(uint64_t q);

// ---- Merge-segment layout probe (2026-07-03) ----
// A descriptor-offset inference over the +0x1D0 chain used to live here; it
// came back UNRESOLVED on all 20 multi-segment shapes in its one live run
// and this probe's dumps showed why: +0x1D0 is not a per-segment descriptor
// array at all (invalid 0xFF..FF pointer on the road cluster, polymorphic
// vtabled objects and even raw index-buffer bytes at the "element" strides
// elsewhere). The inference was deleted; DrawCapture (draw_capture.h)
// provides the partition instead. The probe stays for layout questions:
//   hdr      shape+0x188..+0x1CF as 18 dwords. +0x190 is the candidate
//            per-slot record-count table (expect 9-and-4 dwords on the
//            13-record road cluster), +0x1A0..0x1AC is the proven segTris
//            table, the rest is unknown.
//   wrap     the +0x1D0 chain pointers plus the wrapper's first 5 qwords.
//   desc     all four 0xB8-byte descriptors, raw dwords.
//   xdata    extra-data walk up the parent chain: the NIF-side ground truth
//            (BSPackedCombinedGeomDataExtra) carries NumCombined per
//            BSPackedGeomData, and its runtime object should hang off the
//            shape or a cluster ancestor. "Packed" entries get raw dumps
//            plus one level of pointer-chasing.
//   resample hdr re-read ~2s later, pumped from TryResolveStatic: a live
//            LOD-selector dword changes with camera distance while static
//            count tables stay put.
namespace MergeProbe {

constexpr int kMaxEntries = 8;
struct Entry {
    void*    tri;
    uint64_t hash;
    uint64_t tick;
    uint32_t hdr[18];
    bool     pending;
};
static Entry            sEntries[kMaxEntries];
static int              sCount = 0;         // guarded by sLock
static std::atomic<int> sPending{0};        // fast-path gate for the pump
static std::mutex       sLock;

static void FormatDwordsHex(const uint32_t* d, int n, char* out, size_t cap) {
    size_t pos = 0;
    out[0] = 0;
    for (int i = 0; i < n; ++i) {
        const int w = snprintf(out + pos, cap - pos, i ? " %08X" : "%08X", d[i]);
        if (w <= 0 || (size_t)w >= cap - pos) break;
        pos += (size_t)w;
    }
}

static void FormatQwordsHex(const uint64_t* q, int n, char* out, size_t cap) {
    size_t pos = 0;
    out[0] = 0;
    for (int i = 0; i < n; ++i) {
        const int w = snprintf(out + pos, cap - pos, i ? " %016llX" : "%016llX",
                               (unsigned long long)q[i]);
        if (w <= 0 || (size_t)w >= cap - pos) break;
        pos += (size_t)w;
    }
}

// BSFixedString data pointer -> printable copy (guarded; empty on any doubt).
static void ReadCStrGuarded(uint64_t strPtr, char* out, size_t cap) {
    out[0] = 0;
    if (!HeapLikePtr(strPtr)) return;
    uint64_t q[6] = {};
    if (!PeekQwordsGuarded(reinterpret_cast<const void*>(strPtr), q, 6)) return;
    char tmp[49];
    std::memcpy(tmp, q, 48);
    tmp[48] = 0;
    size_t i = 0;
    for (; i + 1 < cap && tmp[i]; ++i) {
        const unsigned char c = (unsigned char)tmp[i];
        out[i] = (c >= 0x20 && c < 0x7F) ? tmp[i] : '?';
    }
    out[i] = 0;
}

static void Dump(BSTriShape* tri, uint64_t hash) {
    char buf[560];

    // hdr + resample registration
    uint64_t hq[9];
    if (PeekQwordsGuarded(reinterpret_cast<const void*>(
            reinterpret_cast<uintptr_t>(tri) + 0x188), hq, 9)) {
        uint32_t hd[18];
        std::memcpy(hd, hq, sizeof(hd));
        FormatDwordsHex(hd, 18, buf, sizeof(buf));
        _MESSAGE("FO4RemixPlugin: [MergeProbe] hash=0x%llX hdr(+0x188)=[%s]",
                 (unsigned long long)hash, buf);
        std::lock_guard<std::mutex> g(sLock);
        if (sCount < kMaxEntries) {
            Entry& e = sEntries[sCount++];
            e.tri = tri;
            e.hash = hash;
            e.tick = GetTickCount64();
            std::memcpy(e.hdr, hd, sizeof(hd));
            e.pending = true;
            sPending.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        _MESSAGE("FO4RemixPlugin: [MergeProbe] hash=0x%llX hdr UNREADABLE",
                 (unsigned long long)hash);
    }

    // descriptor chain + raw descriptor dumps
    uint64_t wrapPtr = 0;
    if (PeekQwordsGuarded(reinterpret_cast<const void*>(
            reinterpret_cast<uintptr_t>(tri) + 0x1D0), &wrapPtr, 1) &&
        HeapLikePtr(wrapPtr)) {
        uint64_t wq[5] = {};
        if (PeekQwordsGuarded(reinterpret_cast<const void*>(wrapPtr), wq, 5)) {
            FormatQwordsHex(wq, 5, buf, sizeof(buf));
            _MESSAGE("FO4RemixPlugin: [MergeProbe] hash=0x%llX wrap=%016llX q=[%s]",
                     (unsigned long long)hash, (unsigned long long)wrapPtr, buf);
            uint64_t elemBase = 0;
            if (HeapLikePtr(wq[0]) &&
                PeekQwordsGuarded(reinterpret_cast<const void*>(wq[0]), &elemBase, 1) &&
                HeapLikePtr(elemBase)) {
                for (int s = 0; s < 4; ++s) {
                    uint64_t q[23];
                    if (!PeekQwordsGuarded(reinterpret_cast<const void*>(
                            elemBase + (uint64_t)s * 0xB8), q, 23)) {
                        break;
                    }
                    uint32_t dd[46];
                    std::memcpy(dd, q, sizeof(dd));
                    FormatDwordsHex(dd, 46, buf, sizeof(buf));
                    _MESSAGE("FO4RemixPlugin: [MergeProbe] hash=0x%llX desc[%d]=[%s]",
                             (unsigned long long)hash, s, buf);
                }
            }
        }
    } else {
        _MESSAGE("FO4RemixPlugin: [MergeProbe] hash=0x%llX desc chain unreadable "
                 "(+0x1D0=%016llX)", (unsigned long long)hash,
                 (unsigned long long)wrapPtr);
    }

    // extra-data walk up the parent chain (NiObjectNET::m_extraData at +0x20
    // is tMutexArray<NiExtraData*>*: entries qword at +0, count dword at
    // +0x10; NiAVObject::m_parent at +0x28; NiExtraData::m_name at +0x10).
    uintptr_t obj = reinterpret_cast<uintptr_t>(tri);
    for (int depth = 0; depth < 8 && obj; ++depth) {
        uint64_t xarr = 0;
        if (PeekQwordsGuarded(reinterpret_cast<const void*>(obj + 0x20), &xarr, 1) &&
            HeapLikePtr(xarr)) {
            uint64_t entq[3] = {};
            if (PeekQwordsGuarded(reinterpret_cast<const void*>(xarr), entq, 3)) {
                const uint64_t entries = entq[0];
                const uint32_t count = (uint32_t)entq[2];
                if (HeapLikePtr(entries) && count > 0 && count <= 64) {
                    char cls[64] = "";
                    SemanticCapture::GetLeafClassName(reinterpret_cast<void*>(obj),
                                                      cls, sizeof(cls));
                    for (uint32_t i = 0; i < count && i < 16; ++i) {
                        uint64_t xp = 0;
                        if (!PeekQwordsGuarded(reinterpret_cast<const void*>(
                                entries + 8ull * i), &xp, 1) || !HeapLikePtr(xp)) {
                            continue;
                        }
                        char xcls[64] = "";
                        SemanticCapture::GetLeafClassName(reinterpret_cast<void*>(xp),
                                                          xcls, sizeof(xcls));
                        uint64_t namePtr = 0;
                        char xname[48] = "";
                        if (PeekQwordsGuarded(reinterpret_cast<const void*>(xp + 0x10),
                                              &namePtr, 1)) {
                            ReadCStrGuarded(namePtr, xname, sizeof(xname));
                        }
                        _MESSAGE("FO4RemixPlugin: [MergeProbe] hash=0x%llX xdata d=%d "
                                 "owner=%s i=%u/%u cls=%s name=\"%s\"",
                                 (unsigned long long)hash, depth, cls, i, count,
                                 xcls, xname);
                        if (std::strstr(xcls, "Packed") == nullptr &&
                            std::strstr(xname, "Packed") == nullptr) {
                            continue;
                        }
                        // ground-truth candidate: raw object dump plus one
                        // level of pointer-chasing (skip vtbl/refcount/name)
                        uint64_t oq[16] = {};
                        if (!PeekQwordsGuarded(reinterpret_cast<const void*>(xp),
                                               oq, 16)) {
                            continue;
                        }
                        FormatQwordsHex(oq, 16, buf, sizeof(buf));
                        _MESSAGE("FO4RemixPlugin: [MergeProbe] hash=0x%llX xdump "
                                 "obj=%016llX q=[%s]", (unsigned long long)hash,
                                 (unsigned long long)xp, buf);
                        int followed = 0;
                        for (int k = 3; k < 16 && followed < 3; ++k) {
                            if (!HeapLikePtr(oq[k])) continue;
                            uint64_t tq[16] = {};
                            if (!PeekQwordsGuarded(reinterpret_cast<const void*>(oq[k]),
                                                   tq, 16)) {
                                continue;
                            }
                            FormatQwordsHex(tq, 16, buf, sizeof(buf));
                            _MESSAGE("FO4RemixPlugin: [MergeProbe] hash=0x%llX "
                                     "xfollow q%d=%016llX q=[%s]",
                                     (unsigned long long)hash, k,
                                     (unsigned long long)oq[k], buf);
                            ++followed;
                        }
                    }
                }
            }
        }
        uint64_t parent = 0;
        if (!PeekQwordsGuarded(reinterpret_cast<const void*>(obj + 0x28),
                               &parent, 1) || !HeapLikePtr(parent)) {
            break;
        }
        obj = (uintptr_t)parent;
    }
}

// Called from TryResolveStatic on every drawable: re-read registered hdr
// regions once ~2s old so distance-driven dwords show up as changed bits.
// The shape pointer can go stale (cell unload) -- the guarded peek turns
// that into an UNREADABLE line instead of a crash.
static void Pump() {
    if (sPending.load(std::memory_order_relaxed) == 0) return;
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> g(sLock);
    for (int i = 0; i < sCount; ++i) {
        Entry& e = sEntries[i];
        if (!e.pending || now - e.tick < 2000) continue;
        e.pending = false;
        sPending.fetch_sub(1, std::memory_order_relaxed);
        uint64_t hq[9];
        if (!PeekQwordsGuarded(reinterpret_cast<const void*>(
                reinterpret_cast<uintptr_t>(e.tri) + 0x188), hq, 9)) {
            _MESSAGE("FO4RemixPlugin: [MergeProbe] resample hash=0x%llX UNREADABLE "
                     "(dt=%llums)", (unsigned long long)e.hash,
                     (unsigned long long)(now - e.tick));
            continue;
        }
        uint32_t hd[18];
        std::memcpy(hd, hq, sizeof(hd));
        uint32_t mask = 0;
        for (int k = 0; k < 18; ++k) {
            if (hd[k] != e.hdr[k]) mask |= 1u << k;
        }
        char buf[200];
        FormatDwordsHex(hd, 18, buf, sizeof(buf));
        _MESSAGE("FO4RemixPlugin: [MergeProbe] resample hash=0x%llX dt=%llums "
                 "changed=0x%05X hdr=[%s]", (unsigned long long)e.hash,
                 (unsigned long long)(now - e.tick), mask, buf);
    }
}

}  // namespace MergeProbe

static inline int PermXY(int i) { return i == 0 ? 1 : (i == 1 ? 0 : 2); }

// Decode one 80-byte instance record into a Bethesda ROW-VECTOR 3x4
// container: m[i*4 + j] (j < 3) is the 3x3 in the engine's row-vector
// convention, m[i*4 + 3] is translation component i.
//   rowVector: true = stored rows used directly, false = transposed.
//   conjugate: interpret the record in the X/Y-swapped space first
//              (M' = P*S*P, t' = P*t).
// Net linear map fed to Remix per (rowVector, conjugate), identity leaf:
//   (1,0) P*S^T   (0,0) P*S   (1,1) S^T*P   (0,1) S*P
// -- all four record conventions reachable from the ini while the true
// one is pinned down empirically.
static void DecodeInstanceRecord(const float* f, bool rowVector, bool conjugate,
                                 float m[12]) {
    // f[15] is 1.0 on every observed record; treat as scale but range-guard
    // so an unexpected semantic (a radius, say) can't explode geometry.
    const float s = (f[15] > 0.01f && f[15] < 20.0f) ? f[15] : 1.0f;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const int ii = conjugate ? PermXY(i) : i;
            const int jj = conjugate ? PermXY(j) : j;
            const float rij = rowVector ? f[ii * 4 + jj] : f[jj * 4 + ii];
            m[i * 4 + j] = rij * s;
        }
        m[i * 4 + 3] = f[12 + (conjugate ? PermXY(i) : i)];
    }
}

// Row-vector composition, instance applied first, leaf world W second:
//   world = ((v * M_inst + t_inst) * M_W) * s_W + t_W
// out gets the combined 3x3 rows (instance scale carried in M_inst, leaf
// scale folded here) and combined translation in the *4+3 slots.
static void ComposeLeafInstance(const NiTransform& W, const float m[12], float out[12]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out[i * 4 + j] = W.scale * (m[i * 4 + 0] * W.rot.data[0][j] +
                                        m[i * 4 + 1] * W.rot.data[1][j] +
                                        m[i * 4 + 2] * W.rot.data[2][j]);
        }
    }
    for (int j = 0; j < 3; ++j) {
        out[j * 4 + 3] = W.scale * (m[0 * 4 + 3] * W.rot.data[0][j] +
                                    m[1 * 4 + 3] * W.rot.data[1][j] +
                                    m[2 * 4 + 3] * W.rot.data[2][j]) + (&W.pos.x)[j];
    }
}

static bool HeapLikePtr(uint64_t q) {
    return q >= 0x100000000ULL && q < 0x00007F0000000000ULL && (q & 7ULL) == 0ULL;
}

// Read the per-instance record stream of a BSMergeInstancedTriShape. See
// the block comment at the top of this section for the reverse-engineered
// source. Output: the raw validated 20-float records (rotation reading is
// decided later against the records' own bounding spheres), plus the raw
// pointer IDENTITIES of the instance buffer and its paired SRV (wrapper
// qwords 0 and 1) for DrawCapture matching -- no references are held.
// Returns false (out untouched) on ANY validation miss, and the caller
// falls back to the pre-existing single-draw path.
static bool ReadMergeInstanceRecords(BSTriShape* tri,
                                     std::vector<std::array<float, 20>>& out,
                                     void** outBufPtr, void** outSrvPtr) {
    const auto heapLike = HeapLikePtr;
    // shape+0x170 -> wrapper; wrapper qword 0 -> structured instance buffer.
    uint64_t wrapPtr = 0;
    if (!PeekQwordsGuarded(reinterpret_cast<const void*>(
            reinterpret_cast<uintptr_t>(tri) + 0x170), &wrapPtr, 1)) return false;
    if (!heapLike(wrapPtr)) return false;
    uint64_t bufObj = 0;
    if (!PeekQwordsGuarded(reinterpret_cast<const void*>(wrapPtr), &bufObj, 1)) return false;
    if (!heapLike(bufObj)) return false;
    uint64_t vtbl = 0;
    if (!PeekQwordsGuarded(reinterpret_cast<const void*>(bufObj), &vtbl, 1)) return false;
    if (!PtrInD3D11(vtbl)) return false;
    *outBufPtr = reinterpret_cast<void*>(bufObj);
    // Wrapper qword 1 = the paired SRV on every v5 probe sample; validate
    // the same way and pass null if it doesn't look like a D3D11 object.
    *outSrvPtr = nullptr;
    uint64_t srvObj = 0, srvVtbl = 0;
    if (PeekQwordsGuarded(reinterpret_cast<const void*>(wrapPtr + 8), &srvObj, 1) &&
        heapLike(srvObj) &&
        PeekQwordsGuarded(reinterpret_cast<const void*>(srvObj), &srvVtbl, 1) &&
        PtrInD3D11(srvVtbl)) {
        *outSrvPtr = reinterpret_cast<void*>(srvObj);
    }

    void* raw = nullptr;
    if (!QiGuarded(reinterpret_cast<void*>(bufObj), &__uuidof(ID3D11Buffer), &raw)) {
        return false;
    }
    ID3D11Buffer* buf = static_cast<ID3D11Buffer*>(raw);
    bool ok = false;
    do {
        constexpr uint32_t kStride = 80;  // observed on every sampled shape
        D3D11_BUFFER_DESC bd = {};
        buf->GetDesc(&bd);
        if (!(bd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)) break;
        if (bd.StructureByteStride != kStride) break;
        if (bd.ByteWidth == 0 || bd.ByteWidth % kStride != 0) break;
        const uint32_t count = bd.ByteWidth / kStride;
        if (count > 4096) break;  // sanity bound; samples were 2..15

        std::vector<uint8_t> bytes;
        if (ReadbackBufferSlice(buf, 0, bd.ByteWidth, bytes) != bd.ByteWidth) break;

        std::vector<std::array<float, 20>> recs(count);
        bool valid = true;
        for (uint32_t k = 0; k < count; ++k) {
            const float* f = reinterpret_cast<const float*>(bytes.data() + k * kStride);
            if (!ValidRecordA(f)) { valid = false; break; }
            std::memcpy(recs[k].data(), f, 20 * sizeof(float));
        }
        if (!valid) break;
        out = std::move(recs);
        ok = true;
    } while (false);
    buf->Release();
    return ok;
}

// In-flight resolver state. Updated at each gate inside TryResolveStatic
// AND inside RemixRenderer::SubmitDrawable (via Trace::SetStep). Read by
// the SEH handler in semantic_capture.cpp's Tick when an access violation
// is caught -- tells us exactly which drawable + step crashed.
//
// The Step enum lives in lighting_static.h so other TUs (remix_renderer,
// semantic_capture) can name the inside-SubmitDrawable / gate constants
// when calling Trace::SetStep.
namespace ResolverTrace {
    std::atomic<int>      g_lastStep{Trace::kIdle};
    std::atomic<uint64_t> g_lastHash{0};

    const char* StepName(int s) {
        switch (s) {
            case Trace::kIdle:                       return "idle";
            case Trace::kEntered:                    return "entered";
            case Trace::kCastOK:                     return "cast_ok";
            case Trace::kSkinSkipped:                return "skin_skipped";
            case Trace::kParseStart:                 return "parse_start";
            case Trace::kParseOK:                    return "parse_ok";
            case Trace::kExtentRejected:             return "extent_rejected";
            case Trace::kBuildMeshOK:                return "build_mesh_ok";
            case Trace::kMaterialFetched:            return "material_fetched";
            case Trace::kLandscapeSkipped:           return "landscape_skipped";
            case Trace::kTexturesExtracted:          return "textures_extracted";
            case Trace::kSubmitStart:                return "submit_start";
            case Trace::kSubmitOK:                   return "submit_ok";
            case Trace::kSubmitFailed:               return "submit_failed";
            case Trace::kSubmit_BeforeTextureCreate:  return "submit_before_texture_create";
            case Trace::kSubmit_AfterTextureCreate:   return "submit_after_texture_create";
            case Trace::kSubmit_BeforeMaterialCreate: return "submit_before_material_create";
            case Trace::kSubmit_AfterMaterialCreate:  return "submit_after_material_create";
            case Trace::kSubmit_BeforeMeshCreate:     return "submit_before_mesh_create";
            case Trace::kSubmit_AfterMeshCreate:      return "submit_after_mesh_create";
            case Trace::kSubmit_GateInputEmpty:       return "submit_gate_input_empty";
            case Trace::kSubmit_GateVram:             return "submit_gate_vram";
            case Trace::kSubmit_GateBudget:           return "submit_gate_budget";
            case Trace::kLODSkipped:                  return "lod_skipped";
            case Trace::kTopFadeNodeSkipped:          return "topfadenode_skipped";
            case Trace::kWorldLODChunkSkipped:        return "world_lod_chunk_skipped";
            default: return "unknown";
        }
    }
}

namespace Lighting {

bool TryResolveStatic(SemanticCapture::DrawableState& state,
                      uint64_t hash,
                      ID3D11Device* device) {
    Resolvers::MergeProbe::Pump();  // delayed hdr re-reads; no-op when none pending

    if (state.submittedToRemix) return true;

    // Mark in-flight immediately so an SEH catch on an early-step crash
    // still reports the right hash.
    ResolverTrace::g_lastHash.store(hash, std::memory_order_relaxed);
    ResolverTrace::g_lastStep.store(Trace::kEntered, std::memory_order_relaxed);

    // state.geometry is void* (set by the hot-path detour). Cast through
    // NiAVObject* so we can use Bethesda's RTTI helper to filter non-BSTriShape
    // geometry (particle systems, etc.) that might also hit BSLightingShaderProperty.
    NiAVObject* obj = static_cast<NiAVObject*>(state.geometry);
    if (!obj) return false;

    BSTriShape* tri = obj->GetAsBSTriShape();
    if (!tri) return false;  // not a BSTriShape (particle system, segmented shape, ...)

    // 1B scope: skip skinned. Skinning regression accepted; later phase revives.
    if (tri->vertexDesc & BSGeometry::kFlag_Skinned) {
        ResolverTrace::g_lastStep.store(Trace::kSkinSkipped, std::memory_order_relaxed);
        return false;
    }

    // Drop engine LOD chunks. NiAVObject::kFlagIsMeshLOD (1<<12) is set at NIF
    // load time on level-of-detail meshes; the engine fires GetRenderPasses
    // for them concurrently with full-detail counterparts during streaming and
    // at distance, producing visible LOD-over-full-detail overlap. Diagnostic
    // run on 2026-04-28 measured 25-45% of all active drawables flagged as
    // LOD across multiple cells. Filtering at submit means these never become
    // Remix mesh handles. Visible cost: cells outside uGridsToLoad render no
    // distant geometry (path tracer handles atmospheric distance).
    //
    // 2026-04-29: DISABLED. The pendingByGate breakdown showed lod=2016 of
    // pending drawables near the player at Concord -- ~40% of all rejections.
    // Visual symptom: HQ structural meshes (walls/floors/roofs) absent while
    // clutter/debris/doors are submitted normally. Hypothesis under test:
    // kFlagIsMeshLOD on FO4 means "geometry participates in the LOD system,"
    // not "geometry IS a low-detail variant" -- so the engine sets it on HQ
    // statics that have LOD counterparts, and our blanket filter dropped them.
    // The chunk-spatial filter (in OnFrame) handles the worldspace-LOD-overlap
    // case this filter was originally added for. If overlap returns visibly,
    // we'll add a smarter discriminator (e.g., require chunk parent metadata).
    // if (state.initialFlags & (1ULL << 12)) {
    //     ResolverTrace::g_lastStep.store(Trace::kLODSkipped, std::memory_order_relaxed);
    //     return false;
    // }

    // NOTE (2026-04-28): two additional filters were tried and reverted.
    //   1. Filter on parent1 kFlagTopFadeNode (bit 14) -- killed buildings,
    //      because TopFadeNode is the engine's general fade-management for
    //      most static refs, not specifically LOD-swap groups.
    //   2. Filter on parent name "chunk" / "obj" -- killed worldspace LOD
    //      chunks (BTR/BTO) which are the WANTED distant rendering, not
    //      the unwanted close-up overlap.
    // The up-close low-quality+low-quality-texture overlap user reports
    // remains unidentified. Hypothesis space narrowed but not resolved.

    ResolverTrace::g_lastStep.store(Trace::kCastOK, std::memory_order_relaxed);

    // Shader-property flag word, read BEFORE the parse because the vertex-
    // color gate below needs it. BSLightingShaderProperty::flags (UInt64 at
    // +0x30) packs shader-flags-1 in the low 32 bits, shader-flags-2 high.
    uint64_t propFlagsEarly = 0;
    if (state.property) {
        propFlagsEarly = *reinterpret_cast<uint64_t*>(
            reinterpret_cast<uintptr_t>(state.property) + 0x30);
    }

    // Vertex-color gate. FO4 meshes routinely CARRY a vertex-color stream
    // that the vanilla shader only multiplies in when SLSF2_Vertex_Colors
    // (flags2 bit 5 -> merged bit 37; layout anchored by kTwoSided = bit 36
    // and kDecal = bit 26, both independently confirmed) is set -- the data
    // is otherwise a shader-specific mask (AO paint, blend weights), often
    // near-black. Baking it unconditionally rendered whole objects black
    // (power-armor stands, chain-link fences, workstations) while their
    // textures loaded fine: the log's WorkstationChemistry propFlags
    // 0x8180400281 has bit 37 CLEAR, yet its painted colors were applied.
    constexpr uint64_t kFlag_VertexColors = 1ULL << 37;
    const bool applyVertexColors = (propFlagsEarly & kFlag_VertexColors) != 0;

    // ---- Parse vertex / index data ----
    ResolverTrace::g_lastStep.store(Trace::kParseStart, std::memory_order_relaxed);
    ParsedGeometry parsed;
    if (!BsExtraction::ParseShapeGeometry(tri, parsed, /*logRejections=*/g_config.logRejections,
                                          applyVertexColors)) {
        return false;
    }

    // Reject shapes with garbage extents (defensive guard against malformed input).
    constexpr float kMaxExtent = 1.0e6f;
    for (const auto& v : parsed.vertices) {
        if (std::abs(v.position[0]) > kMaxExtent ||
            std::abs(v.position[1]) > kMaxExtent ||
            std::abs(v.position[2]) > kMaxExtent) {
            ResolverTrace::g_lastStep.store(Trace::kExtentRejected, std::memory_order_relaxed);
            return false;
        }
    }

    ResolverTrace::g_lastStep.store(Trace::kParseOK, std::memory_order_relaxed);

    // ---- Build mesh ----
    ExtractedMesh mesh{};
    mesh.hash = hash;
    mesh.vertices = std::move(parsed.vertices);
    mesh.indices  = std::move(parsed.indices);
    SemanticCapture::BuildRemixTransform(tri->m_worldTransform, mesh.worldTransform);
    BsExtraction::ExtractAlphaState(tri, mesh);

    // (Merge-instanced transform read happens just before submit, after the
    // material/texture gates -- a retrying shape shouldn't pay the staging-
    // buffer readback stall on every backoff attempt.)

    // Decal tag. The decal bit is in flags1: bit 26, mask 0x04000000.
    // Confirmed for FO4 1.10.980 by static analysis of the BGSM flag-applier
    // at VA 0x142163480 (called from BSLightingShaderProperty::SetMaterial at
    // 0x142162D7C): it passes bit index 0x1A to SetFlag (RVA 0x02161950) for
    // the decal path, gated on the same source byte that drives bit 27
    // (Dynamic_Decal, 0x08000000). Matches Skyrim's SLSF1_Decal layout.
    // F4SE's published kShaderFlags_* enum at NiProperties.h:125-129 omits
    // this flag. (propFlagsEarly is read above the parse, before the
    // vertex-color gate.)
    {
        constexpr uint64_t kSLSF1_Decal = 0x0000000004000000ULL;
        // Two-sided: bit 36 of the merged 64-bit flag word (flags2 bit 4).
        // CommonLibF4 BSShaderProperty::EShaderPropertyFlag kTwoSided = 1<<36;
        // layout cross-checked against kOwnEmit (22) and kDecal (26), both of
        // which match our independently confirmed bits.
        constexpr uint64_t kFlag_TwoSided = 0x0000001000000000ULL;
        if (propFlagsEarly & kSLSF1_Decal) {
            mesh.isDecal = true;
        }
        if (propFlagsEarly & kFlag_TwoSided) {
            mesh.isTwoSided = true;
        }
    }

    // Detect FO4 worldspace LOD chunk and tag the mesh so OnFrame can apply
    // a spatial filter (skip-when-player-is-inside-coverage). Two patterns
    // identified by the 2026-04-28 parent-chain diagnostic:
    //   parent1.name == "chunk", parent2.name in {"4","8","16","32"} -> terrain LOD
    //     chunkExtent = level * 4096 (one cell == 4096 Beth units)
    //   parent2.name == "obj"                                        -> object LOD
    //     chunkExtent = 16384 (level-4 equivalent; refine if logs show otherwise)
    {
        const char* p1Name = nullptr;
        const char* p2Name = nullptr;
        if (state.parent1) {
            p1Name = static_cast<NiAVObject*>(state.parent1)->m_name.c_str();
        }
        if (state.parent2) {
            p2Name = static_cast<NiAVObject*>(state.parent2)->m_name.c_str();
        }
        float lodLevel = 0.0f;
        if (p1Name && p2Name && std::strcmp(p1Name, "chunk") == 0) {
            // parent2.name is a digit string identifying LOD level.
            if      (std::strcmp(p2Name, "4")  == 0) lodLevel = 4.0f;
            else if (std::strcmp(p2Name, "8")  == 0) lodLevel = 8.0f;
            else if (std::strcmp(p2Name, "16") == 0) lodLevel = 16.0f;
            else if (std::strcmp(p2Name, "32") == 0) lodLevel = 32.0f;
        } else if (p2Name && std::strcmp(p2Name, "obj") == 0) {
            lodLevel = 4.0f;  // assumed; refine with diagnostic if wrong
        }
        if (lodLevel > 0.0f) {
            mesh.isLODChunk   = true;
            mesh.chunkOriginX = tri->m_worldTransform.pos.x;
            mesh.chunkOriginY = tri->m_worldTransform.pos.y;
            mesh.chunkExtent  = lodLevel * 4096.0f;
            // Mirror onto the DrawableState so Tick can maintain the
            // chunk-key index that feeds OnFrame's stale-chunk filter.
            state.isLODChunk  = true;
        }
    }

    ResolverTrace::g_lastStep.store(Trace::kBuildMeshOK, std::memory_order_relaxed);

    // ---- Material + textures ----
    auto* mat = BsExtraction::GetLightingMaterial(tri);
    if (!mat) return false;

    // 1B scope: skip landscape (terrain regression accepted; Phase 5 revives).
    if (mat->GetType() == BSLightingShaderMaterialBase::kType_Landscape) {
        ResolverTrace::g_lastStep.store(Trace::kLandscapeSkipped, std::memory_order_relaxed);
        return false;
    }

    ResolverTrace::g_lastStep.store(Trace::kMaterialFetched, std::memory_order_relaxed);

    // ---- Metal conversion, take 2 (spec-gloss -> metal-rough, 2026-07-02) ----
    // FO4's "shiny metal" pathway is BSLightingShaderMaterialEnvmap: near-
    // black diffuse + kSpecularColor*fSpecularColorScale + cubemap*
    // fEnvmapScale. The path tracer replicates none of that, so untreated
    // envmap materials render as black dielectric voids. Session 2026-07-02
    // log: every user-reported black class (PArig02 power-armor stands,
    // PicketFenceB LODs, street lamps, workstations) is matType==kType_Envmap
    // with the SLSF1_Environment_Mapping propFlags bit set, while fine-
    // rendering controls (picture frames, TVs, vans) are kType_Default.
    //
    // Take 1 (506e5e7, reverted in bb2a02f) targeted this class but failed
    // three ways, each fixed here:
    //   - Gated on fEnvmapScale > 0.01, which skipped the very objects it
    //     was built for (the PA stand reads envScale ~0). Classify on
    //     GetType() alone; log envScale for diagnostics only.
    //   - metallic = 0.9 * envScale: authored-low scales (ShotgunReceiver
    //     0.20) came out nearly dielectric. envScale is a reflection-
    //     intensity knob, not a metalness signal. Modulate by fSmoothness
    //     instead -- wetness-style envmaps (tree bark is kType_Envmap!)
    //     author low smoothness, real metals author high -- with a 0.2
    //     participation floor so nothing classified is fully dielectric.
    //   - Albedo lift BLENDED toward the spec tint, washing white-spec
    //     weapons toward white/sepia. Replaced by a hue-preserving
    //     luminance floor on the diffuse (dark pixels scaled up keeping
    //     their hue, bright pixels untouched) -- see AlbedoLumFloor_Apply.
    // In-game verification (2026-07-02): the luminance floor is the piece
    // that recovers the black objects; the metallic/roughness constants did
    // NOT help -- Remix's metal BRDF takes F0 from albedo, so floor-lifted
    // albedo x high metallic still reads near-black (the metallic constant
    // fights the floor). Both derivations are therefore opt-in via
    // [Materials] MetalMetallicEnabled / MetalRoughnessEnabled, default OFF;
    // when off, materials keep the legacy constants (metallic 0, rough 0.8).
    uint8_t albedoLumFloor = 0;
    if (g_config.metalConversionEnabled &&
        mat->GetType() == BSLightingShaderMaterialBase::kType_Envmap) {
        float smooth = mat->fSmoothness;
        if (smooth < 0.0f) smooth = 0.0f;
        if (smooth > 1.0f) smooth = 1.0f;
        if (g_config.metalMetallicEnabled) {
            mesh.metallicConstant = g_config.metalMetallic * (0.2f + 0.8f * smooth);
        }

        if (g_config.metalRoughnessEnabled) {
            float rough = 1.0f - smooth;
            if (rough < g_config.metalMinRoughness) rough = g_config.metalMinRoughness;
            if (rough > 0.95f) rough = 0.95f;
            mesh.roughnessConstantOverride = rough;
        }

        float floorF = g_config.metalAlbedoLumFloor;
        if (floorF < 0.0f) floorF = 0.0f;
        if (floorF > 1.0f) floorF = 1.0f;
        albedoLumFloor = (uint8_t)(floorF * 255.0f + 0.5f);

        static std::atomic<int> sMetalLogs{0};
        const int mn = sMetalLogs.fetch_add(1, std::memory_order_relaxed);
        if (mn < 40) {
            const float envScale =
                static_cast<BSLightingShaderMaterialEnvmap*>(mat)->fEnvmapScale;
            // metallic/rough report the APPLIED values (0 / -1 = toggle off,
            // legacy constants in effect).
            _MESSAGE("FO4RemixPlugin: [Metal] #%d shape=\"%s\" envScale=%.2f smooth=%.2f "
                     "spec=(%.2f,%.2f,%.2f)x%.2f -> metallic=%.2f rough=%.2f lumFloor=%u",
                     mn, tri->m_name.c_str() ? tri->m_name.c_str() : "",
                     envScale, mat->fSmoothness,
                     mat->kSpecularColor.r, mat->kSpecularColor.g, mat->kSpecularColor.b,
                     mat->fSpecularColorScale, mesh.metallicConstant,
                     mesh.roughnessConstantOverride, (unsigned)albedoLumFloor);
        }
    }

    std::vector<ExtractedTexture> newTextures;
    // For alpha-tested or alpha-blended geometry: synthesize alpha from RGB
    // luminance if the diffuse is BC1 (no alpha channel). BGS LOD foliage
    // atlases are stored as BC1; vanilla DX11's rasterizer hides the lack of
    // alpha (cutout regions render as dark blobs), but Remix's path tracer
    // applies our smoothness-derived roughness map at those pixels and
    // produces mirror-reflective rectangles. The synthesized alpha gives the
    // path tracer a real channel to test against.
    //
    // Decals additionally force synthesis on BC3 diffuses: BGS sometimes
    // packs non-cutout data in BC3.a so the authored alpha doesn't behave as
    // a clean mask. Overriding it with luminance gives a usable cutout for
    // the path tracer.
    // Reverted (2026-05-02): forcing luminance synthesis on BC3 decals turned
    // out to destroy perfectly-good authored alpha. Verified by extracting
    // and decoding DecalDebrise05_d.DDS: BC3 alpha is a proper cutout mask
    // (10% near-zero, 23% near-255, full spread across buckets, 17% pass the
    // alphaTest=241 threshold). Bethesda's authored alpha is clean for these
    // BC3 decals; synthesis would replace it with worse luminance-derived
    // values. Use the BC1-only synthesis: BC1 inputs (no alpha at all) get
    // the synthesized cutout, BC3/BC7 inputs preserve their authored alpha.
    const TexturePostProcess diffusePostProcess =
        (mesh.alphaTestEnabled || mesh.alphaBlendEnabled)
            ? TexturePostProcess::DiffuseAlphaFromLuminance
            : TexturePostProcess::None;
    mesh.diffuseTextureHash = BsExtraction::ExtractMaterialTexture(
        mat->spDiffuseTexture, "diffuse", device, newTextures, diffusePostProcess,
        /*minRoughness=*/0, albedoLumFloor);
    mesh.normalTextureHash = BsExtraction::ExtractMaterialTexture(
        mat->spNormalTexture, "normal", device, newTextures, TexturePostProcess::Octahedral);
    // Smoothness/spec-mask (_s.dds) extraction REMOVED (2026-07-02). FO4's
    // packed spec maps translate too inconsistently to naive roughness:
    // "very smooth" authoring became roughness~0 mirrors (decals needed a
    // clamp band-aid; metal fences/racks read as black voids reflecting a
    // dark environment), and per-asset channel packing varies. Dropping the
    // slot leaves mesh.roughnessTextureHash == 0, so SubmitDrawable builds
    // materials with roughnessConstant=0.8 -- and saves a GPU readback +
    // BC decompress + invert per material. Revisit only as part of a real
    // spec-gloss -> metal-rough conversion (spec color/envmap -> metallic).
    BsExtraction::ExtractEmissiveData(tri, mat, device, newTextures,
                                      mesh.emissiveTextureHash,
                                      mesh.emissiveColorR, mesh.emissiveColorG, mesh.emissiveColorB,
                                      mesh.emissiveIntensity);

    // No diffuse -> can't render lit; retry next frame in case texture resolves later.
    if (mesh.diffuseTextureHash == 0) return false;

    ResolverTrace::g_lastStep.store(Trace::kTexturesExtracted, std::memory_order_relaxed);

    // ---- Merge-instanced transform read (2026-07-03) ----
    // BSMergeInstancedTriShape carries per-instance placements in a
    // structured D3D buffer the parser never sees (see the reader's block
    // comment near the top of this file). Read the raw records here; the
    // submit section below expands the shape into one Remix drawable per
    // instance. Empty instRecords = plain shape OR read fell through
    // validation -> single-draw path, pre-fix behavior. Scoped to
    // MergeInstanced only: BSMultiStreamInstanceTriShape (engine grass) has
    // a different stream layout (v2 probe: 8000.0f fade distance at +0x190)
    // and stays on the single-draw path until sampled properly.
    std::vector<std::array<float, 20>> instRecords;
    bool instRowVector = g_config.mergeInstanceRowVector;
    // Per-segment triangle counts from shape+0x1A0 (4 dwords; zeros = empty
    // slots; the nonzero ones sum to numTriangles on every probe sample).
    // A merged shape is a concatenation of sub-models ("segments" -- tree
    // variants, trunk/canopy parts) sharing one material, and the instance
    // records are partitioned per segment: the [MergeInstRec] dumps show
    // exact duplicate transform runs (count == placements x segments, e.g.
    // 39 == 13 x 3 with record 13 duplicating record 0). Drawing the WHOLE
    // mesh with EVERY record put segment geometry on other segments'
    // placements: canopy-style geometry (verts authored high) landed on
    // ground-piece records ("floating"), wrong variants on other variants'
    // spots ("upside down"), while single-segment shapes (the fence lines
    // that proved the decode) were perfect.
    uint32_t instSegTris[4] = { 0, 0, 0, 0 };
    void* instBufPtr = nullptr;   // identity only, for DrawCapture matching
    void* instSrvPtr = nullptr;
    if (g_config.mergeInstanceExpansion && g_config.gpuInstancingEnabled) {
        char instLeaf[64] = "";
        SemanticCapture::GetLeafClassName(reinterpret_cast<void*>(tri),
                                          instLeaf, sizeof(instLeaf));
        if (std::strstr(instLeaf, "MergeInstanced") != nullptr) {
            const bool got = ReadMergeInstanceRecords(tri, instRecords,
                                                      &instBufPtr, &instSrvPtr);
            if (got) {
                uint64_t segQ[2] = {};
                if (PeekQwordsGuarded(reinterpret_cast<const void*>(
                        reinterpret_cast<uintptr_t>(tri) + 0x1A0), segQ, 2)) {
                    std::memcpy(instSegTris, segQ, sizeof(instSegTris));
                }
                // Multi-segment shapes: ask DrawCapture for the engine's own
                // draw parameters (per-sub-model index ranges + record
                // partition). The engine draws the shape within a frame or
                // two of the drawable appearing, so while the capture is
                // pending, defer the whole resolve exactly like a
                // not-yet-ready texture -- the retry path re-enters here.
                // Deliberately placed BEFORE the [MergeInst] logging so
                // deferrals don't burn the capped log counters.
                int nzEarly = 0;
                for (int s = 0; s < 4; ++s) {
                    if (instSegTris[s]) ++nzEarly;
                }
                if (nzEarly >= 2 && g_config.mergeInstanceDrawCapture && instBufPtr) {
                    std::vector<DrawCapture::SegDraw> capPeek;
                    if (DrawCapture::Query(instBufPtr, instSrvPtr, hash,
                                           (uint32_t)instRecords.size(),
                                           instSegTris, capPeek) ==
                        DrawCapture::kCapturing) {
                        return false;
                    }
                }
            }
            const NiTransform& W = tri->m_worldTransform;
            static std::atomic<int> sInstLogs{0};
            const int n = sInstLogs.fetch_add(1, std::memory_order_relaxed);
            if (n < 40) {
                if (got && !instRecords.empty()) {
                    // Determinant of the stored 3x3 (reading-independent up
                    // to sign conventions): det < 0 = the record MIRRORS the
                    // piece. Mirrored instances would render inside-out with
                    // the shared winding-flipped mesh -- if detNeg shows up
                    // nonzero, that's a real follow-up (split mirrored
                    // instances onto a reversed-winding mesh clone).
                    int detNeg = 0;
                    int tilted = 0;  // records with a significant non-yaw part
                    for (const auto& rec : instRecords) {
                        const float* f = rec.data();
                        const float det =
                            f[0] * (f[5] * f[10] - f[6] * f[9]) -
                            f[1] * (f[4] * f[10] - f[6] * f[8]) +
                            f[2] * (f[4] * f[9]  - f[5] * f[8]);
                        if (det < 0.0f) ++detNeg;
                        if (std::fabs(f[2]) > 0.05f || std::fabs(f[6]) > 0.05f ||
                            std::fabs(f[8]) > 0.05f || std::fabs(f[9]) > 0.05f) {
                            ++tilted;
                        }
                    }
                    const float* f0 = instRecords[0].data();
                    _MESSAGE("FO4RemixPlugin: [MergeInst] #%d hash=0x%llX count=%zu "
                             "leafPos=(%.1f,%.1f,%.1f) t0=(%.1f,%.1f,%.1f) "
                             "conv=%s conj=%d detNeg=%d tilted=%d s0=%.2f "
                             "segTris=[%u,%u,%u,%u]",
                             n, (unsigned long long)hash, instRecords.size(),
                             W.pos.x, W.pos.y, W.pos.z, f0[12], f0[13], f0[14],
                             instRowVector ? "row" : "col",
                             g_config.mergeInstanceConjugate ? 1 : 0,
                             detNeg, tilted, f0[15],
                             instSegTris[0], instSegTris[1], instSegTris[2], instSegTris[3]);
                    // Full record dumps for the first shapes: enough raw
                    // data to test any decode hypothesis offline.
                    if (n < 8) {
                        const size_t kMax = instRecords.size() < 16 ? instRecords.size() : 16;
                        for (size_t k = 0; k < kMax; ++k) {
                            const float* f = instRecords[k].data();
                            _MESSAGE("FO4RemixPlugin: [MergeInstRec] #%d k=%zu "
                                     "r=[%.4f,%.4f,%.4f|%.4f,%.4f,%.4f|%.4f,%.4f,%.4f] "
                                     "t=(%.2f,%.2f,%.2f) s=%.3f",
                                     n, k,
                                     f[0], f[1], f[2], f[4], f[5], f[6],
                                     f[8], f[9], f[10],
                                     f[12], f[13], f[14], f[15]);
                        }
                    }
                } else {
                    _MESSAGE("FO4RemixPlugin: [MergeInst] #%d hash=0x%llX stream read "
                             "FAILED -> single-draw fallback leafPos=(%.1f,%.1f,%.1f)",
                             n, (unsigned long long)hash,
                             W.pos.x, W.pos.y, W.pos.z);
                }
            }
        }
    }

    // ---- Submit to Remix ----
    ResolverTrace::g_lastStep.store(Trace::kSubmitStart, std::memory_order_relaxed);

    if (!instRecords.empty()) {
        // Merge-instanced expansion: one drawable per hardware instance.
        // Drawables of one segment share that segment's sub-mesh -- the
        // content-hash mesh cache collapses them onto one Remix mesh/BLAS
        // and OnFrame batches each bucket via InstanceInfoGpuInstancingEXT.
        // Instance 0 keeps the base hash so every existing bookkeeping path
        // (state.meshHash, retry/backoff, sweeps) is untouched; extras get
        // derived hashes recorded in state.extraMeshHashes.
        //
        // Segments: the merged mesh concatenates up to 4 sub-models whose
        // triangle counts sit at +0x1A0 (see instSegTris above), and the
        // record buffer is partitioned into equal contiguous blocks, one
        // per NONZERO segment, in dword order (record count == placements
        // x segments per the duplicate-run evidence). Each segment's index
        // subrange is submitted only with its own record block. When the
        // segment picture doesn't validate (counts don't sum to the parsed
        // triangle count, or records don't divide evenly), fall back to
        // whole mesh x all records -- correct for single-segment shapes,
        // which is the common case.
        //
        // Transform: records are cluster-local and share the engine's
        // row-vector convention (decode proven by the fence-line check:
        // record row0 tracks consecutive piece positions to <1 degree), so
        // compose instance-then-leaf in Bethesda space and reuse
        // BuildRemixTransform for the one Beth->Remix conversion.
        //
        // Extras that fail (VRAM/budget gates) log and are skipped: the
        // cluster renders partially rather than blocking the base drawable.
        // Base failure returns false BEFORE any extra is submitted, so the
        // normal retry path re-runs the whole expansion.
        struct SegDraw {
            uint32_t triStart, triCount;    // into mesh.indices/3
            size_t   recStart, recCount;    // into instRecords
        };
        SegDraw segs[8];
        int nSegs = 0;
        {
            uint32_t totalSegTris = 0;
            int nonZero = 0;
            for (int s = 0; s < 4; ++s) {
                totalSegTris += instSegTris[s];
                if (instSegTris[s]) ++nonZero;
            }
            const uint32_t meshTris = (uint32_t)(mesh.indices.size() / 3);
            if (nonZero >= 2) {
                // Preferred: the engine's own draw parameters for this
                // shape, captured off the D3D11 draw stream by matching
                // the instance-record SRV at VS t7/t8 (draw_capture.h).
                // The engine CPU-instances merged shapes -- one plain
                // DrawIndexed per instance, record index via constant
                // buffer -- so a captured frame is a multiset of index
                // ranges: each unique range is a sub-model (offset into a
                // SHARED index buffer; normalize by the smallest start),
                // its repeat count = that sub-model's instance count times
                // the number of render passes k (depth/main/shadow), which
                // divides out. Record blocks are contiguous per sub-model
                // in first-seen draw order. Anything that doesn't validate
                // re-arms the watch for another frame (up to 5) and then
                // falls back to the take-5 path.
                if (g_config.mergeInstanceDrawCapture && instBufPtr) {
                    std::vector<DrawCapture::SegDraw> cap;
                    if (DrawCapture::Query(instBufPtr, instSrvPtr, hash,
                                           (uint32_t)instRecords.size(),
                                           instSegTris, cap) ==
                            DrawCapture::kReady &&
                        !cap.empty()) {
                        const uint32_t rc = (uint32_t)instRecords.size();
                        // Only the DrawIndexed capture is trusted; kind-0
                        // (DrawIndexedInstanced) entries were only ever
                        // stray leftover-binding draws.
                        std::vector<DrawCapture::SegDraw> uniq;
                        for (const auto& d : cap) {
                            if (d.kind == 1) uniq.push_back(d);
                        }
                        bool ok = !uniq.empty() && uniq.size() <= 8;
                        uint32_t base = UINT32_MAX;
                        uint64_t total = 0;
                        for (const auto& d : uniq) {
                            if (d.startIndex < base) base = d.startIndex;
                            total += d.instanceCount;
                        }
                        const uint32_t k =
                            (ok && rc && total % rc == 0) ? (uint32_t)(total / rc) : 0;
                        ok = ok && k >= 1;
                        SegDraw capSegs[8];
                        int nCapSegs = 0;
                        if (ok) {
                            std::sort(uniq.begin(), uniq.end(),
                                      [](const DrawCapture::SegDraw& a,
                                         const DrawCapture::SegDraw& b) {
                                          return a.order < b.order;
                                      });
                            uint32_t recCursor = 0;
                            for (const auto& d : uniq) {
                                const uint32_t rel = d.startIndex - base;
                                if (d.instanceCount % k != 0 ||
                                    rel % 3 != 0 || d.indexCount == 0 ||
                                    d.indexCount % 3 != 0 ||
                                    (uint64_t)rel + d.indexCount >
                                        mesh.indices.size()) {
                                    ok = false;
                                    break;
                                }
                                const uint32_t recCount = d.instanceCount / k;
                                capSegs[nCapSegs++] = { rel / 3, d.indexCount / 3,
                                                        recCursor, recCount };
                                recCursor += recCount;
                            }
                            ok = ok && recCursor == rc;
                        }
                        static std::atomic<int> sDrawLogs{0};
                        const int dn = sDrawLogs.fetch_add(1, std::memory_order_relaxed);
                        if (dn < 24) {
                            char list[512];
                            size_t pos = 0;
                            list[0] = 0;
                            for (size_t i = 0; i < cap.size() && i < 10; ++i) {
                                const auto& d = cap[i];
                                const int wln = snprintf(list + pos, sizeof(list) - pos,
                                    "%s[k%u idx %u+%u n%u bv%d]", i ? " " : "",
                                    d.kind, d.startIndex, d.indexCount,
                                    d.instanceCount, d.baseVertex);
                                if (wln <= 0 || (size_t)wln >= sizeof(list) - pos) break;
                                pos += (size_t)wln;
                            }
                            _MESSAGE("FO4RemixPlugin: [MergeDraw] #%d hash=0x%llX "
                                     "raw=%zu uniq=%zu rc=%u meshTris=%u base=%u "
                                     "passes=%u ok=%d %s",
                                     dn, (unsigned long long)hash, cap.size(),
                                     uniq.size(), rc, meshTris, base, k,
                                     ok ? 1 : 0, list);
                        }
                        if (ok) {
                            for (int c = 0; c < nCapSegs; ++c) {
                                segs[nSegs++] = capSegs[c];
                            }
                        } else if (DrawCapture::Rearm(hash)) {
                            // Bad frame (partial shadow set, LOD mix):
                            // capture another one before giving up.
                            return false;
                        }
                    }
                }
                if (nSegs == 0 && totalSegTris == meshTris &&
                    instRecords.size() % (size_t)nonZero == 0) {
                    // Equal contiguous blocks on divisibility alone -- the
                    // take-5 semantics, restored 2026-07-03 after the live
                    // run: requiring bit-identical blocks here (plus the
                    // LOD collapse that followed) REGRESSED hedges, the
                    // one user-verified-correct multi-segment case. The
                    // known cost is the proven silent-wrong shape (15 =
                    // "5/5/5" over a true 8+7) rendering with misassigned
                    // records -- accepted until the [MergeProbe] dumps pin
                    // the real per-segment count table.
                    const size_t block = instRecords.size() / (size_t)nonZero;
                    uint32_t triCursor = 0;
                    size_t   recCursor = 0;
                    for (int s = 0; s < 4; ++s) {
                        if (!instSegTris[s]) continue;
                        segs[nSegs++] = { triCursor, instSegTris[s], recCursor, block };
                        triCursor += instSegTris[s];
                        recCursor += block;
                    }
                }
                if (nSegs == 0) {
                    static std::atomic<int> sSegFailLogs{0};
                    const int fn = sSegFailLogs.fetch_add(1, std::memory_order_relaxed);
                    if (fn < 20) {
                        _MESSAGE("FO4RemixPlugin: [MergeSeg] #%d hash=0x%llX partition "
                                 "UNRESOLVED (multi-seg, count=%zu segTris=[%u,%u,%u,%u] "
                                 "meshTris=%u) -> whole-mesh fallback",
                                 fn, (unsigned long long)hash, instRecords.size(),
                                 instSegTris[0], instSegTris[1], instSegTris[2],
                                 instSegTris[3], meshTris);
                    }
                    if (fn < 6) {
                        MergeProbe::Dump(tri, hash);
                    }
                }
            }
            if (nSegs == 0) {
                segs[0] = { 0, meshTris, 0, instRecords.size() };
                nSegs = 1;
            }

            // NOTE(2026-07-03): the LOD-replication collapse that lived here
            // (bit-identical record blocks -> draw only the largest-tri
            // segment) is REMOVED: the live run broke hedges, take 5's one
            // user-verified-correct multi-segment case. The twin-shape LOD
            // evidence it rested on was cross-shape (full-detail twin vs
            // isLOD sibling); within ONE shape, identical blocks evidently
            // include co-placed sub-models sharing placements, where
            // dropping segments loses geometry. Exact-coincident duplicate
            // DRAWS are still skipped per-record in the submit loop below,
            // which is the visually-safe half of that idea.
        }

        const NiTransform& W = tri->m_worldTransform;
        std::vector<uint64_t> extraHashes;
        extraHashes.reserve(instRecords.size() - 1);
        size_t extrasFailed = 0;
        size_t drawIndex = 0;  // 0 keeps the base hash
        const std::vector<uint32_t> fullIndices = std::move(mesh.indices);
        for (int s = 0; s < nSegs; ++s) {
            const SegDraw& sd = segs[s];
            // Segment sub-mesh: index subrange (triangle-aligned, winding
            // flip from the parse preserved); vertices shared as-is --
            // ContentHashOf covers (vertices, indices) so each segment gets
            // its own Remix mesh/BLAS.
            mesh.indices.assign(fullIndices.begin() + (size_t)sd.triStart * 3,
                                fullIndices.begin() + ((size_t)sd.triStart + sd.triCount) * 3);
            for (size_t k = 0; k < sd.recCount; ++k, ++drawIndex) {
                // Skip exact duplicates within this segment's block: two
                // coincident path-traced instances self-Z-fight; vanilla's
                // rasterizer hid them. (Also collapses the fallback path's
                // repeated LOD sets when the segment picture didn't parse.)
                bool dup = false;
                for (size_t p = 0; p < k && !dup; ++p) {
                    dup = std::memcmp(instRecords[sd.recStart + k].data(),
                                      instRecords[sd.recStart + p].data(),
                                      sizeof(float) * 20) == 0;
                }
                if (dup) continue;
                float m[12], comp[12];
                DecodeInstanceRecord(instRecords[sd.recStart + k].data(), instRowVector,
                                     g_config.mergeInstanceConjugate, m);
                ComposeLeafInstance(W, m, comp);
                // Raw buffer instead of `NiTransform xf;`: f4se_minimal
                // declares but does not define NiPoint3's default ctor, so
                // constructing NiTransform directly fails to link.
                alignas(16) unsigned char xfBuf[sizeof(NiTransform)] = {};
                NiTransform& xf = *reinterpret_cast<NiTransform*>(xfBuf);
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        xf.rot.data[r][c] = comp[r * 4 + c];
                    }
                }
                xf.pos.x = comp[3];
                xf.pos.y = comp[7];
                xf.pos.z = comp[11];
                xf.scale = 1.0f;  // leaf scale folded by ComposeLeafInstance
                SemanticCapture::BuildRemixTransform(xf, mesh.worldTransform);

                const uint64_t instHash = (drawIndex == 0)
                    ? hash
                    : hash ^ (0x9E3779B97F4A7C15ULL * (uint64_t)drawIndex);
                auto instStatus = RemixRenderer::SubmitDrawable(instHash, mesh, newTextures);
                if (instStatus == RemixRenderer::SubmitStatus::kSubmitted) {
                    if (drawIndex > 0) extraHashes.push_back(instHash);
                } else if (drawIndex == 0) {
                    ResolverTrace::g_lastStep.store(Trace::kSubmitFailed,
                                                    std::memory_order_relaxed);
                    return false;
                } else {
                    ++extrasFailed;
                }
            }
        }
        if (extrasFailed > 0) {
            _MESSAGE("FO4RemixPlugin: [MergeInst] hash=0x%llX %zu/%zu extra instances "
                     "failed to submit (cluster renders partially)",
                     (unsigned long long)hash, extrasFailed, instRecords.size() - 1);
        }
        state.extraMeshHashes = std::move(extraHashes);
    } else {
        auto status = RemixRenderer::SubmitDrawable(hash, mesh, newTextures);
        if (status != RemixRenderer::SubmitStatus::kSubmitted) {
            ResolverTrace::g_lastStep.store(Trace::kSubmitFailed, std::memory_order_relaxed);
            return false;
        }
    }
    ResolverTrace::g_lastStep.store(Trace::kSubmitOK, std::memory_order_relaxed);

    // Update DrawableState to mark submission and track refcount targets.
    state.submittedToRemix = true;
    state.meshHash = hash;
    // Note: state.materialHash is left at 0 here. SubmitDrawable's hash
    // computation is internal to remix_renderer; if you want symmetric
    // tracking, expose a helper or have SubmitDrawable take an out-param.
    // For 1B, ReleaseDrawable looks up by `hash` and finds the materialHash
    // via g_drawables, so leaving state.materialHash at 0 is fine -- the
    // refcount cleanup goes through g_drawables anyway.
    // LOD-overlap diagnostic (2026-04-28): include geometry name, the
    // static IsMeshLOD bit (from initialFlags captured on first-seen),
    // the live flag word at submit time, technique flag from the hook
    // arg, world position, and the parent NiNode chain (2 levels up,
    // captured in the detour). Parent reads are guarded by null checks;
    // the resolver is wrapped in SEH at the caller, so a stale parent
    // pointer that survived freshness gating gets caught upstream.
    const char* meshName = obj->m_name.c_str();
    const bool  isLOD = ((state.initialFlags >> 12) & 1ULL) != 0;

    const char* p1Name = "";
    uint64_t    p1Flags = 0;
    const char* p2Name = "";
    uint64_t    p2Flags = 0;
    if (state.parent1) {
        auto* pn = static_cast<NiAVObject*>(state.parent1);
        const char* n = pn->m_name.c_str();
        p1Name = n ? n : "";
        p1Flags = pn->flags;
    }
    if (state.parent2) {
        auto* pn = static_cast<NiAVObject*>(state.parent2);
        const char* n = pn->m_name.c_str();
        p2Name = n ? n : "";
        p2Flags = pn->flags;
    }

    // Alpha-test diagnostic (2026-04-29): emit material type, BSShaderProperty
    // shader-flags, and the contents of geo->effectState (the NiAlphaProperty
    // slot at offset 0x130 on BSGeometry per f4se BSGeometry.h). For foliage
    // drawables that render as solid alpha cards, we want to know which
    // alpha-test signal source the engine is using -- NiAlphaProperty (geo
    // level), BSLightingShaderProperty::flags (shader level), or BSLighting-
    // ShaderMaterialBase fields (material level, requires GetType discriminator).
    const uint32_t matType = mat ? mat->GetType() : 0xFFFFFFFFu;
    uint64_t       propFlags = 0;
    if (state.property) {
        propFlags = *reinterpret_cast<uint64_t*>(
            reinterpret_cast<uintptr_t>(state.property) + 0x30);
    }
    void*    effectState     = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(tri) + 0x130);
    uint16_t alphaFlags      = 0;
    uint8_t  alphaThreshold  = 0;
    if (effectState) {
        alphaFlags     = *reinterpret_cast<uint16_t*>(
            reinterpret_cast<uintptr_t>(effectState) + 0x28);
        alphaThreshold = *reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(effectState) + 0x2A);
    }

    // Rotation+scale dump (PROBE 2026-05-03): roads/statics rendering flat
    // when bUsePreCombines=0; need to see whether m_worldTransform.rot
    // arrives as identity (rotation lost upstream) or with the slope intact
    // (then BuildRemixTransform / Remix submission is the leak).
    const auto& rot = tri->m_worldTransform.rot;
    const float scale = tri->m_worldTransform.scale;
    // Leaf RTTI class name (PROBE 2026-05-03): identify whether road/static
    // sub-meshes are plain BSTriShape vs BSMergeInstancedTriShape vs another
    // subclass with per-instance transform attributes we don't handle.
    char leafClass[64] = "";
    SemanticCapture::GetLeafClassName(reinterpret_cast<void*>(tri),
                                      leafClass, sizeof(leafClass));
    _MESSAGE("FO4RemixPlugin: [Resolver] submitted hash=0x%llX name=\"%s\" "
             "leafClass=\"%s\" "
             "isLOD=%d isDecal=%d flags=0x%016llX tech=0x%08X pos=(%.1f,%.1f,%.1f) "
             "p1=\"%s\"(0x%016llX) p2=\"%s\"(0x%016llX) "
             "matType=%u propFlags=0x%016llX effectState=%p "
             "alphaFlags=0x%04X alphaThreshold=%u alphaTestEnabled=%d "
             "alphaBlendEnabled=%d srcFactor=%u dstFactor=%u "
             "rot=[%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f] scale=%.3f",
             (unsigned long long)hash,
             meshName ? meshName : "(null)",
             leafClass[0] ? leafClass : "(unknown)",
             isLOD ? 1 : 0,
             mesh.isDecal ? 1 : 0,
             (unsigned long long)state.lastFlags,
             state.lastTechniqueFlags,
             tri->m_worldTransform.pos.x,
             tri->m_worldTransform.pos.y,
             tri->m_worldTransform.pos.z,
             p1Name, (unsigned long long)p1Flags,
             p2Name, (unsigned long long)p2Flags,
             matType, (unsigned long long)propFlags, effectState,
             alphaFlags, alphaThreshold, mesh.alphaTestEnabled ? 1 : 0,
             mesh.alphaBlendEnabled ? 1 : 0,
             mesh.srcColorBlendFactor, mesh.dstColorBlendFactor,
             rot.data[0][0], rot.data[0][1], rot.data[0][2],
             rot.data[1][0], rot.data[1][1], rot.data[1][2],
             rot.data[2][0], rot.data[2][1], rot.data[2][2],
             scale);
    for (const auto& t : newTextures) {
        state.textureHashes.insert(t.hash);
    }

    // Reset trace so we can tell when we're between resolver calls.
    ResolverTrace::g_lastStep.store(Trace::kIdle, std::memory_order_relaxed);
    return true;
}

}  // namespace Lighting

// ---- Trace accessors (for SEH handler in semantic_capture.cpp) ----
namespace Trace {
    int LastStep() {
        return ResolverTrace::g_lastStep.load(std::memory_order_relaxed);
    }
    uint64_t LastHash() {
        return ResolverTrace::g_lastHash.load(std::memory_order_relaxed);
    }
    const char* StepName(int s) {
        return ResolverTrace::StepName(s);
    }
    void SetStep(int s) {
        ResolverTrace::g_lastStep.store(s, std::memory_order_relaxed);
    }
}

}  // namespace Resolvers
