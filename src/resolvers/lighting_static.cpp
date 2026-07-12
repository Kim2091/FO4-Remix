#include "lighting_static.h"

#include "../bs_extraction.h"
#include "../semantic_capture.h"
#include "../remix_renderer.h"
#include "../skinned_meshes.h"
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
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

    // low region +0x140..+0x187 (9 qwords): pointer-rich (the +0x170
    // record-buffer wrapper lives here); chase heap-like qwords one level
    // hunting per-sub-model tables the +0x1D0 route never was.
    uint64_t lq[9];
    if (PeekQwordsGuarded(reinterpret_cast<const void*>(
            reinterpret_cast<uintptr_t>(tri) + 0x140), lq, 9)) {
        FormatQwordsHex(lq, 9, buf, sizeof(buf));
        _MESSAGE("FO4RemixPlugin: [MergeProbe] hash=0x%llX low(+0x140)=[%s]",
                 (unsigned long long)hash, buf);
        int chased = 0;
        for (int i = 0; i < 9 && chased < 3; ++i) {
            if (!HeapLikePtr(lq[i])) continue;
            uint64_t tq[16];
            if (!PeekQwordsGuarded(reinterpret_cast<const void*>(lq[i]), tq, 16)) {
                continue;
            }
            FormatQwordsHex(tq, 16, buf, sizeof(buf));
            _MESSAGE("FO4RemixPlugin: [MergeProbe] hash=0x%llX chase +0x%X=%016llX "
                     "q=[%s]", (unsigned long long)hash, 0x140 + i * 8,
                     (unsigned long long)lq[i], buf);
            ++chased;
        }
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

// Take-9 chunk diagnostics: captured baked-chunk IA identities are raw
// pointers with no reference held; validate the vtable lives in d3d11 and
// QI before touching (the deferral keeps resolve within a frame or two of
// capture, but the engine could still have freed them).
static ID3D11Buffer* SafeBufferFromIdentity(void* p) {
    if (!HeapLikePtr((uint64_t)(uintptr_t)p)) return nullptr;
    uint64_t vtbl = 0;
    if (!PeekQwordsGuarded(p, &vtbl, 1) || !PtrInD3D11(vtbl)) return nullptr;
    void* raw = nullptr;
    if (!QiGuarded(p, &__uuidof(ID3D11Buffer), &raw)) return nullptr;
    return static_cast<ID3D11Buffer*>(raw);
}

// Guarded bulk copy for engine-structure reads (POD only; SEH cannot
// coexist with C++ unwinding in one function).
static bool PeekBytesGuarded(const void* src, void* dst, size_t bytes) {
    __try {
        std::memcpy(dst, src, bytes);
        return true;
    } __except (1) {
        return false;
    }
}

// Half-float decode comes from config.h's shared HalfToFloat (a local copy
// here used to shadow it with divergent denormal handling -- a silent trap
// for anyone fixing the shared one; removed 2026-07-10).

// ---- Take 11 (scene-graph baked-chunk walk) REMOVED (2026-07-04) ----
// Live re-verification (frida, same save/binary) disproved every premise:
// no baked-chunk BSTriShapes exist under the cell's BSMultiBoundNode (the
// MB-parented stride-32 shapes are cell terrain, 289-vert/512-tri LOD
// quads), flags bits 35/43 are TRANSIENT batch-renderer state (verified
// toggling between reads seconds apart; the engine's traversal SKIPS
// bit-35 nodes), the wrapper +0x38 field is a refcount (a shared IB
// wrapper read 101), and no scene-graph geometry shares the merge
// shape's shaderProperty. Run 2's one-time success was a race on the
// transient flag. The reproducible ground truth for baked cluster
// geometry is the engine's own draw stream (take 10 below) plus the
// capture-upgrade path that re-resolves once the engine draws the
// cluster. Removed here: BakedChunk, CollectBakedChunks,
// BuildMeshFromBakedChunks, g_bakedChunkOwner, g_bakedMeshCache.

// ---- Baked expanded-mesh rebuild (take 10, 2026-07-03) ----
// The engine renders BSMergeInstancedTriShape from a PRE-BAKED expanded
// mesh: every placed instance's geometry duplicated into large shared
// IB/VB pools, positions already CLUSTER-LOCAL (take-9 readback proof:
// float3 spanning hundreds of units matching record translations, unit
// normals as biased ubytes, half2 UVs). So instead of partitioning the
// record buffer (five failed takes), rebuild our Remix mesh FROM the
// engine's own baked chunks and submit it with just the leaf transform:
// placement, rotation, and sub-model pairing are the engine's own.
//
// Captured chunk lists contain stray draws (the record SRV lingers bound
// across unrelated geometry -- a 4km quad in a 20-byte format showed up).
// Filters: exact baked vertex format (stride 32, R16_UINT indices) and
// every sampled position inside the shape's world bound (leaf rotations
// are identity on every sampled merged shape, so the local-space bound is
// just the world bound recentered on the leaf position).
static bool BuildMeshFromChunks(BSTriShape* tri,
                                const DrawCapture::ChunkDraw* chunks, int nChunks,
                                uint64_t hash,
                                bool useVertexColors,
                                const std::vector<std::array<float, 20>>& recs,
                                const std::vector<remixapi_HardcodedVertex>& srcVerts,
                                std::vector<remixapi_HardcodedVertex>& outVerts,
                                std::vector<uint32_t>& outIndices,
                                int& keptChunks) {
    keptChunks = 0;
    const NiTransform& W = tri->m_worldTransform;
    const NiBound& B = tri->m_worldBound;
    const float cx = B.m_kCenter.x - W.pos.x;
    const float cy = B.m_kCenter.y - W.pos.y;
    const float cz = B.m_kCenter.z - W.pos.z;
    const float rad = B.m_fRadius * 1.6f + 128.0f;
    const float radSq = rad * rad;
    // Record-anchored gate (run-5 lesson): a 352-tri source cluster baked
    // 12,223 tris because unrelated DrawIndexed geometry -- neighbor
    // clusters' chunks in the same shared pools -- was drawn while our
    // record SRV sat sticky at t8, and the cluster-radius filter (r up to
    // ~1800) happily kept in-radius neighbors. The expanded mesh is OUR
    // source pieces duplicated at OUR record translations, so every valid
    // chunk vertex must lie within source-extent of at least one record
    // translation. rSrc = parsed source mesh extent (pieces are authored
    // around the leaf origin), scaled per record.
    float rSrcSq = 0.0f;
    for (const remixapi_HardcodedVertex& v : srcVerts) {
        const float d = v.position[0] * v.position[0] +
                        v.position[1] * v.position[1] +
                        v.position[2] * v.position[2];
        if (d > rSrcSq) rSrcSq = d;
    }
    float rSrc = std::sqrt(rSrcSq);
    if (rSrc < 64.0f) rSrc = 64.0f;
    struct RecAnchor { float x, y, z, reach2; };
    std::vector<RecAnchor> anchors;
    anchors.reserve(recs.size());
    for (const auto& r : recs) {
        const float* f = r.data();
        const float s = (f[15] > 1.0f && f[15] < 20.0f) ? f[15] : 1.0f;
        const float reach = rSrc * s + 96.0f;
        anchors.push_back({ f[12], f[13], f[14], reach * reach });
    }
    int dFmt = 0, dIb = 0, dWin = 0, dVb = 0, dBound = 0, dRec = 0;

    for (int c = 0; c < nChunks; ++c) {
        const DrawCapture::ChunkDraw& ch = chunks[c];
        if (ch.ibFormat != 57 /*R16_UINT*/ || ch.vbStride != 32 ||
            ch.idxCount == 0 || ch.idxCount % 3 != 0 || ch.idxCount > 200000) {
            ++dFmt;
            continue;
        }
        ID3D11Buffer* ibb = SafeBufferFromIdentity(ch.ib);
        if (!ibb) { ++dIb; continue; }
        std::vector<uint8_t> ibBytes;
        const bool ibOk = ReadbackBufferSlice(ibb, ch.ibOffset, ch.idxCount * 2,
                                              ibBytes) == ch.idxCount * 2;
        ibb->Release();
        if (!ibOk) { ++dIb; continue; }
        const uint16_t* idx = reinterpret_cast<const uint16_t*>(ibBytes.data());
        uint16_t mn = 0xFFFF, mx = 0;
        for (uint32_t i = 0; i < ch.idxCount; ++i) {
            if (idx[i] < mn) mn = idx[i];
            if (idx[i] > mx) mx = idx[i];
        }
        const uint32_t nVerts = (uint32_t)mx - mn + 1;
        if (nVerts > 70000) { ++dWin; continue; }
        ID3D11Buffer* vbb = SafeBufferFromIdentity(ch.vb);
        if (!vbb) { ++dVb; continue; }
        std::vector<uint8_t> vbytes;
        const bool vbOk = ReadbackBufferSlice(vbb, ch.vbOffset + (uint32_t)mn * 32u,
                                              nVerts * 32u, vbytes) == nVerts * 32u;
        vbb->Release();
        if (!vbOk) { ++dVb; continue; }
        // Every vertex must (a) land inside the cluster bound and (b) lie
        // within source-extent of SOME record translation. (a) kills
        // NaN/garbage from repacked pool slices (comparisons with NaN are
        // false); (b) kills in-radius NEIGHBOR geometry drawn under the
        // sticky t8 binding. One bad vertex disqualifies the whole chunk.
        bool inside = true;
        bool anchored = true;
        for (uint32_t v = 0; v < nVerts && inside && anchored; ++v) {
            float p[3];
            std::memcpy(p, vbytes.data() + v * 32u, 12);
            const float dx = p[0] - cx, dy = p[1] - cy, dz = p[2] - cz;
            inside = (dx * dx + dy * dy + dz * dz) <= radSq;
            if (!inside) break;
            bool nearRec = false;
            for (const RecAnchor& a : anchors) {
                const float ax = p[0] - a.x, ay = p[1] - a.y, az = p[2] - a.z;
                if (ax * ax + ay * ay + az * az <= a.reach2) { nearRec = true; break; }
            }
            anchored = nearRec;
        }
        if (!inside) { ++dBound; continue; }
        if (!anchored) { ++dRec; continue; }
        // append: positions float3@0, UV half2@16, normal biased-ubyte@20,
        // packed color@28. Keep color neutral when the shader doesn't consume
        // it (or uses color.r as a grayscale-palette selector).
        const uint32_t vbase = (uint32_t)outVerts.size();
        outVerts.resize(vbase + nVerts);
        for (uint32_t v = 0; v < nVerts; ++v) {
            const uint8_t* p = vbytes.data() + v * 32u;
            remixapi_HardcodedVertex& hv = outVerts[vbase + v];
            std::memset(&hv, 0, sizeof(hv));
            std::memcpy(hv.position, p, 12);
            uint16_t uvh[2];
            std::memcpy(uvh, p + 16, 4);
            hv.texcoord[0] = HalfToFloat(uvh[0]);
            hv.texcoord[1] = HalfToFloat(uvh[1]);
            float nx = (p[20] / 255.0f) * 2.0f - 1.0f;
            float ny = (p[21] / 255.0f) * 2.0f - 1.0f;
            float nz = (p[22] / 255.0f) * 2.0f - 1.0f;
            const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (nl > 0.25f) {
                nx /= nl; ny /= nl; nz /= nl;
            } else {
                nx = 0.0f; ny = 0.0f; nz = 1.0f;
            }
            hv.normal[0] = nx;
            hv.normal[1] = ny;
            hv.normal[2] = nz;
            if (useVertexColors) {
                std::memcpy(&hv.color, p + 28, 4);
            } else {
                hv.color = 0xFFFFFFFF;
            }
        }
        // same per-triangle 1<->2 winding flip the parser applies
        const size_t ibase = outIndices.size();
        outIndices.resize(ibase + ch.idxCount);
        for (uint32_t t = 0; t + 2 < ch.idxCount; t += 3) {
            outIndices[ibase + t]     = vbase + (uint32_t)(idx[t] - mn);
            outIndices[ibase + t + 1] = vbase + (uint32_t)(idx[t + 2] - mn);
            outIndices[ibase + t + 2] = vbase + (uint32_t)(idx[t + 1] - mn);
        }
        ++keptChunks;
    }
    const bool ok = keptChunks > 0 && outIndices.size() >= 48;
    static std::atomic<int> sBakeLogs{0};
    const int bl = sBakeLogs.fetch_add(1, std::memory_order_relaxed);
    if (bl < 48) {
        _MESSAGE("FO4RemixPlugin: [MergeBake] hash=0x%llX %s chunks=%d/%d tris=%zu "
                 "verts=%zu rSrc=%.0f recs=%zu drop=[fmt%d ib%d win%d vb%d bnd%d rec%d] "
                 "bound=(%.0f,%.0f,%.0f r=%.0f)",
                 (unsigned long long)hash, ok ? "OK" : "REJECT", keptChunks, nChunks,
                 outIndices.size() / 3, outVerts.size(), rSrc, recs.size(),
                 dFmt, dIb, dWin, dVb, dBound, dRec, cx, cy, cz, rad);
    }
    return ok;
}

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

// ---------------------------------------------------------------------------
// [WindDiag] inside-out investigation (2026-07-08). PArig / street-lamp
// shapes render backface-culled from the front while sharing parse code, the
// universal winding flip, and transform conventions with content that renders
// correctly. Log every facing-relevant quantity for the affected shapes
// (diffuse texture path contains "parig" or "streetlamp") plus the first few
// generic opaque statics as references, so the discriminator -- vertex-format
// decode bug, subclass parse branch, transform determinant / scale sign, or
// winding-vs-normals parity -- can be read off a single run. Note the
// transform proof to date covered leaf ROTATIONS (det>0); BuildRemixTransform
// multiplies xf.scale into the linear part, so a negative scale would flip
// facing while passing that proof -- rotDet and scale are logged separately.
// Capped (24 targets + 8 refs), hash-deduped, ~7 lines per shape.
// ---------------------------------------------------------------------------
static bool NameContainsCI(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return false;
    const size_t n = std::strlen(needle);
    for (const char* p = hay; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i] &&
               std::tolower((unsigned char)p[i]) ==
                   std::tolower((unsigned char)needle[i]))
            ++i;
        if (i == n) return true;
    }
    return false;
}

static float WindDiagDet3(const float m[3][4]) {  // det of the 3x3 block
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
         - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
         + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

// ---------------------------------------------------------------------------
// [HeadDiag] missing-FaceGen-heads investigation (2026-07-08). Heads never
// render while every other skinned mesh works; the candidate exits are all
// SILENT (the post-parse hasSkinning gate, Register's false paths, the
// noDiffuse retry). Trace every resolver gate for any shape whose own name
// or captured parent names look like a head part. Logs are per-hash deduped
// on message CONTENT: a retrying drawable that keeps failing the same gate
// logs once, and logs again only when it progresses to a different gate.
// "Headlamp"/"Bulkhead"-style false positives are acceptable noise for a
// capped diagnostic.
// ---------------------------------------------------------------------------
static bool HeadDiagNameMatch(const char* n) {
    static const char* const kKeys[] = {
        "head", "hair", "eye", "mouth", "teeth", "beard", "face", "brow"
    };
    for (const char* k : kKeys)
        if (NameContainsCI(n, k)) return true;
    return false;
}

static bool HeadDiagMatch(BSTriShape* tri,
                          const SemanticCapture::DrawableState& state) {
    if (HeadDiagNameMatch(tri->m_name.c_str())) return true;
    if (state.parent1 &&
        HeadDiagNameMatch(static_cast<NiAVObject*>(state.parent1)->m_name.c_str()))
        return true;
    if (state.parent2 &&
        HeadDiagNameMatch(static_cast<NiAVObject*>(state.parent2)->m_name.c_str()))
        return true;
    return false;
}

static void HeadDiagLog(uint64_t hash, const char* fmt, ...) {
    char msg[448];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    // Per-hash SET of already-logged messages, not just the last one: a
    // retrying shape alternates "seen" / gate lines every backoff tick, and
    // last-message dedup let that alternation burn ~a third of the cap on
    // two shapes (2026-07-08 run 1).
    static std::mutex s_mx;
    static std::unordered_map<uint64_t, std::unordered_set<std::string>> s_seen;
    static int s_logs = 0;
    constexpr int kCap = 160;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (s_logs >= kCap) return;
        if (!s_seen[hash].insert(msg).second) return;
        ++s_logs;
    }
    _MESSAGE("FO4RemixPlugin: [HeadDiag] hash=%016llX %s",
             (unsigned long long)hash, msg);
}

static void LogWindingDiag(BSTriShape* tri,
                           const ExtractedMesh& mesh,
                           const ParsedGeometry& parsed,
                           BSLightingShaderMaterialBase* mat,
                           uint64_t propFlags,
                           const SemanticCapture::DrawableState& state)
{
    NiTexture* dif = mat->spDiffuseTexture;
    const char* texName = dif ? dif->name.c_str() : nullptr;
    const bool isTarget = NameContainsCI(texName, "parig") ||
                          NameContainsCI(texName, "streetlamp");

    // References: only meaty opaque statics -- menu clutter and blended
    // effects would burn the ref cap before the player reaches a lamp.
    if (!isTarget && (mesh.indices.size() < 300 || mesh.isDecal ||
                      mesh.alphaBlendEnabled))
        return;

    static std::mutex s_mx;
    static std::unordered_set<uint64_t> s_logged;
    static int s_targets = 0, s_refs = 0;
    constexpr int kMaxTargets = 24, kMaxRefs = 8;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (s_logged.count(mesh.hash)) return;
        if (isTarget) { if (s_targets >= kMaxTargets) return; ++s_targets; }
        else          { if (s_refs    >= kMaxRefs)    return; ++s_refs;    }
        s_logged.insert(mesh.hash);
    }

    const char* tag = isTarget ? "TARGET" : "REF";
    const unsigned long long h = (unsigned long long)mesh.hash;

    char leaf[64] = "?";
    SemanticCapture::GetLeafClassName(tri, leaf, sizeof(leaf));
    char p1Leaf[64] = "", p2Leaf[64] = "";
    const char* p1Name = "";
    const char* p2Name = "";
    if (state.parent1) {
        SemanticCapture::GetLeafClassName(state.parent1, p1Leaf, sizeof(p1Leaf));
        const char* n = static_cast<NiAVObject*>(state.parent1)->m_name.c_str();
        if (n) p1Name = n;
    }
    if (state.parent2) {
        SemanticCapture::GetLeafClassName(state.parent2, p2Leaf, sizeof(p2Leaf));
        const char* n = static_cast<NiAVObject*>(state.parent2)->m_name.c_str();
        if (n) p2Name = n;
    }

    _MESSAGE("FO4RemixPlugin: [WindDiag] %s hash=%016llX shape=\"%s\" leaf=%s "
             "dyn=%d nV=%u nT=%u tex=\"%s\"",
             tag, h, tri->m_name.c_str() ? tri->m_name.c_str() : "",
             leaf, parsed.isDynamic ? 1 : 0,
             (unsigned)tri->numVertices, (unsigned)tri->numTriangles,
             texName ? texName : "(null)");
    _MESSAGE("FO4RemixPlugin: [WindDiag] %s hash=%016llX parents p1=\"%s\"(%s) "
             "p2=\"%s\"(%s) niFlags=%016llX propFlags=%016llX",
             tag, h, p1Name, p1Leaf, p2Name, p2Leaf,
             (unsigned long long)state.initialFlags,
             (unsigned long long)propFlags);

    // Vertex-format decode, exactly as ParseShapeGeometry computes it
    // (attribute nibbles are absolute; dynamic shapes' descs arrive
    // pre-rebased by the engine's facegen conversion, so no n1 term there).
    const uint64_t desc = parsed.vertexDesc;
    const uint32_t szVertex  = (uint32_t)((desc >> 4) & 0xF);
    const uint32_t attrShift = parsed.isDynamic ? 0 : szVertex;
    const uint32_t oUV      = (attrShift + (uint32_t)((desc >>  8) & 0xF)) * 4;
    const uint32_t oNormal  = (attrShift + (uint32_t)((desc >> 16) & 0xF)) * 4;
    const uint32_t oColor   = (attrShift + (uint32_t)((desc >> 24) & 0xF)) * 4;
    const bool halfPos = !(desc & BSGeometry::kFlag_FullPrecision);
    _MESSAGE("FO4RemixPlugin: [WindDiag] %s hash=%016llX desc=%016llX stride=%u "
             "szV=%u oUV=%u oN=%u oC=%u halfPos=%d hasUV=%d hasN=%d hasC=%d",
             tag, h, (unsigned long long)desc, (unsigned)parsed.vertexSize,
             szVertex, oUV, oNormal, oColor, halfPos ? 1 : 0,
             (desc & BSGeometry::kFlag_UVs) ? 1 : 0,
             (desc & BSGeometry::kFlag_Normals) ? 1 : 0,
             (desc & BSGeometry::kFlag_VertexColors) ? 1 : 0);

    // Transform: engine leaf transform (rot det and scale SEPARATELY) plus
    // the determinant of the 3x3 actually submitted to Remix (reflection
    // expected: X/Y swap makes it negative when rotDet*scale^3 is positive).
    const NiTransform& xf = tri->m_worldTransform;
    float rd[3][4];
    std::memcpy(rd, xf.rot.data, sizeof(rd));
    const float rotDet = WindDiagDet3(rd);
    const float subDet = WindDiagDet3(mesh.worldTransform);
    _MESSAGE("FO4RemixPlugin: [WindDiag] %s hash=%016llX xf pos=(%.1f,%.1f,%.1f) "
             "scale=%.4f rotDet=%.4f submittedDet=%.4f twoSided=%d decal=%d "
             "matType=%u aTest=%d aBlend=%d",
             tag, h, xf.pos.x, xf.pos.y, xf.pos.z, xf.scale, rotDet, subDet,
             mesh.isTwoSided ? 1 : 0, mesh.isDecal ? 1 : 0,
             (unsigned)mat->GetType(),
             mesh.alphaTestEnabled ? 1 : 0, mesh.alphaBlendEnabled ? 1 : 0);

    // Winding-vs-authored-normals parity over ALL triangles, in OBJECT space,
    // on the SUBMITTED (post-flip) index order. meanDot ~ -1 is the majority
    // convention observed by [MergeWind]; a target at ~+1 means its runtime
    // winding parity is genuinely opposite to the bulk of content.
    double dotSum = 0.0;
    uint32_t dotN = 0, negN = 0, posN = 0;
    const size_t triCount = mesh.indices.size() / 3;
    for (size_t t = 0; t < triCount; ++t) {
        const auto& a = mesh.vertices[mesh.indices[t * 3 + 0]];
        const auto& b = mesh.vertices[mesh.indices[t * 3 + 1]];
        const auto& c = mesh.vertices[mesh.indices[t * 3 + 2]];
        const float e1[3] = { b.position[0] - a.position[0],
                              b.position[1] - a.position[1],
                              b.position[2] - a.position[2] };
        const float e2[3] = { c.position[0] - a.position[0],
                              c.position[1] - a.position[1],
                              c.position[2] - a.position[2] };
        const float g[3] = { e1[1] * e2[2] - e1[2] * e2[1],
                             e1[2] * e2[0] - e1[0] * e2[2],
                             e1[0] * e2[1] - e1[1] * e2[0] };
        const float n[3] = { a.normal[0] + b.normal[0] + c.normal[0],
                             a.normal[1] + b.normal[1] + c.normal[1],
                             a.normal[2] + b.normal[2] + c.normal[2] };
        const float gl = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (gl < 1e-12f || nl < 1e-6f) continue;
        const float d = (g[0] * n[0] + g[1] * n[1] + g[2] * n[2]) / (gl * nl);
        dotSum += d;
        ++dotN;
        if (d < 0.0f) ++negN; else ++posN;
    }
    _MESSAGE("FO4RemixPlugin: [WindDiag] %s hash=%016llX parity meanDot=%.4f "
             "neg=%u pos=%u of %u",
             tag, h, dotN ? (float)(dotSum / dotN) : 0.0f, negN, posN, dotN);

    // First triangle, submitted order (raw NIF/runtime order = corners 0,2,1
    // of this -- the universal flip is an involution).
    if (triCount > 0 && mesh.indices.size() >= 3) {
        const uint32_t i0 = mesh.indices[0], i1 = mesh.indices[1],
                       i2 = mesh.indices[2];
        const auto& a = mesh.vertices[i0];
        const auto& b = mesh.vertices[i1];
        const auto& c = mesh.vertices[i2];
        _MESSAGE("FO4RemixPlugin: [WindDiag] %s hash=%016llX tri0 idx=(%u,%u,%u) "
                 "p0=(%.3f,%.3f,%.3f) p1=(%.3f,%.3f,%.3f) p2=(%.3f,%.3f,%.3f) "
                 "n0=(%.2f,%.2f,%.2f)",
                 tag, h, i0, i1, i2,
                 a.position[0], a.position[1], a.position[2],
                 b.position[0], b.position[1], b.position[2],
                 c.position[0], c.position[1], c.position[2],
                 a.normal[0], a.normal[1], a.normal[2]);
    }

    // Raw bytes of vertex 0 straight from the runtime VB, for byte-level
    // comparison against the NIF ground truth (BA2 extraction).
    if (parsed.vbData && parsed.vertexSize > 0) {
        const uint32_t nBytes = parsed.vertexSize < 48 ? parsed.vertexSize : 48;
        char hex[48 * 3 + 1];
        for (uint32_t i = 0; i < nBytes; ++i)
            std::snprintf(hex + i * 3, 4, "%02X ", parsed.vbData[i]);
        hex[nBytes * 3] = '\0';
        _MESSAGE("FO4RemixPlugin: [WindDiag] %s hash=%016llX v0raw=%s",
                 tag, h, hex);
    }
}

// Phase-1 output cached across texture-pending retries (2026-07-09 pop-in
// speed; stored type-erased in DrawableState::resolveCache). Holds the fully
// built mesh (vertices/indices/transform/flags/skinning; texture-hash fields
// are refreshed every attempt by the extraction calls) plus the small tint
// derivation results, so a retry whose textures are still in the async
// readback/decode pipeline skips the per-vertex parse work entirely.
struct ResolveCache {
    ExtractedMesh mesh;
    uint8_t  albedoLumFloor = 0;
    // Tint from the skin/hair branch, or 0xFFFFFF. For palette meshes this
    // stays 0xFFFFFF (the branches are exclusive); the LUT fallback tint is
    // re-sampled per attempt from paletteRowByte below.
    uint32_t diffuseTint    = 0xFFFFFFu;
    bool     paletteBranch  = false;  // grayscale-to-palette branch was taken
    uint32_t paletteRowByte = 255;    // per-mesh MODE of the vc red byte
};

// Retry-backoff ceiling for keeping the phase-1 cache alive. Beyond this
// many failed attempts the delays go exponential (seconds-scale waits), and
// holding a full vertex copy for a drawable that may never resolve isn't
// worth the memory -- the eventual retry just re-parses.
constexpr uint32_t kResolveCacheMaxAttempts = 12;

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

    // Skinned meshes (revived 2026-07-08 behind [Skinning] Enabled): parsed
    // with blend weights/indices, registered with SkinnedMeshes for per-Tick
    // bone updates, and submitted with the bare mirror-P instance transform
    // (bones produce Beth world coordinates). See skinned_meshes.h for the
    // composition math and the post-mortem of the retired pre-1B module.
    const bool isSkinned = (tri->vertexDesc & BSGeometry::kFlag_Skinned) != 0;

    // [HeadDiag]: identity line fires once per hash (dedup on content), then
    // each gate below reports the first time this shape fails it. Gated on
    // Diagnostics.Enabled -- the match itself is up to 3x8 case-insensitive
    // substring scans per drawable per attempt, pure scaffolding from the
    // (resolved) missing-FaceGen-heads investigation.
    const bool headDiag = g_config.diagEnabled && HeadDiagMatch(tri, state);
    if (headDiag) {
        char leaf[64] = "?";
        SemanticCapture::GetLeafClassName(tri, leaf, sizeof(leaf));
        const char* p1 = state.parent1
            ? static_cast<NiAVObject*>(state.parent1)->m_name.c_str() : nullptr;
        const char* p2 = state.parent2
            ? static_cast<NiAVObject*>(state.parent2)->m_name.c_str() : nullptr;
        // vbSize / numVertices = the PHYSICAL static-VB stride: confirms the
        // facegen conversion's shared VB really is the position-stripped
        // 24-byte layout the rewritten desc claims (the one open question in
        // scripts/dynamic_trishape_desc.md).
        uint32_t vbSize = 0;
        if (auto* gfx = static_cast<BSGraphics::TriShape*>(tri->pRendererData)) {
            if (gfx->pVB) vbSize = gfx->pVB->uiDataSize;
        }
        HeadDiagLog(hash,
                    "seen shape=\"%s\" leaf=%s dyn=%d desc=%016llX skinned=%d "
                    "nV=%u nT=%u vbSize=%u p1=\"%s\" p2=\"%s\"",
                    tri->m_name.c_str() ? tri->m_name.c_str() : "", leaf,
                    tri->GetAsBSDynamicTriShape() ? 1 : 0,
                    (unsigned long long)tri->vertexDesc, isSkinned ? 1 : 0,
                    (unsigned)tri->numVertices, (unsigned)tri->numTriangles,
                    vbSize, p1 ? p1 : "", p2 ? p2 : "");
    }

    if (isSkinned && !g_config.skinningEnabled) {
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
    // Merged bit 37 (flags2 bit 5) means "geometry HAS a vertex-color stream"
    // -- decompiler-proven for this build (SetFlag(prop,0x25,...) driven by the
    // vertexDesc color nibble; scripts/dynamic_trishape_desc.md). The vanilla
    // combine is a straight albedo.rgb *= vertexColor.rgb.
    constexpr uint64_t kFlag_VertexColors = 1ULL << 37;
    // SLSF1 GrayscaleToPaletteColor (merged bit 4, mask 0x10): the diffuse is
    // GRAYSCALE and vertexColor.x is a PALETTE-ROW selector into spLookupTexture
    // -- NOT a tint. Multiplying that near-gray selector into the grayscale
    // diffuse is what turned blue denim (and clothing/clutter generally) gray
    // (2026-07-08). For these meshes DON'T multiply the vertex color; the real
    // color is applied below as a LUT-sampled diffuse tint.
    constexpr uint64_t kFlag_GrayscaleToPalette = 0x10ULL;
    const bool isGrayscaleToPalette = (propFlagsEarly & kFlag_GrayscaleToPalette) != 0;
    const bool applyVertexColors =
        (propFlagsEarly & kFlag_VertexColors) != 0 && !isGrayscaleToPalette;

    // ---- Material fetch (hoisted above the parse 2026-07-09: both the
    // phase-1 path and the cached-retry path below need it) ----
    auto* mat = BsExtraction::GetLightingMaterial(tri);
    if (!mat) {
        if (headDiag) HeadDiagLog(hash, "GATE no lighting material");
        return false;
    }

    // 1B scope: skip landscape (terrain regression accepted; Phase 5 revives).
    if (mat->GetType() == BSLightingShaderMaterialBase::kType_Landscape) {
        ResolverTrace::g_lastStep.store(Trace::kLandscapeSkipped, std::memory_order_relaxed);
        if (headDiag) HeadDiagLog(hash, "GATE landscape material skip");
        return false;
    }

    ResolverTrace::g_lastStep.store(Trace::kMaterialFetched, std::memory_order_relaxed);

    // Shared phase-1 outputs: produced fresh below, or restored from the
    // cache a prior texture-pending attempt left behind.
    ExtractedMesh mesh{};
    uint8_t  albedoLumFloor = 0;
    uint32_t diffuseTint    = 0xFFFFFFu;
    NiTexture* paletteLut   = nullptr;  // grayscale-to-palette per-pixel remap
    float    paletteRowV    = 0.0f;     // final engine LUT row (scale+pow applied)
    bool     paletteBranch  = false;
    uint32_t paletteRowByte = 255;

    // ---- Cached-retry fast path (2026-07-09) ----
    // Ownership moves OUT of the state immediately: if this attempt crashes
    // (SEH catch in Tick) the cache is already gone and the next attempt
    // re-parses cleanly instead of resuming from a moved-from mesh. Pending
    // returns below re-stash it.
    std::shared_ptr<void> cacheHolder = std::move(state.resolveCache);
    ResolveCache* cached = static_cast<ResolveCache*>(cacheHolder.get());
    if (cached) {
        mesh           = std::move(cached->mesh);
        albedoLumFloor = cached->albedoLumFloor;
        diffuseTint    = cached->diffuseTint;
        paletteBranch  = cached->paletteBranch;
        paletteRowByte = cached->paletteRowByte;
        if (paletteBranch) {
            // Re-sample the palette LUT from the cached row byte (the
            // histogram over the engine VB was phase-1 work); the LUT decode
            // itself is cached inside SampleLookupColor after the first hit.
            float scale = mat->fLookupScale;
            if (!(scale >= 0.0f && scale <= 2.0f)) scale = 1.0f;
            float v = scale - 1.0f + std::pow(paletteRowByte / 255.0f, 1.0f / 2.2f);
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            uint32_t pal = 0xFFFFFFu;
            const int st = BsExtraction::SampleLookupColor(
                mat->spLookupTexture, device, /*u=*/0.75f, v, pal);
            if (st == 1) {
                // Same attempt cap as stashPhase1Cache: without it a LUT
                // that never reads back keeps a full vertex copy pinned in
                // the cache for this drawable indefinitely.
                if (state.resolveAttempts <= kResolveCacheMaxAttempts) {
                    cached->mesh = std::move(mesh);
                    state.resolveCache = std::move(cacheHolder);
                }
                if (headDiag) HeadDiagLog(hash, "GATE palette LUT pending (cached)");
                return false;
            }
            if (st == 0) {
                diffuseTint = pal;                   // fallback (BC7 diffuse)
                paletteLut  = mat->spLookupTexture;  // per-pixel engine remap
                paletteRowV = v;
            }
        }
    }
    if (!cached) {
    // ==================================================================
    // PHASE 1: parse + mesh build + tint derivation. Skipped entirely on
    // cached retries (the dominant retry class: textures still in the
    // async readback/decode pipeline). Kept at original indentation to
    // preserve diff/blame history.
    // ==================================================================

    // ---- Parse vertex / index data ----
    ResolverTrace::g_lastStep.store(Trace::kParseStart, std::memory_order_relaxed);
    ParsedGeometry parsed;
    if (!BsExtraction::ParseShapeGeometry(tri, parsed, /*logRejections=*/g_config.logRejections,
                                          applyVertexColors, /*parseSkinning=*/isSkinned)) {
        if (headDiag) HeadDiagLog(hash, "GATE parse FAILED (see [ParseBail])");
        return false;
    }
    if (isSkinned && !parsed.hasSkinning) {
        // Desc says skinned but the skinning attribute didn't fit the
        // stride: without weights the mesh would render as a model-space
        // blob. Skip permanently-ish (retries are cheap and keep the log
        // line from ParseShapeGeometry capped by logRejections).
        if (headDiag) {
            const uint64_t d = tri->vertexDesc;
            const uint32_t shift =
                tri->GetAsBSDynamicTriShape() ? 0 : (uint32_t)((d >> 4) & 0xF);
            const uint32_t oSkin = (shift + (uint32_t)((d >> 28) & 0xF)) * 4;
            HeadDiagLog(hash,
                        "GATE hasSkinning=0: oSkin=%u+12 > stride=%u "
                        "(desc=%016llX) -- dropped every retry",
                        oSkin, (unsigned)tri->GetVertexSize(),
                        (unsigned long long)d);
        }
        return false;
    }

    // [HeadDiag] geometry stats, once per hash (parsed positions include the
    // dynamicVertices decode for facegen shapes; the raw first floats of the
    // dynamic buffer discriminate float3-packed real positions from
    // uninitialized heap or a different element layout, and dynSize vs 12N
    // validates the decompiled creation-path allocation).
    if (headDiag) {
        static std::mutex s_gmx;
        static std::unordered_set<uint64_t> s_geomLogged;
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(s_gmx);
            first = s_geomLogged.insert(hash).second;
        }
        if (first) {
            float maxAbs = 0.0f;
            uint32_t suspect = 0;
            for (const auto& v : parsed.vertices) {
                float m = std::abs(v.position[0]);
                m = (std::max)(m, std::abs(v.position[1]));
                m = (std::max)(m, std::abs(v.position[2]));
                if (m > maxAbs) maxAbs = m;
                if (m > 1000.0f) ++suspect;
            }
            float maxTriple = 0.0f;
            uint32_t maxIdx = 0;
            const size_t nW = parsed.blendWeights.size() / 4;
            for (size_t v = 0; v < nW; ++v) {
                const float t = parsed.blendWeights[v * 4 + 0] +
                                parsed.blendWeights[v * 4 + 1] +
                                parsed.blendWeights[v * 4 + 2];
                if (t > maxTriple) maxTriple = t;
                for (int k = 0; k < 4; ++k)
                    if (parsed.blendIndices[v * 4 + k] > maxIdx)
                        maxIdx = parsed.blendIndices[v * 4 + k];
            }
            char dyn[320] = "";
            if (BSDynamicTriShape* ds = tri->GetAsBSDynamicTriShape()) {
                const uint8_t* db = ds->dynamicVertices;
                const uint32_t nV = tri->numVertices;
                if (db && nV >= 8) {
                    // Raw bytes: the packing verdict (float3 vs half4 vs
                    // half3 vs interleaved) lives in the bit patterns, and
                    // %.2f floats destroy it. head = first 48 bytes; mid8N /
                    // mid6N = 12 bytes at the half4/half3 fill boundaries --
                    // valid coordinates there vs heap garbage discriminates
                    // how much of the 12N allocation the engine actually
                    // wrote.
                    char hexHead[48 * 2 + 1];
                    for (int k = 0; k < 48; ++k)
                        std::snprintf(hexHead + k * 2, 3, "%02X", db[k]);
                    char hex8N[12 * 2 + 1], hex6N[12 * 2 + 1];
                    const uint8_t* p8 = db + (size_t)8 * nV;
                    const uint8_t* p6 = db + (size_t)6 * nV;
                    for (int k = 0; k < 12; ++k) {
                        std::snprintf(hex8N + k * 2, 3, "%02X", p8[k]);
                        std::snprintf(hex6N + k * 2, 3, "%02X", p6[k]);
                    }
                    std::snprintf(dyn, sizeof(dyn),
                                  " dynSize=%u expected12N=%u dynHex0..47=%s "
                                  "at8N=%s at6N=%s",
                                  ds->uiDynamicDataSize, nV * 12u,
                                  hexHead, hex8N, hex6N);
                }
            }
            HeadDiagLog(hash,
                        "geom maxAbsPos=%.1f suspect1e3=%u v0=(%.2f,%.2f,%.2f) "
                        "wMaxTriple=%.3f wMaxIdx=%u%s",
                        maxAbs, suspect,
                        parsed.vertices.empty() ? 0.0f : parsed.vertices[0].position[0],
                        parsed.vertices.empty() ? 0.0f : parsed.vertices[0].position[1],
                        parsed.vertices.empty() ? 0.0f : parsed.vertices[0].position[2],
                        maxTriple, maxIdx, dyn);
        }
    }

    // Reject shapes with garbage extents (defensive guard against malformed input).
    constexpr float kMaxExtent = 1.0e6f;
    for (const auto& v : parsed.vertices) {
        if (std::abs(v.position[0]) > kMaxExtent ||
            std::abs(v.position[1]) > kMaxExtent ||
            std::abs(v.position[2]) > kMaxExtent) {
            ResolverTrace::g_lastStep.store(Trace::kExtentRejected, std::memory_order_relaxed);
            if (headDiag) {
                HeadDiagLog(hash, "GATE extent reject: v=(%.1f,%.1f,%.1f)",
                            v.position[0], v.position[1], v.position[2]);
            }
            return false;
        }
    }

    ResolverTrace::g_lastStep.store(Trace::kParseOK, std::memory_order_relaxed);

    // ---- Build mesh ---- (declared at function scope above; filled here)
    mesh.hash = hash;
    mesh.useVertexColors = applyVertexColors &&
                           (parsed.vertexDesc & BSGeometry::kFlag_VertexColors) != 0;
    constexpr uint64_t kFlag_VertexAlpha = 1ULL << 3;
    constexpr uint64_t kFlag_TreeAnim    = 1ULL << 61;
    mesh.useVertexAlpha = mesh.useVertexColors &&
                          (propFlagsEarly & kFlag_VertexAlpha) != 0 &&
                          (propFlagsEarly & kFlag_TreeAnim) == 0 &&
                          mat->GetType() != BSLightingShaderMaterialBase::kType_TreeAnim;
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

    // ---- Skinned drawable wiring (2026-07-08) ----
    if (isSkinned) {
        uint32_t boneCount = 0;
        const char* skinFail = nullptr;
        if (!SkinnedMeshes::Register(hash, tri, boneCount, &skinFail)) {
            // Skin instance not ready yet (streaming) or failed validation:
            // standard retry-next-tick contract.
            if (headDiag) {
                HeadDiagLog(hash, "GATE SkinnedMeshes::Register: %s",
                            skinFail ? skinFail : "?");
            }
            return false;
        }
        // Facegen bone-source dump (capped inside): the corruption
        // discriminator between garbage face-bone transforms and garbage
        // positions/weights.
        if (headDiag && tri->GetAsBSDynamicTriShape()) {
            SkinnedMeshes::LogBones(hash, tri->m_name.c_str()
                                              ? tri->m_name.c_str() : "");
        }
        // Blend indices reference the skin instance's bone array; clamp any
        // out-of-range index to bone 0 rather than letting the runtime's
        // skinning shader read a garbage matrix slot. Clamps are LOGGED
        // (capped): a nonzero count means the VB indexes a bigger bone table
        // than Register derived -- the clamp visual is vertices spiking
        // toward the skeleton root, so this must be treated as a bug signal,
        // not sanitization noise.
        uint32_t clamped = 0, maxIdx = 0;
        for (auto& idx : parsed.blendIndices) {
            if (idx > maxIdx) maxIdx = idx;
            if (idx >= boneCount) { idx = 0; ++clamped; }
        }
        if (clamped > 0) {
            static std::atomic<int> sClampLogs{0};
            if (sClampLogs.fetch_add(1, std::memory_order_relaxed) < 12) {
                _MESSAGE("FO4RemixPlugin: [Skinning] shape \"%s\" clamped %u/%zu "
                         "blend indices (max=%u boneCount=%u)",
                         tri->m_name.c_str() ? tri->m_name.c_str() : "",
                         clamped, parsed.blendIndices.size(), maxIdx, boneCount);
            }
        }
        // [SkinVtx] weight-sanity scan (2026-07-08 spike hunt). The parse
        // renormalizes triple sums > 1, so seeing over>0 here would mean the
        // renorm didn't cover the real anomaly; the worst-vertex dump gives
        // the raw decode for offline comparison either way.
        {
            const size_t vCount = parsed.blendWeights.size() / 4;
            float maxTriple = 0.0f;
            size_t over = 0, worst = 0;
            for (size_t v = 0; v < vCount; ++v) {
                const float t = parsed.blendWeights[v * 4 + 0] +
                                parsed.blendWeights[v * 4 + 1] +
                                parsed.blendWeights[v * 4 + 2];
                if (t > 1.001f) ++over;
                if (t > maxTriple) { maxTriple = t; worst = v; }
            }
            static std::atomic<int> sVtxLogs{0};
            if (vCount > 0 && sVtxLogs.fetch_add(1, std::memory_order_relaxed) < 16) {
                const float* w = &parsed.blendWeights[worst * 4];
                const uint32_t* bi = &parsed.blendIndices[worst * 4];
                _MESSAGE("FO4RemixPlugin: [SkinVtx] shape=\"%s\" verts=%zu bones=%u "
                         "tripleOver1=%zu maxTriple=%.4f worst=v%zu "
                         "w=(%.3f,%.3f,%.3f,%.3f) idx=(%u,%u,%u,%u)",
                         tri->m_name.c_str() ? tri->m_name.c_str() : "",
                         vCount, boneCount, over, maxTriple, worst,
                         w[0], w[1], w[2], w[3], bi[0], bi[1], bi[2], bi[3]);
            }
        }
        mesh.hasSkinning  = true;
        mesh.boneCount    = boneCount;
        mesh.blendWeights = std::move(parsed.blendWeights);
        mesh.blendIndices = std::move(parsed.blendIndices);
        // Skinned-key side index (Tick's live app-culled refresh + OnFrame's
        // hidden-geometry skip -- hair-under-hats, 2026-07-08).
        state.isSkinnedActor = true;
        // [FaceAnim] expressions probe: track the first facegen head's bone
        // motion (heads carry ~10 bones; eyes/mouths only 1).
        if (headDiag && tri->GetAsBSDynamicTriShape() && boneCount >= 8) {
            SkinnedMeshes::SetFaceProbe(hash);
        }
        // Instance transform = bare Beth->Remix mirror P. The bone matrices
        // (queued per Tick by SkinnedMeshes::UpdateAndQueue) take bind-pose
        // model space -> Beth WORLD space, so the instance carries only the
        // mirror -- which is also what makes the runtime's facing flip fire
        // (isObjectToWorldMirrored, same mechanism as BatchedMirrorBase).
        static const float kMirrorP[3][4] = {
            { 0.0f, 1.0f, 0.0f, 0.0f },
            { 1.0f, 0.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f, 0.0f },
        };
        std::memcpy(mesh.worldTransform, kMirrorP, sizeof(kMirrorP));
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

    // (Material fetch + landscape gate hoisted above the parse, 2026-07-09 --
    // the cached-retry path needs the material before phase 1 would run.)

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
    // (albedoLumFloor is declared at function scope for the retry cache.)
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

    // ---- [WindDiag] inside-out investigation (2026-07-08) ----
    // Gated on Diagnostics.Enabled: the parity pass inside walks every
    // triangle of the first N shapes, all during the heaviest cell load
    // (and the inside-out root cause was found -- b112e08).
    if (g_config.diagEnabled) {
        LogWindingDiag(tri, mesh, parsed, mat, propFlagsEarly, state);
    }

    // ---- Skin / hair tint (2026-07-08 broken-NPC-colors fix) ----
    // FO4 authors unpigmented/grayscale diffuse maps for tinted body parts and
    // multiplies the real color in at draw time; without it NPCs render pale
    // with gray hair. Two DISTINCT color sources, decompiler-proven
    // (scripts/dynamic_trishape_desc.md):
    //   SKIN -- BSLightingShaderMaterialSkinTint (bodies, hands, rear head):
    //     kTintColor NiColorA at material+0xC0, stored LINEAR. The engine
    //     multiplies it in linear shader space after the sRGB sample decode,
    //     so baking it into the (sRGB-sampled) diffuse needs gamma
    //     compensation t^(1/2.2): srgb_decode(diffuse * t^(1/2.2)) ==
    //     diffuse_linear * t. (User-verified skin colors on this path.)
    //   HAIR -- NPC hair is a BSLightingShaderMaterialGlowmap (type 2) with
    //     the Hair shader flag; the per-NPC hue is NOT in the material, it is
    //     the pEmissiveColor on the PROPERTY (+0xB8, NiColor*) times
    //     fEmitColorScale (+0xC8). Render color = pow(pEmissiveColor*scale,
    //     2.2), i.e. the constant is stored GAMMA -- so baking it into the
    //     sRGB diffuse is a DIRECT multiply, no exponent:
    //     srgb_decode(diffuse * (c*scale)) == diffuse_linear *
    //     pow(c*scale,2.2). (This is why the "teal hair" needed both fixes:
    //     the emissive path is skipped for hair in ExtractEmissiveData, and
    //     the color moves here as a diffuse tint. The shared HairColor_LGrad
    //     LUT's tonal shaping is not yet replicated -- a flat multiply is the
    //     first approximation.)
    // Classes/hair identified by RTTI leaf + the Hair flag, NOT GetType()
    // (live logs showed hair as type 2 where the F4SE enum says HairTint=6).
    // (diffuseTint / paletteLut / paletteRowV are declared at function scope
    // for the retry cache.)
    {
        char matLeaf[64] = "";
        SemanticCapture::GetLeafClassName(mat, matLeaf, sizeof(matLeaf));
        const bool isSkinTint =
            std::strcmp(matLeaf, "BSLightingShaderMaterialSkinTint") == 0;
        const bool isHairTintMat =
            std::strcmp(matLeaf, "BSLightingShaderMaterialHairTint") == 0;
        // Hair discriminator: the shared HairColor gradient LUT bound in
        // spLookupTexture. The F4SE kShaderFlags_Hair (bit 46) is WRONG for
        // this build -- it matched landscape terrain and never real hair
        // (2026-07-08 run: 24/24 "hair" tint hits were BSLightingShaderMaterial-
        // Landscape, zero actual hair), the same enum drift that makes hair
        // report GetType()==2. The LUT name is build-independent: exactly one
        // "HairColor_LGrad" texture exists game-wide and only hair binds it.
        const NiTexture* lut = mat->spLookupTexture;
        const char* lutName = (lut && lut->name.c_str()) ? lut->name.c_str() : nullptr;
        const bool isHair = NameContainsCI(lutName, "haircolor");

        if (isSkinTint || isHairTintMat) {
            // Material kTintColor, linear-stored -> gamma-compensated bake.
            const float* tc = reinterpret_cast<const float*>(
                reinterpret_cast<uintptr_t>(mat) + 0xC0);
            bool sane = true;
            for (int c = 0; c < 3; ++c)
                if (!(tc[c] >= 0.0f && tc[c] <= 1.0f)) sane = false;
            if (sane) {
                uint32_t packed = 0;
                for (int c = 0; c < 3; ++c) {
                    const float g = std::pow(tc[c], 1.0f / 2.2f);
                    packed = (packed << 8) | (uint32_t)(g * 255.0f + 0.5f);
                }
                diffuseTint = packed;
            }
            static std::atomic<int> sTintLogs{0};
            if (sTintLogs.fetch_add(1, std::memory_order_relaxed) < 24) {
                _MESSAGE("FO4RemixPlugin: [Tint] skin shape=\"%s\" mat=%s "
                         "kTintColor=(%.3f,%.3f,%.3f) -> %06X%s",
                         tri->m_name.c_str() ? tri->m_name.c_str() : "",
                         matLeaf, tc[0], tc[1], tc[2], diffuseTint,
                         sane ? "" : " (implausible -- ignored)");
            }
        } else if (isHair && state.property) {
            // Property pEmissiveColor * fEmitColorScale, gamma-stored ->
            // direct multiply (no exponent).
            const uintptr_t pp = reinterpret_cast<uintptr_t>(state.property);
            const float* pEmis = *reinterpret_cast<float**>(pp + 0xB8);
            const float scale = *reinterpret_cast<float*>(pp + 0xC8);
            float r = 1.0f, g = 1.0f, b = 1.0f;
            bool sane = false;
            if (pEmis) {
                const float s = (scale >= 0.0f && scale <= 8.0f) ? scale : 1.0f;
                r = pEmis[0] * s; g = pEmis[1] * s; b = pEmis[2] * s;
                sane = (r == r && g == g && b == b);  // reject NaN
                auto clamp01 = [](float v) {
                    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                };
                r = clamp01(r); g = clamp01(g); b = clamp01(b);
            }
            if (sane) {
                diffuseTint = ((uint32_t)(r * 255.0f + 0.5f) << 16) |
                              ((uint32_t)(g * 255.0f + 0.5f) << 8) |
                               (uint32_t)(b * 255.0f + 0.5f);
            }
            static std::atomic<int> sHairTintLogs{0};
            if (sHairTintLogs.fetch_add(1, std::memory_order_relaxed) < 24) {
                _MESSAGE("FO4RemixPlugin: [Tint] hair shape=\"%s\" mat=%s "
                         "pEmis=%p emis=(%.3f,%.3f,%.3f) scale=%.3f -> %06X%s",
                         tri->m_name.c_str() ? tri->m_name.c_str() : "", matLeaf,
                         (void*)pEmis, pEmis ? pEmis[0] : 0.0f,
                         pEmis ? pEmis[1] : 0.0f, pEmis ? pEmis[2] : 0.0f, scale,
                         diffuseTint, sane ? "" : " (no color -- untinted)");
            }
        } else if (isGrayscaleToPalette) {
            // General grayscale-to-palette (clothing, clutter, painted wood,
            // vehicles, painted power armor): the diffuse is grayscale and
            // the engine REPLACES the albedo with a palette-LUT fetch.
            // Decompiled palette PS (blob @0x7b7e68 in Shaders011.fxp,
            // re-disassembled 2026-07-09):
            //   albedo.rgb = LUT(u = pow(diffuse.g, 1/2.2),
            //                    v = GrayscaleToPaletteScale - 1
            //                        + pow(vertexColor.x, 1/2.2))
            // The lighting VS passes COLOR0 through raw (851-VS scan of the
            // package: plain mov, or a compiled-out pow(c,1) log/exp pair),
            // so vc.x is the raw UNORM byte. The old approximation ignored
            // the pow AND the per-material scale AND used the row MEAN --
            // wrong palette row for anything whose scale isn't the row's
            // fixed point (cyan cars, sage "white" picket fences, red T-60
            // hot-rod paint), and the mean of two regions' rows lands
            // BETWEEN palette rows producing colors that exist in no row
            // (bright-purple painted-PA torso).
            //   row: per-mesh MODE of the red byte = dominant palette row
            //     (per-vertex rows can't vary inside one Remix texture).
            //   pixels: ExtractMaterialTexture remaps the diffuse through
            //     the LUT row per-pixel (paletteLut/paletteRowV below) -- a
            //     flat tint can't reproduce the ramp's rust->paint hue
            //     shift. The u=0.75 sample here is only the fallback tint
            //     for undecodable (BC7) diffuses.
            uint32_t rowByte = 255, rowMin = 255, rowMax = 0;
            const bool hasColorStream = (parsed.vertexDesc & (1ULL << 49)) != 0;
            if (hasColorStream && parsed.vbData && parsed.vertexSize > 0) {
                const uint64_t d = parsed.vertexDesc;
                const uint32_t szV = (uint32_t)((d >> 4) & 0xF);
                const uint32_t shift = parsed.isDynamic ? 0u : szV;
                const uint32_t oColor = (shift + (uint32_t)((d >> 24) & 0xF)) * 4;
                if (oColor + 4 <= parsed.vertexSize) {
                    uint32_t histo[256] = {};
                    const size_t nV = tri->numVertices;
                    for (size_t i = 0; i < nV; ++i) {
                        const uint8_t r = parsed.vbData[i * parsed.vertexSize + oColor];
                        ++histo[r];
                        if (r < rowMin) rowMin = r;
                        if (r > rowMax) rowMax = r;
                    }
                    uint32_t best = 0;
                    for (uint32_t b = 0; b < 256; ++b)
                        if (histo[b] > best) { best = histo[b]; rowByte = (uint8_t)b; }
                }
            }
            // GrayscaleToPaletteScale = fLookupScale (material+0xB8, F4SE
            // NiMaterials.h). The BGSM survey proves it's the load-bearing
            // row selector: every color variant of a shared LUT differs
            // ONLY by this scale (WoodSidingBlue02=0.65, Pink=0.60,
            // GreenLt=0.45 -> the sage row our fences wrongly got, Tan=0.35,
            // White=1.0). 0.0 is authored (T45body01) -- allow it; NaN or
            // out-of-range reads (layout drift) fall back to 1.
            // Record for the retry cache: the cached path re-derives v from
            // this byte + the live material scale instead of re-walking the VB.
            paletteBranch  = true;
            paletteRowByte = rowByte;
            float scale = mat->fLookupScale;
            if (!(scale >= 0.0f && scale <= 2.0f)) scale = 1.0f;
            float v = scale - 1.0f + std::pow(rowByte / 255.0f, 1.0f / 2.2f);
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            uint32_t pal = 0xFFFFFFu;
            const int st = BsExtraction::SampleLookupColor(
                mat->spLookupTexture, device, /*u=*/0.75f, v, pal);
            if (st == 1) {
                // LUT not read back yet -- retry like any pending texture.
                // Stash phase-1 so the retry skips the parse (mesh is fully
                // built at this point; diffuseTint is still the pre-palette
                // 0xFFFFFF because the tint branches are exclusive).
                if (state.resolveAttempts <= kResolveCacheMaxAttempts) {
                    auto rc = std::make_shared<ResolveCache>();
                    rc->mesh           = std::move(mesh);
                    rc->albedoLumFloor = albedoLumFloor;
                    rc->diffuseTint    = 0xFFFFFFu;
                    rc->paletteBranch  = true;
                    rc->paletteRowByte = rowByte;
                    state.resolveCache = std::move(rc);
                }
                if (headDiag) HeadDiagLog(hash, "GATE palette LUT pending");
                return false;
            }
            if (st == 0) {
                diffuseTint = pal;                   // fallback (BC7 diffuse)
                paletteLut  = mat->spLookupTexture;  // per-pixel engine remap
                paletteRowV = v;
            }
            static std::atomic<int> sGtpLogs{0};
            if (sGtpLogs.fetch_add(1, std::memory_order_relaxed) < 24) {
                const NiTexture* l = mat->spLookupTexture;
                _MESSAGE("FO4RemixPlugin: [Tint] palette shape=\"%s\" mat=%s "
                         "rowByte=%u (spread %u..%u) scale=%.3f v=%.3f "
                         "lut=\"%s\" st=%d fallback=%06X",
                         tri->m_name.c_str() ? tri->m_name.c_str() : "", matLeaf,
                         rowByte, rowMin, rowMax, scale, v,
                         (l && l->name.c_str()) ? l->name.c_str() : "",
                         st, diffuseTint);
            }
        }
    }

    // ---- [DetailDiag] over-dirty clothing/buildings (2026-07-08) ----
    // NPC outfits and world objects render far grayer/dirtier than vanilla
    // (systemic across NPCs + statics). Leading suspect: the per-vertex COLOR
    // stream is being multiplied into the albedo when vanilla would not (or
    // would apply it gentler) -- and the enum drift that just broke the Hair
    // flag makes our SLSF2_Vertex_Colors bit (37) suspect. Log, once per hash
    // for shapes that CARRY a color stream: propFlags, whether we applied the
    // colors, the raw vertex-color mean/min (dark colors = the "dirt"), the
    // material class/type, and the diffuse path -- so the applied-vs-authored
    // and flag-vs-darkness correlation can be read off one run.
    // (Moved into phase 1 2026-07-09: it reads `parsed`, which no longer
    // exists on the cached-retry path. Now logs at first parse rather than
    // first successful diffuse extraction -- same once-per-hash cap.)
    {
        const uint64_t d = parsed.vertexDesc;
        const bool hasColorStream = (d & (1ULL << 49)) != 0;
        if (g_config.diagEnabled &&
            hasColorStream && parsed.vbData && parsed.vertexSize > 0) {
            const uint32_t szV = (uint32_t)((d >> 4) & 0xF);
            const uint32_t shift = parsed.isDynamic ? 0u : szV;
            const uint32_t oColor = (shift + (uint32_t)((d >> 24) & 0xF)) * 4;
            if (oColor + 4 <= parsed.vertexSize) {
                static std::mutex s_dmx;
                static std::unordered_set<uint64_t> s_dseen;
                bool first = false;
                {
                    std::lock_guard<std::mutex> lk(s_dmx);
                    static int s_dlogs = 0;
                    if (s_dlogs < 48 && s_dseen.insert(hash).second) {
                        ++s_dlogs; first = true;
                    }
                }
                if (first) {
                    // parsed.vertices was std::move'd into mesh.vertices
                    // above -- use the shape's own count and read raw colors
                    // straight from the (still-valid) engine VB pointer.
                    const size_t nV = tri->numVertices;
                    uint32_t sr = 0, sg = 0, sb = 0, sa = 0;
                    uint8_t mnr = 255, mng = 255, mnb = 255;
                    for (size_t i = 0; i < nV; ++i) {
                        const uint8_t* c = parsed.vbData + i * parsed.vertexSize + oColor;
                        sr += c[0]; sg += c[1]; sb += c[2]; sa += c[3];
                        if (c[0] < mnr) mnr = c[0];
                        if (c[1] < mng) mng = c[1];
                        if (c[2] < mnb) mnb = c[2];
                    }
                    const size_t n = nV ? nV : 1;
                    char matLeaf[64] = "?";
                    SemanticCapture::GetLeafClassName(mat, matLeaf, sizeof(matLeaf));
                    const char* dn = (mat->spDiffuseTexture && mat->spDiffuseTexture->name.c_str())
                        ? mat->spDiffuseTexture->name.c_str() : "";
                    _MESSAGE("FO4RemixPlugin: [DetailDiag] \"%s\" mat=%s type=%u "
                             "propFlags=%016llX applyVtxCol=%d nV=%zu "
                             "vcMean=(%u,%u,%u,a%u) vcMin=(%u,%u,%u) diffuse=\"%s\"",
                             tri->m_name.c_str() ? tri->m_name.c_str() : "",
                             matLeaf, (unsigned)mat->GetType(),
                             (unsigned long long)propFlagsEarly,
                             applyVertexColors ? 1 : 0, nV,
                             (unsigned)(sr / n), (unsigned)(sg / n),
                             (unsigned)(sb / n), (unsigned)(sa / n),
                             mnr, mng, mnb, dn);
                }
            }
        }
    }

    // ==================================================================
    // end PHASE 1
    // ==================================================================
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
    // (2026-07-06 black-merge investigation: raw BC1/BC3-SRGB merge uploads
    // were briefly suspected and force-decompressed to RGBA8 -- the visual
    // discriminator run showed black blobs SURVIVING the conversion, so BC
    // upload is exonerated and the conversion is reverted. The real cause
    // was the whole-mesh x all-records fallback; see the single-record
    // fallback below.)
    const TexturePostProcess diffusePostProcess =
        (mesh.alphaTestEnabled || mesh.alphaBlendEnabled)
            ? TexturePostProcess::DiffuseAlphaFromLuminance
            : TexturePostProcess::None;

    // Stash phase-1 output for a retry (the parse is the expensive part of
    // an attempt; textures normally land within a few ticks of async
    // readback + worker decode). Dropped once the backoff goes exponential
    // so a never-resolving drawable doesn't pin its vertex copy in memory.
    auto stashPhase1Cache = [&]() {
        if (state.resolveAttempts > kResolveCacheMaxAttempts) return;
        if (cached) {
            cached->mesh = std::move(mesh);
            state.resolveCache = std::move(cacheHolder);
        } else {
            auto rc = std::make_shared<ResolveCache>();
            rc->mesh           = std::move(mesh);
            rc->albedoLumFloor = albedoLumFloor;
            rc->diffuseTint    = paletteBranch ? 0xFFFFFFu : diffuseTint;
            rc->paletteBranch  = paletteBranch;
            rc->paletteRowByte = paletteRowByte;
            state.resolveCache = std::move(rc);
        }
    };

    // ---- Texture probe pass (2026-07-09) ----
    // Fill the hash fields and start any not-yet-started readback/decode,
    // but copy NO pixel buffers (supplyPixels=false). Since decode moved to
    // worker threads the four slots complete at DIFFERENT ticks, and the old
    // single consuming pass had two measured failure modes:
    //   - diffuse finishing LAST: every 1-frame retry re-copied the already
    //     decoded sibling chains (~22MB each) into newTextures and threw
    //     them away at the diffuse gate -- multi-ms memcpy per retry that
    //     crowded real submissions out of the ResolveBudget ("slower
    //     pop-in", 2026-07-09);
    //   - diffuse finishing FIRST: the drawable submitted immediately and
    //     PERMANENTLY without its still-decoding normal/roughness/emissive
    //     (a 0 hash was indistinguishable from "slot doesn't exist").
    // Any slot still pending -> cheap retry, no copies. The supplying pass
    // below runs only on the attempt that submits.
    bool pendDiffuse = false, pendNormal = false, pendRough = false,
         pendEmissive = false;
    mesh.diffuseTextureHash = BsExtraction::ExtractMaterialTexture(
        mat->spDiffuseTexture, "diffuse", device, newTextures, diffusePostProcess,
        /*minRoughness=*/0, albedoLumFloor, diffuseTint, paletteLut, paletteRowV,
        &pendDiffuse, /*supplyPixels=*/false);
    mesh.normalTextureHash = BsExtraction::ExtractMaterialTexture(
        mat->spNormalTexture, "normal", device, newTextures, TexturePostProcess::Octahedral,
        /*minRoughness=*/0, /*albedoLumFloor=*/0, /*tintRGB=*/0xFFFFFFu,
        /*paletteLut=*/nullptr, /*paletteRowV=*/0.0f,
        &pendNormal, /*supplyPixels=*/false);
    // Smoothness/spec-mask (_s.dds) -> per-pixel roughness, RESTORED
    // 2026-07-07 (removed 2026-07-02 in 74c28b9). The removal fell back to
    // roughnessConstant=0.8, and the [Metal] roughness path derived its
    // constant from the material's fSmoothness SCALAR -- which FO4 authors
    // as a multiplier over the _s map: it reads 1.00 on every sampled
    // material, so 1-fSmoothness clamped at the floor turned the whole
    // envmap class into 0.15-rough chrome (user report: vault interiors
    // "ultra shiny", floor a literal mirror). The real per-pixel smoothness
    // lives in _s.dds G (BC5: R=spec mask, G=smoothness; the InvertRGB
    // decode path inverts G into roughness). The two 07-02 removal blockers
    // are resolved since: mirror DECALS get the >= 0.3 floor below, and the
    // "metal black voids" were the pre-luminance-floor albedo problem (the
    // floor ships on, and interiors now have real lights).
    uint8_t roughnessFloor = 0;
    if (g_config.roughnessMapsEnabled) {
        const uint8_t cfgFloor = (uint8_t)(std::clamp(
            g_config.roughnessMapFloor, 0.0f, 1.0f) * 255.0f + 0.5f);
        roughnessFloor =
            mesh.isDecal ? (std::max)(cfgFloor, (uint8_t)76) : cfgFloor;
        mesh.roughnessTextureHash = BsExtraction::ExtractMaterialTexture(
            mat->spSmoothnessSpecMaskTexture, "roughness", device, newTextures,
            TexturePostProcess::InvertRGB, roughnessFloor,
            /*albedoLumFloor=*/0, /*tintRGB=*/0xFFFFFFu,
            /*paletteLut=*/nullptr, /*paletteRowV=*/0.0f,
            &pendRough, /*supplyPixels=*/false);
        // Per-pixel roughness supersedes any scalar-derived constant: the
        // constant only exists as the fallback for materials with no _s map.
        if (mesh.roughnessTextureHash != 0) {
            mesh.roughnessConstantOverride = -1.0f;
        }
    }
    BsExtraction::ExtractEmissiveData(tri, mat, device, newTextures,
                                      mesh.emissiveTextureHash,
                                      mesh.emissiveColorR, mesh.emissiveColorG, mesh.emissiveColorB,
                                      mesh.emissiveIntensity,
                                      &pendEmissive, /*supplyPixels=*/false);

    // Any slot still in the async pipeline: retry next tick. Cheap -- the
    // probe made no copies, and the phase-1 stash skips the re-parse.
    if (pendDiffuse || pendNormal || pendRough || pendEmissive) {
        stashPhase1Cache();
        if (headDiag) {
            HeadDiagLog(hash, "GATE texturesPending d=%d n=%d r=%d e=%d -- retry",
                        pendDiffuse ? 1 : 0, pendNormal ? 1 : 0,
                        pendRough ? 1 : 0, pendEmissive ? 1 : 0);
        }
        return false;
    }

    // No diffuse (and not pending) -> can't render lit; retry via backoff in
    // case the engine backs the resource later.
    if (mesh.diffuseTextureHash == 0) {
        stashPhase1Cache();
        if (headDiag) {
            NiTexture* dt = mat->spDiffuseTexture;
            HeadDiagLog(hash,
                        "GATE noDiffuse: matType=%u tex=%p name=\"%s\" -- retry",
                        (unsigned)mat->GetType(), (void*)dt,
                        (dt && dt->name.c_str()) ? dt->name.c_str() : "");
        }
        return false;
    }

    // ---- Texture supply pass ----
    // Everything is decoded; re-run the extractions with supplyPixels=true so
    // any texture whose Remix-side handle is missing gets its pixels copied
    // into newTextures -- exactly once, on this submitting attempt. Pure
    // cache hits: no readback, no decode, identical hashes.
    mesh.diffuseTextureHash = BsExtraction::ExtractMaterialTexture(
        mat->spDiffuseTexture, "diffuse", device, newTextures, diffusePostProcess,
        /*minRoughness=*/0, albedoLumFloor, diffuseTint, paletteLut, paletteRowV);
    mesh.normalTextureHash = BsExtraction::ExtractMaterialTexture(
        mat->spNormalTexture, "normal", device, newTextures, TexturePostProcess::Octahedral);
    if (g_config.roughnessMapsEnabled) {
        mesh.roughnessTextureHash = BsExtraction::ExtractMaterialTexture(
            mat->spSmoothnessSpecMaskTexture, "roughness", device, newTextures,
            TexturePostProcess::InvertRGB, roughnessFloor);
        if (mesh.roughnessTextureHash != 0) {
            mesh.roughnessConstantOverride = -1.0f;
        }
    }
    BsExtraction::ExtractEmissiveData(tri, mat, device, newTextures,
                                      mesh.emissiveTextureHash,
                                      mesh.emissiveColorR, mesh.emissiveColorG, mesh.emissiveColorB,
                                      mesh.emissiveIntensity);

    // Textures resolved: the phase-1 cache (if any) is consumed by this
    // attempt; cacheHolder frees it at return.

    ResolverTrace::g_lastStep.store(Trace::kTexturesExtracted, std::memory_order_relaxed);

    // Record the resident diffuse resolution this submit captured at, so the
    // Tick's re-capture-on-approach poll can detect when the engine later
    // streams a sharper mip in and re-resolve for the full-res texture (see
    // DrawableState::submittedDiffuseWidth and GetMaterialDiffuseResidentWidth).
    // Must match what ExtractMaterialTexture folded into the diffuse hash --
    // both read the live diffuse resource width, so a later strictly-larger
    // live width is a genuine streamed upgrade. Opt-in; left 0 when off so the
    // Tick poll skips this drawable (and the per-submit GetDesc is avoided).
    if (g_config.textureUpgradeOnApproach) {
        state.submittedDiffuseWidth = (uint16_t)(std::min)(
            BsExtraction::GetMaterialDiffuseResidentWidth(mat), 0xFFFFu);
    }

    // ([DetailDiag] moved into phase 1, 2026-07-09 -- it reads `parsed`,
    // which does not exist on the cached-retry path.)

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
    // Take-13 exact partition from the engine's CPU-resident u16 table
    // (filled by the [MergeT7] block below when every invariant holds).
    // t7Table[group] = record index for that GS-triangle group; the submit
    // section expands it into one submesh per record.
    std::vector<uint16_t> t7Table;
    uint32_t t7GS = 0;
    bool t7Valid = false;
    if (g_config.mergeInstanceExpansion && g_config.gpuInstancingEnabled) {
        // Classify once per drawable (state.mergeLeafKind caches the RTTI
        // walk + substring scan; retrying drawables used to re-pay it every
        // attempt).
        if (state.mergeLeafKind < 0) {
            char instLeaf[64] = "";
            SemanticCapture::GetLeafClassName(reinterpret_cast<void*>(tri),
                                              instLeaf, sizeof(instLeaf));
            state.mergeLeafKind =
                (std::strstr(instLeaf, "MergeInstanced") != nullptr) ? 1 : 0;
        }
        if (state.mergeLeafKind == 1) {
            const bool got = ReadMergeInstanceRecords(tri, instRecords,
                                                      &instBufPtr, &instSrvPtr);
            if (got) {
                uint64_t segQ[2] = {};
                if (PeekQwordsGuarded(reinterpret_cast<const void*>(
                        reinterpret_cast<uintptr_t>(tri) + 0x1A0), segQ, 2)) {
                    std::memcpy(instSegTris, segQ, sizeof(instSegTris));
                }
                // The segment table is EXACTLY 3 dwords -- decompiler-proven
                // (2026-07-06 static-RE pass, scripts/merge_partition_
                // research.md): the bake loop at 0x1421E27D0 is literally
                // `while (seg < 3)` writing +0x1A0/+0x1A4/+0x1A8, and +0x1AC
                // is never touched by ctor, bake, clone, or draw -- pure
                // heap junk. (A brief "keep 4 slots when they sum" variant
                // is reverted: any such sum is coincidence.) Segments are
                // up to 3 DISTANCE-DETAIL BANDS of the cluster; near the
                // player the engine draws all of them.
                instSegTris[3] = 0;

                // ---- Take 13: exact partition from the engine's own u16
                // group->record table (2026-07-06, decompiler-proven).
                // The engine never draws these instanced and keeps no
                // {recordStart,recordCount} table; its vertex shader maps
                // each triangle GROUP (GS consecutive triangles, GS at
                // shape+0x194) to a record index via a u16 table bound at
                // t7 -- and that table's CPU copy is RETAINED on the shape
                // (+0x178 -> wrapper+0x8). Per band s:
                //   recordStart(s) = T[prefix(s)/GS]
                //   recordCount(s) = T[lastGroup(s)] - T[firstGroup(s)] + 1
                // This needs no draw capture and works for off-screen
                // clusters. All reads guarded; every invariant from the
                // research doc gates acceptance -- any violation falls back
                // to the existing capture -> take-5 -> single-record chain.
                // (t7RecStart/t7RecCount/t7NSegs/t7Valid declared at
                // function scope beside instSegTris.)
                {
                    const uint32_t mTris = (uint32_t)(mesh.indices.size() / 3);
                    const char* rej = nullptr;
                    uint64_t strideGs = 0;   // +0x190 stride | +0x194 GS
                    uint64_t w178q = 0, w170q = 0;
                    uint32_t GS = 0, nGroups = 0;
                    std::vector<uint16_t> T;
                    uint32_t recBufCount = 0;
                    // SRV-slice diagnostics for the [MergeT7] line (zeros
                    // when the CPU fast path was taken).
                    uint32_t tFmt = 0, tFE = 0, tNE = 0, tUsed = 0;
                    bool t7Defer = false;
                    do {
                        if (!PeekQwordsGuarded(reinterpret_cast<const void*>(
                                reinterpret_cast<uintptr_t>(tri) + 0x190),
                                &strideGs, 1)) { rej = "peek+0x190"; break; }
                        GS = (uint32_t)(strideGs >> 32);
                        // Upper bound 1024, not 128: the 2026-07-07 live run
                        // shows real GS=256 shapes (vault interiors); the
                        // decompiled derivation only proves pow2 >= 16.
                        if (GS < 16 || GS > 1024 || (GS & (GS - 1)) != 0) {
                            rej = "GS"; break;
                        }
                        uint64_t sumSeg = (uint64_t)instSegTris[0] +
                                          instSegTris[1] + instSegTris[2];
                        if (mTris == 0 || sumSeg != mTris ||
                            (mTris % GS) != 0) { rej = "segSum"; break; }
                        bool segMod = false;
                        for (int s = 0; s < 3; ++s) {
                            if (instSegTris[s] % GS) segMod = true;
                        }
                        if (segMod) { rej = "segMod"; break; }
                        nGroups = mTris / GS;
                        if (!PeekQwordsGuarded(reinterpret_cast<const void*>(
                                reinterpret_cast<uintptr_t>(tri) + 0x178),
                                &w178q, 1) || !w178q) { rej = "w178"; break; }
                        if (!PeekQwordsGuarded(reinterpret_cast<const void*>(
                                reinterpret_cast<uintptr_t>(tri) + 0x170),
                                &w170q, 1) || !w170q) { rej = "w170"; break; }
                        // Table acquisition, two paths:
                        // (a) CPU fast path -- valid when the wrapper OWNS a
                        //     retained CPU copy (bake-time shapes; the owned
                        //     flag at +0x4E was decompiler-proven for the
                        //     CreateDataBuffer bake path).
                        // (b) GPU staging readback of the shape's slice via
                        //     the SRV at wrapper+0x18 -- live sessions show
                        //     BA2-LOADED shapes never set the owned flag
                        //     (60/60 REJECT:owns on 2026-07-07), so shipped
                        //     precombines take this path. In-flight uploads
                        //     DEFER via the pending counter at +0x44 (doc
                        //     invariant 7) instead of rejecting.
                        bool haveT = false;
                        uint8_t owns = 0;
                        if (PeekBytesGuarded(reinterpret_cast<const void*>(
                                (uintptr_t)w178q + 0x4E), &owns, 1) &&
                            owns == 1) {
                            uint32_t usedBytes = 0;
                            uint64_t tPtr = 0;
                            const uint32_t expBytes = (2u * nGroups + 3u) & ~3u;
                            if (PeekBytesGuarded(reinterpret_cast<const void*>(
                                    (uintptr_t)w178q + 0x34), &usedBytes, 4) &&
                                usedBytes == expBytes &&
                                PeekQwordsGuarded(reinterpret_cast<const void*>(
                                    (uintptr_t)w178q + 0x8), &tPtr, 1) &&
                                tPtr && HeapLikePtr(tPtr)) {
                                tUsed = usedBytes;
                                T.resize(nGroups);
                                haveT = PeekBytesGuarded(
                                    reinterpret_cast<const void*>((uintptr_t)tPtr),
                                    T.data(), nGroups * 2);
                            }
                        }
                        if (!haveT) {
                            // The t7 table lives in a SHARED pool buffer --
                            // reading the ID3D11Buffer from byte 0 returned
                            // the identical pool head for every shape
                            // (half-float vertex data). The shape's slice is
                            // encoded in its SRV (wrapper+0x18): query the
                            // view, take the resource FROM it, read exactly
                            // that slice.
                            //
                            // 2026-07-07 run 1 pinned the slice layout: the
                            // REJECT population split exactly at the 64-
                            // element boundary (numElem is DWORD-granular
                            // over a 256-byte-rounded pool slice, so an
                            // element-vs-group compare rejects every table
                            // over half the granule), and the u32 parse
                            // tripped its range check precisely where a
                            // packed u16 pair first gets a nonzero high
                            // half. The view is dword-shaped; the DATA is
                            // packed u16 -- capacity is checked in BYTES and
                            // the slice is parsed as u16 for every format.
                            uint32_t tPend = 0;
                            if (PeekBytesGuarded(reinterpret_cast<const void*>(
                                    (uintptr_t)w178q + 0x44), &tPend, 4) &&
                                tPend != 0) {
                                // Async upload in flight: the pool bytes are
                                // not the table yet. Defer the resolve (same
                                // contract as a not-yet-ready texture) -- a
                                // reject here would park the shape on the
                                // single-record fallback for the lifetime of
                                // the cell.
                                t7Defer = true; rej = "tPending"; break;
                            }
                            PeekBytesGuarded(reinterpret_cast<const void*>(
                                (uintptr_t)w178q + 0x34), &tUsed, 4);
                            uint64_t tSrvObj = 0, tVtbl = 0;
                            if (!PeekQwordsGuarded(reinterpret_cast<const void*>(
                                    (uintptr_t)w178q + 0x18), &tSrvObj, 1) ||
                                !HeapLikePtr(tSrvObj) ||
                                !PeekQwordsGuarded(reinterpret_cast<const void*>(
                                    (uintptr_t)tSrvObj), &tVtbl, 1) ||
                                !PtrInD3D11(tVtbl)) { rej = "tSrv"; break; }
                            void* rawV = nullptr;
                            if (!QiGuarded(reinterpret_cast<void*>(tSrvObj),
                                           &__uuidof(ID3D11ShaderResourceView),
                                           &rawV)) { rej = "tSrvQi"; break; }
                            ID3D11ShaderResourceView* tSrv =
                                static_cast<ID3D11ShaderResourceView*>(rawV);
                            D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
                            tSrv->GetDesc(&svd);
                            tFmt = (uint32_t)svd.Format;
                            if (svd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER) {
                                tFE = svd.Buffer.FirstElement;
                                tNE = svd.Buffer.NumElements;
                            } else if (svd.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) {
                                tFE = svd.BufferEx.FirstElement;
                                tNE = svd.BufferEx.NumElements;
                            } else {
                                tSrv->Release();
                                rej = "tSrvDim"; break;
                            }
                            ID3D11Resource* tRes = nullptr;
                            tSrv->GetResource(&tRes);
                            tSrv->Release();
                            if (!tRes) { rej = "tRes"; break; }
                            void* rawB = nullptr;
                            const bool qiOk = SUCCEEDED(tRes->QueryInterface(
                                __uuidof(ID3D11Buffer), &rawB)) && rawB;
                            tRes->Release();
                            if (!qiOk) { rej = "tResQi"; break; }
                            ID3D11Buffer* tBuf = static_cast<ID3D11Buffer*>(rawB);
                            uint32_t elemSize = 0;
                            switch (svd.Format) {
                            case DXGI_FORMAT_R16_UINT:
                            case DXGI_FORMAT_R16_TYPELESS: elemSize = 2; break;
                            case DXGI_FORMAT_R32_UINT:
                            case DXGI_FORMAT_R32_TYPELESS:
                            case DXGI_FORMAT_R16G16_UINT:
                            case DXGI_FORMAT_R16G16_TYPELESS: elemSize = 4; break;
                            case DXGI_FORMAT_UNKNOWN: {
                                // Structured view: element size lives on the
                                // buffer desc, not the view format.
                                D3D11_BUFFER_DESC bd = {};
                                tBuf->GetDesc(&bd);
                                elemSize = bd.StructureByteStride;
                                break;
                            }
                            default: break;
                            }
                            if (elemSize != 2 && elemSize != 4) {
                                tBuf->Release();
                                rej = "tSrvFmt"; break;
                            }
                            if ((uint64_t)tNE * elemSize < 2ull * nGroups) {
                                tBuf->Release();
                                rej = "tSrvCap"; break;
                            }
                            std::vector<uint8_t> tBytes;
                            const uint32_t want = nGroups * 2;
                            const bool readOk =
                                ReadbackBufferSlice(tBuf, tFE * elemSize,
                                                    want, tBytes) == want;
                            tBuf->Release();
                            if (!readOk) { rej = "tReadback"; break; }
                            T.resize(nGroups);
                            std::memcpy(T.data(), tBytes.data(), want);
                            haveT = true;
                        }
                        if (T[0] != 0) { rej = "t0"; break; }
                        bool mono = true;
                        for (uint32_t g = 1; g < nGroups; ++g) {
                            const int d = (int)T[g] - (int)T[g - 1];
                            if (d != 0 && d != 1) { mono = false; break; }
                        }
                        if (!mono) { rej = "mono"; break; }
                        // No wrapper tag/count gate: the +0x38 typeTag==2 and
                        // +0x34 elementCount fields are decompiler-proven only
                        // for the bake-path CreateStructuredBuffer wrapper --
                        // live 2026-07-07, EVERY shipped (BA2-loaded) shape
                        // rejected at tag with a perfectly-formed T, and the
                        // 2026-07-04 live pass had already shown +0x38 is a
                        // REFCOUNT on live wrappers. The count that matters
                        // for correctness is the one the submit loop indexes:
                        // instRecords.size() from the t8 readback (fence-line
                        // proven). T is monotone here, so its last element is
                        // max(T).
                        recBufCount = (uint32_t)instRecords.size();
                        if ((uint32_t)T[nGroups - 1] + 1 != recBufCount) {
                            rej = "recMatch"; break;
                        }
                        // Per-band range validation: contiguity across
                        // ascending active bands and full record coverage
                        // (the per-record expansion happens at submit).
                        uint32_t prefix = 0, expectStart = 0;
                        bool ok = true;
                        for (int s = 0; s < 3 && ok; ++s) {
                            if (!instSegTris[s]) continue;
                            const uint32_t fg = prefix / GS;
                            const uint32_t ng = instSegTris[s] / GS;
                            const uint32_t lg = fg + ng - 1;
                            if (lg >= nGroups) { ok = false; break; }
                            const uint32_t rs = T[fg];
                            const uint32_t rc2 = (uint32_t)T[lg] - rs + 1;
                            if (rs != expectStart) { ok = false; break; }
                            expectStart = rs + rc2;
                            prefix += instSegTris[s];
                        }
                        if (!ok || expectStart != recBufCount) {
                            rej = "ranges"; break;
                        }
                        t7Table = std::move(T);
                        t7GS = GS;
                        t7Valid = true;
                    } while (false);
                    if (t7Defer && state.mergeT7Deferrals < 240) {
                        // Transient by definition (pending-upload counter);
                        // retry the whole resolve next tick. No log line:
                        // deferrals would burn the capped counters and hide
                        // the real OK/REJECT population. Bounded (~4s of
                        // ticks) because +0x44 is only proven for the
                        // bake-path wrapper -- junk that never clears must
                        // fall through to the capture/fallback chain, not
                        // hide the shape forever.
                        ++state.mergeT7Deferrals;
                        return false;
                    }
                    static std::atomic<int> sT7Logs{0};
                    const int tn = sT7Logs.fetch_add(1, std::memory_order_relaxed);
                    if (tn < 60) {
                        // Dump the head of whatever we read as T: content
                        // evidence for the load-path wrapper/format question
                        // (u16 vs u32 elements, headers, pending uploads).
                        char tdump[96] = "";
                        if (!T.empty()) {
                            size_t p = 0;
                            for (size_t d = 0; d < T.size() && d < 8; ++d) {
                                const int wln = snprintf(tdump + p, sizeof(tdump) - p,
                                                         "%s%u", d ? "," : " T=[",
                                                         (unsigned)T[d]);
                                if (wln <= 0 || (size_t)wln >= sizeof(tdump) - p) break;
                                p += (size_t)wln;
                            }
                            snprintf(tdump + p, sizeof(tdump) - p, "]");
                        }
                        _MESSAGE("FO4RemixPlugin: [MergeT7] #%d hash=0x%llX %s%s "
                                 "GS=%u groups=%u recs=%u segTris=[%u,%u,%u] "
                                 "ub=%u srv=[f%u fe=%u ne=%u]%s",
                                 tn, (unsigned long long)hash,
                                 t7Valid ? "OK" : "REJECT:",
                                 t7Valid ? "" : (rej ? rej : "?"),
                                 GS, nGroups, recBufCount,
                                 instSegTris[0], instSegTris[1], instSegTris[2],
                                 tUsed, tFmt, tFE, tNE,
                                 tdump);
                    }
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
                // Capture deferral only matters when the t7 table did NOT
                // validate -- a t7-partitioned shape resolves immediately.
                if (!t7Valid && nzEarly >= 2 && g_config.mergeInstanceDrawCapture &&
                    instBufPtr) {
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
                    // data to test any decode hypothesis offline. e[] is
                    // f[16..19] -- per NIF BSPackedGeomDataCombined each
                    // combined instance carries a bounding sphere; if
                    // e[1..3] encode radius/center, records of the same
                    // sub-model share a radius and the record partition
                    // (9+4 on the road cluster) falls out of THIS dump
                    // with no draw capture at all. Hex alongside floats:
                    // ints/flags masquerade as denormals in %g.
                    if (n < 10) {
                        const size_t kMax = instRecords.size() < 48 ? instRecords.size() : 48;
                        for (size_t k = 0; k < kMax; ++k) {
                            const float* f = instRecords[k].data();
                            uint32_t e[4];
                            std::memcpy(e, f + 16, sizeof(e));
                            _MESSAGE("FO4RemixPlugin: [MergeInstRec] #%d k=%zu "
                                     "r=[%.4f,%.4f,%.4f|%.4f,%.4f,%.4f|%.4f,%.4f,%.4f] "
                                     "t=(%.2f,%.2f,%.2f) s=%.3f "
                                     "e=[%.4g,%.4g,%.4g,%.4g|%08X,%08X,%08X,%08X]",
                                     n, k,
                                     f[0], f[1], f[2], f[4], f[5], f[6],
                                     f[8], f[9], f[10],
                                     f[12], f[13], f[14], f[15],
                                     f[16], f[17], f[18], f[19],
                                     e[0], e[1], e[2], e[3]);
                        }
                    }
                } else {
                    _MESSAGE("FO4RemixPlugin: [MergeInst] #%d hash=0x%llX stream read "
                             "FAILED -> single-draw fallback leafPos=(%.1f,%.1f,%.1f)",
                             n, (unsigned long long)hash,
                             W.pos.x, W.pos.y, W.pos.z);
                }
            }
            // [MergeVtx] diagnostic (2026-07-06): the black-object repro shows
            // merge fallbacks rendering pure black while textures resolve fine
            // and zero [FORK-DIAG] events fire runtime-side. Two candidate
            // data-side causes: (a) baked vertex COLORS -- bit 37 is set on
            // the black trash-pile shapes and the pooled color stream of
            // precombined geometry is a blend/AO mask, often near-black;
            // (b) wrong UV decode against the pool layout. One capped stats
            // line per shape discriminates: black colors -> (a); degenerate
            // or wild UVs -> (b).
            {
                static std::atomic<int> sVtxLogs{0};
                const int vn = g_config.diagEnabled
                    ? sVtxLogs.fetch_add(1, std::memory_order_relaxed) : 150;
                if (vn < 150 && !mesh.vertices.empty()) {
                    uint32_t rMin = 255, rMax = 0;
                    uint64_t rSum = 0, gSum = 0, bSum = 0, aSum = 0;
                    float uMin = 1e9f, uMax = -1e9f, vMin = 1e9f, vMax = -1e9f;
                    for (const auto& v : mesh.vertices) {
                        const uint32_t c = v.color;
                        const uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF,
                                       b = (c >> 16) & 0xFF, a = (c >> 24) & 0xFF;
                        rSum += r; gSum += g; bSum += b; aSum += a;
                        if (r < rMin) rMin = r;
                        if (r > rMax) rMax = r;
                        if (v.texcoord[0] < uMin) uMin = v.texcoord[0];
                        if (v.texcoord[0] > uMax) uMax = v.texcoord[0];
                        if (v.texcoord[1] < vMin) vMin = v.texcoord[1];
                        if (v.texcoord[1] > vMax) vMax = v.texcoord[1];
                    }
                    const size_t nv = mesh.vertices.size();
                    // Diffuse content stats: if the extracted albedo pixels
                    // are themselves near-black, the object renders black
                    // with every other stage working perfectly -- the one
                    // mechanism left after FORK-DIAG and vertex stats came
                    // back clean.
                    uint32_t tw = 0, th = 0, tfmt = 0, tMean[4] = {};
                    BsExtraction::GetCachedTextureStats(mesh.diffuseTextureHash,
                                                        &tw, &th, &tfmt, tMean);
                    const char* dTexName = "(none)";
                    if (mat && mat->spDiffuseTexture) {
                        const char* nm = mat->spDiffuseTexture->name.c_str();
                        if (nm) dTexName = nm;
                    }
                    _MESSAGE("FO4RemixPlugin: [MergeVtx] #%d hash=0x%llX verts=%zu "
                             "bit37=%d colorMeanRGBA=(%llu,%llu,%llu,%llu) rMin=%u rMax=%u "
                             "uv=[%.3f..%.3f, %.3f..%.3f] desc=0x%llX vsize=%u "
                             "difTex='%s' difHash=0x%llX %ux%u fmt=%u difMeanRGBA=(%u,%u,%u,%u)",
                             vn, (unsigned long long)hash, nv,
                             (int)((propFlagsEarly >> 37) & 1),
                             (unsigned long long)(rSum / nv), (unsigned long long)(gSum / nv),
                             (unsigned long long)(bSum / nv), (unsigned long long)(aSum / nv),
                             rMin, rMax, uMin, uMax, vMin, vMax,
                             (unsigned long long)tri->vertexDesc,
                             (unsigned)tri->GetVertexSize(),
                             dTexName, (unsigned long long)mesh.diffuseTextureHash,
                             tw, th, tfmt, tMean[0], tMean[1], tMean[2], tMean[3]);
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
        std::vector<SegDraw> t7Segs;
        {
            uint32_t totalSegTris = 0;
            int nonZero = 0;
            for (int s = 0; s < 4; ++s) {
                totalSegTris += instSegTris[s];
                if (instSegTris[s]) ++nonZero;
            }
            const uint32_t meshTris = (uint32_t)(mesh.indices.size() / 3);
            // Snapshot before the capture path replaces instRecords with a
            // single identity record: the background watch needs the REAL
            // record count for its desc-verified SRV matching.
            const uint32_t realRecCount = (uint32_t)instRecords.size();
            if (t7Valid) {
                // Take 13 (2026-07-06): engine-exact per-RECORD expansion
                // from the CPU-resident u16 group->record table. The
                // engine's VS transforms each GS-triangle group by ITS OWN
                // record (t8[t7[group]]) inside one plain DrawIndexed --
                // the merged mesh already contains one padded triangle
                // block per (band, model, placement). So the correct
                // plugin expansion is one submesh per record: the
                // contiguous group run mapped to that record, drawn once
                // at that record's transform. Capture-free, off-screen-
                // safe, and correct for BOTH multi-band and single-band
                // multi-model shapes (the latter were silently duplicated
                // by the old whole-mesh x all-records semantic).
                t7Segs.reserve(instRecords.size());
                const uint32_t nG = (uint32_t)t7Table.size();
                uint32_t g = 0;
                while (g < nG) {
                    const uint16_t r = t7Table[g];
                    uint32_t gEnd = g + 1;
                    while (gEnd < nG && t7Table[gEnd] == r) ++gEnd;
                    t7Segs.push_back({ g * t7GS, (gEnd - g) * t7GS,
                                       (size_t)r, 1 });
                    g = gEnd;
                }
                // ---- [MergeSphere] rotation-convention self-check
                // (2026-07-07). Vault-111 ground truth: pieces land at the
                // right POSITIONS under either convention (vault leaf
                // rotations are identity, so translation is convention-
                // independent) but ORIENTATIONS come out mixed-wrong under
                // both MergeInstanceRowVector settings -- fences/roads were
                // too symmetric to ever catch it. The engine keeps an
                // independent oracle: the parent BSFadeNode's per-record
                // bounding-sphere array (research doc 1.5/evidence
                // 0x1421E5250 -- same order as records, centers taken from
                // source data WITHOUT the translation rebase). Predict each
                // record's sphere center from its t7 vertex-block centroid
                // under BOTH conventions and log which one the engine's own
                // spheres agree with. Read-only diagnostic, capped.
                static std::atomic<int> sSphereLogs{0};
                if (sSphereLogs.load(std::memory_order_relaxed) < 40) {
                    do {
                        uint64_t prop = 0, fadeNode = 0, arr = 0;
                        if (!PeekQwordsGuarded(reinterpret_cast<const void*>(
                                reinterpret_cast<uintptr_t>(tri) + 0x138),
                                &prop, 1) || !HeapLikePtr(prop)) break;
                        if (!PeekQwordsGuarded(reinterpret_cast<const void*>(
                                (uintptr_t)prop + 0x48), &fadeNode, 1) ||
                            !HeapLikePtr(fadeNode)) break;
                        if (!PeekQwordsGuarded(reinterpret_cast<const void*>(
                                (uintptr_t)fadeNode + 0x180), &arr, 1) ||
                            !HeapLikePtr(arr)) break;
                        const size_t nRec = instRecords.size();
                        std::vector<float> sph(nRec * 4);
                        if (!PeekBytesGuarded(reinterpret_cast<const void*>(
                                (uintptr_t)arr), sph.data(),
                                (uint32_t)(nRec * 16))) break;
                        bool sane = true;
                        for (size_t k = 0; k < nRec && sane; ++k) {
                            const float rad = sph[k * 4 + 3];
                            sane = std::isfinite(rad) && rad > 0.01f &&
                                   rad < 1.0e6f &&
                                   std::isfinite(sph[k * 4]) &&
                                   std::isfinite(sph[k * 4 + 1]) &&
                                   std::isfinite(sph[k * 4 + 2]);
                        }
                        if (!sane) break;
                        // Shape-local translate = the merged bound center the
                        // bake subtracted from record translations (S+0x60).
                        float C[3] = {};
                        PeekBytesGuarded(reinterpret_cast<const void*>(
                            reinterpret_cast<uintptr_t>(tri) + 0x60), C, 12);
                        double eRow = 0, eCol = 0, eRowC = 0, eColC = 0;
                        int used = 0;
                        for (const auto& sd2 : t7Segs) {
                            // Centroid of this record's vertex block,
                            // cluster-local parsed positions.
                            double cx = 0, cy = 0, cz = 0;
                            size_t nv2 = 0;
                            const size_t i0 = (size_t)sd2.triStart * 3;
                            const size_t i1 = i0 + (size_t)sd2.triCount * 3;
                            for (size_t ii = i0; ii < i1 && ii < mesh.indices.size(); ++ii) {
                                const auto& v = mesh.vertices[mesh.indices[ii]];
                                cx += v.position[0];
                                cy += v.position[1];
                                cz += v.position[2];
                                ++nv2;
                            }
                            if (!nv2) continue;
                            cx /= nv2; cy /= nv2; cz /= nv2;
                            const float* f = instRecords[sd2.recStart].data();
                            const float s2 = f[15];
                            const float* sc = &sph[sd2.recStart * 4];
                            double pr[3], pc[3];
                            for (int i = 0; i < 3; ++i) {
                                const double c3[3] = { cx, cy, cz };
                                // row-vector: pred_j = sum_i c_i * R[i][j]
                                pr[i] = (c3[0] * f[0 * 4 + i] +
                                         c3[1] * f[1 * 4 + i] +
                                         c3[2] * f[2 * 4 + i]) * s2 + f[12 + i];
                                // col-vector: pred_i = sum_j R[i][j] * c_j
                                pc[i] = (c3[0] * f[i * 4 + 0] +
                                         c3[1] * f[i * 4 + 1] +
                                         c3[2] * f[i * 4 + 2]) * s2 + f[12 + i];
                            }
                            auto d3 = [&](const double p[3], const float off[3]) {
                                const double dx = p[0] + off[0] - sc[0];
                                const double dy = p[1] + off[1] - sc[1];
                                const double dz = p[2] + off[2] - sc[2];
                                return std::sqrt(dx * dx + dy * dy + dz * dz);
                            };
                            static const float kZero3[3] = { 0, 0, 0 };
                            eRow  += d3(pr, kZero3);
                            eCol  += d3(pc, kZero3);
                            eRowC += d3(pr, C);
                            eColC += d3(pc, C);
                            ++used;
                        }
                        if (!used) break;
                        const int sn = sSphereLogs.fetch_add(1,
                                           std::memory_order_relaxed);
                        if (sn < 40) {
                            _MESSAGE("FO4RemixPlugin: [MergeSphere] #%d hash=0x%llX "
                                     "n=%d C=(%.1f,%.1f,%.1f) "
                                     "errRow=%.1f errCol=%.1f errRowC=%.1f errColC=%.1f "
                                     "sph0=(%.1f,%.1f,%.1f r=%.1f) rec0t=(%.1f,%.1f,%.1f)",
                                     sn, (unsigned long long)hash, used,
                                     C[0], C[1], C[2],
                                     eRow / used, eCol / used,
                                     eRowC / used, eColC / used,
                                     sph[0], sph[1], sph[2], sph[3],
                                     instRecords[0][12], instRecords[0][13],
                                     instRecords[0][14]);
                        }
                    } while (false);
                }
            }
            if (nonZero >= 2 && !t7Valid) {
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
                if (nSegs == 0 && g_config.mergeInstanceDrawCapture && instBufPtr) {
                    std::vector<DrawCapture::SegDraw> cap;
                    if (DrawCapture::Query(instBufPtr, instSrvPtr, hash,
                                           (uint32_t)instRecords.size(),
                                           instSegTris, cap) ==
                        DrawCapture::kReady) {
                        const uint32_t rc = (uint32_t)instRecords.size();
                        // ---- Baked-mesh rebuild (take 10) ----
                        // Preferred: replace our parsed source mesh with
                        // the engine's own baked expanded geometry (see
                        // BuildMeshFromChunks). One drawable, leaf
                        // transform only -- the records are already baked
                        // into the vertices by the engine.
                        DrawCapture::ChunkDraw chunks[64];
                        const int nChunks = DrawCapture::GetChunks(hash, chunks, 64);
                        bool bakedMesh = false;
                        if (nChunks > 0) {
                            std::vector<remixapi_HardcodedVertex> bv;
                            std::vector<uint32_t> bi;
                            int keptChunks = 0;
                            if (BuildMeshFromChunks(tri, chunks, nChunks, hash,
                                                    mesh.useVertexColors,
                                                    instRecords, mesh.vertices,
                                                    bv, bi, keptChunks)) {
                                mesh.vertices = std::move(bv);
                                mesh.indices = std::move(bi);
                                instRecords.clear();
                                std::array<float, 20> ident = {};
                                ident[0] = ident[5] = ident[10] = ident[15] = 1.0f;
                                instRecords.push_back(ident);
                                segs[0] = { 0, (uint32_t)(mesh.indices.size() / 3),
                                            0, 1 };
                                nSegs = 1;
                                bakedMesh = true;
                                // Mark the accumulated chunk set consumed:
                                // the background hunt keeps merging new
                                // chunk draws (the engine only draws
                                // visible pieces per frame, so a single
                                // capture is a view-dependent SUBSET), and
                                // the upgrade poll re-resolves when the
                                // set grows -- holes fill in as the player
                                // looks around.
                                DrawCapture::MarkConsumed(hash);
                            }
                        }
                        if (!bakedMesh) {
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
                }
                // Keep the background hunt alive for every real multi-seg
                // shape, whether the capture starved (engine only issues
                // chunk draws for clusters it currently renders -- an
                // off-screen cluster can never capture at initial resolve)
                // or succeeded (a single-frame capture covers only the
                // pieces visible THAT frame). Tick's upgrade poll
                // re-resolves when the accumulated chunk set grows, so the
                // engine-exact geometry appears/completes as the player
                // looks around; DrawCapture's per-watch upgrade budget
                // bounds the resubmission churn.
                // t7-partitioned shapes are already engine-exact -- no
                // background watch or upgrade churn needed for them.
                if (!t7Valid && g_config.mergeInstanceDrawCapture && instBufPtr) {
                    state.mergeCaptureUpgradePending = true;
                    state.mergeWatchBuf = instBufPtr;
                    state.mergeWatchSrv = instSrvPtr;
                    state.mergeWatchRecordCount = realRecCount;
                    std::memcpy(state.mergeWatchSegTris, instSegTris,
                                sizeof(state.mergeWatchSegTris));
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
            if (nSegs == 0 && t7Segs.empty()) {
                // Unresolved-partition fallback: ONE record (2026-07-06).
                // Whole-mesh x ALL records stacks N copies of the entire
                // cluster mesh onto overlapping placements, and the flat
                // segments in the concatenation z-fight to PURE BLACK in the
                // path tracer -- this was the "black texture bug" (texture
                // content, vertex data, material creation, and the runtime
                // texture table were all verified healthy; single-record
                // submission fixed ~99% of black objects, user-verified).
                // A flatness gate ("3D shapes get all records") was tried
                // and REVERTED same day: multi-segment concatenations mix
                // flat blankets with tall pieces, so shapes classified 3D
                // still contained flat segments that stacked black. Every
                // record-multiplying variant of this fallback re-blackens
                // something. With take 13 (t7 table) most shapes never
                // reach here; the leftovers are invariant-violating shapes
                // where one visible copy remains the safe choice.
                segs[0] = { 0, meshTris, 0, 1 };
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
        // Merge geometry renders DOUBLE-SIDED (2026-07-07). The submitted
        // transforms are engine-exact -- proven twice: the merge VS DXBC
        // (research doc §7, row-vector v*R, stored rows) and the engine's
        // own per-record bounding spheres ([MergeSphere], row-vector fits
        // every rotation-bearing shape) -- yet Vault 111 still showed a
        // stable per-shape mixed set of inside-out pieces under single-
        // sided culling, through every transform-convention combination.
        // With uniform code, per-shape variance can only be CONTENT: the
        // baked kit models' authored winding is not consistently front-
        // facing (vanilla tolerates it -- cull mode is a per-item material
        // field, doc §6, and the shipped kit materials evidently don't rely
        // on backface culling for these). Until the [MergeWind] stats below
        // justify a per-triangle normal-matched re-flip, double-sided is
        // the vanilla-faithful choice; scope is merge shapes only.
        // [Performance] MergeTwoSided=0 is the single-sided experiment: the
        // 2026-07-07 inside-out evidence predated the b112e08 batched-base
        // mirror fix, which may have been the real culprit -- single-sided
        // merges (with the per-instance det flip below re-enabled) would be
        // a real path-tracing perf win if the content winding holds up.
        mesh.isTwoSided = g_config.mergeTwoSided;
        // [MergeWind] winding-vs-normals stats: fraction of sampled
        // triangles whose geometric normal (current winding) OPPOSES the
        // authored vertex normals. ~0 or ~1 per shape = consistent content
        // (a deterministic re-flip could restore single-sided culling);
        // mid-range = mixed within one mesh.
        {
            static std::atomic<int> sWindLogs{0};
            if (g_config.diagEnabled &&
                sWindLogs.load(std::memory_order_relaxed) < 40 &&
                !mesh.indices.empty()) {
                const size_t nTri = mesh.indices.size() / 3;
                const size_t step = nTri > 200 ? nTri / 200 : 1;
                int nNeg = 0, nUsed = 0;
                double dotSum = 0;
                for (size_t t = 0; t < nTri; t += step) {
                    const auto& a = mesh.vertices[mesh.indices[t * 3 + 0]];
                    const auto& b = mesh.vertices[mesh.indices[t * 3 + 1]];
                    const auto& c = mesh.vertices[mesh.indices[t * 3 + 2]];
                    const double e1[3] = { b.position[0] - a.position[0],
                                           b.position[1] - a.position[1],
                                           b.position[2] - a.position[2] };
                    const double e2[3] = { c.position[0] - a.position[0],
                                           c.position[1] - a.position[1],
                                           c.position[2] - a.position[2] };
                    const double gn[3] = { e1[1] * e2[2] - e1[2] * e2[1],
                                           e1[2] * e2[0] - e1[0] * e2[2],
                                           e1[0] * e2[1] - e1[1] * e2[0] };
                    const double gl = std::sqrt(gn[0] * gn[0] + gn[1] * gn[1] +
                                                gn[2] * gn[2]);
                    if (gl < 1e-9) continue;  // padding/degenerate tris
                    const double vn[3] = {
                        (double)a.normal[0] + b.normal[0] + c.normal[0],
                        (double)a.normal[1] + b.normal[1] + c.normal[1],
                        (double)a.normal[2] + b.normal[2] + c.normal[2] };
                    const double vl = std::sqrt(vn[0] * vn[0] + vn[1] * vn[1] +
                                                vn[2] * vn[2]);
                    if (vl < 1e-9) continue;
                    const double d = (gn[0] * vn[0] + gn[1] * vn[1] +
                                      gn[2] * vn[2]) / (gl * vl);
                    dotSum += d;
                    if (d < 0) ++nNeg;
                    ++nUsed;
                }
                if (nUsed > 0) {
                    const int wn = sWindLogs.fetch_add(1,
                                       std::memory_order_relaxed);
                    if (wn < 40) {
                        _MESSAGE("FO4RemixPlugin: [MergeWind] #%d hash=0x%llX "
                                 "tris=%d meanDot=%.2f fracNeg=%.2f",
                                 wn, (unsigned long long)hash, nUsed,
                                 dotSum / nUsed, (double)nNeg / nUsed);
                    }
                }
            }
        }
        const std::vector<uint32_t> fullIndices = std::move(mesh.indices);
        // Take 13: the t7 per-record expansion supersedes the fixed segs[]
        // array when the table validated (one submesh per record; count can
        // exceed 8).
        const SegDraw* segArr = t7Segs.empty() ? segs : t7Segs.data();
        const int segN = t7Segs.empty() ? nSegs : (int)t7Segs.size();
        for (int s = 0; s < segN; ++s) {
            const SegDraw& sd = segArr[s];
            // Segment sub-mesh: index subrange (triangle-aligned, winding
            // flip from the parse preserved); vertices shared as-is --
            // ContentHashOf covers (vertices, indices) so each segment gets
            // its own Remix mesh/BLAS.
            mesh.indices.assign(fullIndices.begin() + (size_t)sd.triStart * 3,
                                fullIndices.begin() + ((size_t)sd.triStart + sd.triCount) * 3);
            // Winding parity is PER INSTANCE, not per shape -- the flip (if
            // any) is applied inside the record loop below. Vault-111
            // evidence (2026-07-07): record-0 fallback submissions of wall
            // clusters rendered correct-side-in, the same triangles expanded
            // to other records rendered inside-out (user tcl-verified), and
            // a blanket whole-mesh flip inverted previously-correct
            // geometry. Mirrored placements (negative-determinant record
            // rotations, standard for interior kit walls) add a parity the
            // constant parse-time flip can't see; the composed determinant
            // below decides each instance. This is exactly the "reversed-
            // winding mesh clone" follow-up the detNeg diagnostic predicted.
            bool meshWindingFlipped = false;
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
                // Mirrored instance (composed 3x3 det < 0): the record adds
                // a reflection on top of the Beth->Remix mirror the parse
                // flip already compensates, so this instance needs its
                // triangles re-flipped or its single-sided faces cull
                // inward (Vault 111 walls). Toggle in place; the content
                // hash covers indices, so flipped instances get their own
                // mesh/BLAS bucket. SKIPPED while the merge renders two-
                // sided (2026-07-10): under double-sided rendering the flip
                // is visually a no-op, but it still burned O(indices) swaps
                // per det-sign change and split otherwise-identical
                // instances into two BLAS/instancing buckets -- defeating
                // the sharing the merge path exists to provide.
                if (!mesh.isTwoSided) {
                    const float detC =
                        comp[0] * (comp[5] * comp[10] - comp[6] * comp[9]) -
                        comp[1] * (comp[4] * comp[10] - comp[6] * comp[8]) +
                        comp[2] * (comp[4] * comp[9]  - comp[5] * comp[8]);
                    const bool needFlip = detC < 0.0f;
                    if (needFlip != meshWindingFlipped) {
                        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
                            std::swap(mesh.indices[t + 1], mesh.indices[t + 2]);
                        }
                        meshWindingFlipped = needFlip;
                    }
                }
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
    if (headDiag) {
        HeadDiagLog(hash, "SUBMITTED skinned=%d bones=%u diffuse=%016llX matType=%u",
                    mesh.hasSkinning ? 1 : 0, mesh.boneCount,
                    (unsigned long long)mesh.diffuseTextureHash,
                    (unsigned)mat->GetType());
    }
    // Note: state.materialHash is left at 0 here. SubmitDrawable's hash
    // computation is internal to remix_renderer; if you want symmetric
    // tracking, expose a helper or have SubmitDrawable take an out-param.
    // For 1B, ReleaseDrawable looks up by `hash` and finds the materialHash
    // via g_drawables, so leaving state.materialHash at 0 is fine -- the
    // refcount cleanup goes through g_drawables anyway.
    // Per-submission diagnostic, gated on [Logging] LogShapeInfo (2026-07-10;
    // it was unconditional -- the one hot-path log with no throttle. A cell
    // attach resolves hundreds of statics in one burst on the game thread,
    // each paying an RTTI walk + raw engine reads + a ~30-arg _MESSAGE, all
    // competing with the resolve time budget).
    if (g_config.logShapeInfo) {
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

        // Alpha-test diagnostic (2026-04-29): emit material type,
        // BSShaderProperty shader-flags, and the contents of geo->effectState
        // (the NiAlphaProperty slot at offset 0x130 on BSGeometry per f4se
        // BSGeometry.h). For foliage drawables that render as solid alpha
        // cards, we want to know which alpha-test signal source the engine is
        // using -- NiAlphaProperty (geo level), BSLightingShaderProperty::
        // flags (shader level), or BSLightingShaderMaterialBase fields
        // (material level, requires GetType discriminator).
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

        // Rotation+scale dump (PROBE 2026-05-03): roads/statics rendering
        // flat when bUsePreCombines=0; need to see whether
        // m_worldTransform.rot arrives as identity (rotation lost upstream)
        // or with the slope intact (then BuildRemixTransform / Remix
        // submission is the leak).
        const auto& rot = tri->m_worldTransform.rot;
        const float scale = tri->m_worldTransform.scale;
        // Leaf RTTI class name (PROBE 2026-05-03): identify whether
        // road/static sub-meshes are plain BSTriShape vs
        // BSMergeInstancedTriShape vs another subclass with per-instance
        // transform attributes we don't handle.
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
    }
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
