#include "lighting_static.h"

#include "../bs_extraction.h"
#include "../semantic_capture.h"
#include "../remix_renderer.h"
#include "../config.h"
#include "../fo4_diagnostics.h"
#include "f4se/NiObjects.h"
#include "f4se/NiNodes.h"      // NiNode (parent-chain walk in InstDiag)
#include "f4se/NiExtraData.h"  // NiExtraData (InstDiag extra-data peek)
#include "f4se/BSGeometry.h"
#include "f4se/NiMaterials.h"
#include "f4se/PluginAPI.h"  // _MESSAGE

#include <atomic>
#include <vector>
#include <unordered_set>
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
// InstDiag v5 (2026-07-03). v4 verdict: 64-byte [rot 3x4 | trans, 1.0]
// records (plus a wider sphere-bearing variant) DO exist near several
// wrapper members -- the format is confirmed -- but the 16KB heap sweep is
// unbounded, so hits landed in neighboring allocations and could not be
// attributed to the probed shape (#3 and #11 "found" the identical far-away
// record; #5's index member hit far past its 0x480-byte payload). Also
// shapes #1/#3 have NULL +0x1D0/+0x1D8, so the CPU-side aux object is not
// universal. The per-shape D3D objects ARE universal: wrap+170.q0/.q1 and
// wrap+178.q3 carry d3d11.dll vtables (two distinct ones -> buffer/SRV
// pairs) and wrap+178.q0 is the shared pool. v5 interrogates those
// directly:
//   - QueryInterface for ID3D11Buffer / ID3D11ShaderResourceView (guarded;
//     only attempted when the vtable lies inside d3d11.dll's image);
//   - log descs: ByteWidth / StructureByteStride / BindFlags give record
//     size and slice bounds; a buffer-dimension SRV's FirstElement /
//     NumElements give the pool slice AND likely the instance count;
//   - copy contents back through a STAGING buffer (bounded by ByteWidth,
//     no neighbor ambiguity) and run a strict record-run detector.
// Heap-side members (game vtables / raw data) get the same detector over a
// SEH-snapshotted 16KB window so CPU-side candidates stay visible.
//
// The detector looks for consecutive fixed-stride records, each parsing as
// three near-orthonormal rotation rows (uniform scale tolerated) plus a
// finite translation, in either of two layouts:
//   A: rows padded to 4 with EXACT 0.0f, translation row 4 = [t.xyz, s]
//      (the 64-byte pattern v4 confirmed in the wild)
//   B: HLSL float3x4 -- translation in column 3 (48-byte instance streams)
// Translations are classified world-near-shape / cluster-local / zero so a
// run can be attributed to the probed shape by content, and the run length
// can be correlated against the +0x190 capacity dwords.

// One detected run of fixed-stride transform records.
struct XformRun {
    int   stride = 0;    // bytes between record starts
    int   phase  = 0;    // byte offset of the first record
    int   count  = 0;    // consecutive valid records
    char  layout = '-';  // 'A' or 'B' (see above)
    int   nWorld = 0, nLocal = 0, nZero = 0;
    float t0[3] = {}, t1[3] = {}, tN[3] = {};
};

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

// Layout B, 12 floats: rows (0..2)(4..6)(8..10), translation 3/7/11.
static bool ValidRecordB(const float* f) {
    if (!RotRowsPlausible(f, 0, 4, 8)) return false;
    return TransPlausible(f[3], f[7], f[11]);
}

// 0=zero 1=cluster-local 2=world-near-shape 3=far (someone else's data)
static int ClassifyTrans(const float t[3], const float wpos[3]) {
    if (std::fabs(t[0]) < 0.01f && std::fabs(t[1]) < 0.01f && std::fabs(t[2]) < 0.01f) return 0;
    if (std::fabs(t[0] - wpos[0]) < 8192.0f && std::fabs(t[1] - wpos[1]) < 8192.0f &&
        std::fabs(t[2] - wpos[2]) < 8192.0f) return 2;
    if (std::fabs(t[0]) < 8192.0f && std::fabs(t[1]) < 8192.0f && std::fabs(t[2]) < 8192.0f) return 1;
    return 3;
}

// Longest fixed-stride run of valid records in [0, size), first record no
// deeper than maxPhase. Layout A wins ties (stricter validation).
static XformRun FindBestXformRun(const uint8_t* data, size_t size,
                                 const float wpos[3], int maxPhase = 1 << 30) {
    XformRun best;
    if (size < 64) return best;
    const size_t nOff = (size - 64) / 8 + 1;  // 8-byte-aligned record starts
    std::vector<uint8_t> validA(nOff), validB(nOff);
    for (size_t i = 0; i < nOff; ++i) {
        const float* f = reinterpret_cast<const float*>(data + i * 8);
        validA[i] = ValidRecordA(f) ? 1 : 0;
        validB[i] = ValidRecordB(f) ? 1 : 0;
    }
    static const int kStrides[] = { 48, 64, 80, 96, 112, 128, 144, 160,
                                    176, 184, 192, 208, 224, 240, 256 };
    std::vector<int> run(nOff);
    const char layouts[2] = { 'A', 'B' };
    for (char layout : layouts) {
        const std::vector<uint8_t>& valid = (layout == 'A') ? validA : validB;
        for (int s : kStrides) {
            if (layout == 'A' && s < 64) continue;
            const size_t step = (size_t)s / 8;
            for (size_t i = nOff; i-- > 0;) {
                run[i] = valid[i] ? (i + step < nOff ? run[i + step] + 1 : 1) : 0;
            }
            for (size_t i = 0; i < nOff; ++i) {
                if (!run[i] || i * 8 > (size_t)maxPhase) continue;
                if (i >= step && run[i - step] > run[i]) continue;  // mid-run
                if (run[i] > best.count) {
                    best.stride = s;
                    best.phase  = (int)(i * 8);
                    best.count  = run[i];
                    best.layout = layout;
                }
            }
        }
    }
    if (best.count) {
        for (int k = 0; k < best.count; ++k) {
            const float* f = reinterpret_cast<const float*>(
                data + best.phase + (size_t)k * best.stride);
            float t[3];
            if (best.layout == 'A') { t[0] = f[12]; t[1] = f[13]; t[2] = f[14]; }
            else                    { t[0] = f[3];  t[1] = f[7];  t[2] = f[11]; }
            const int c = ClassifyTrans(t, wpos);
            if (c == 0) ++best.nZero;
            else if (c == 1) ++best.nLocal;
            else if (c == 2) ++best.nWorld;
            if (k == 0)              std::memcpy(best.t0, t, sizeof(t));
            if (k == 1)              std::memcpy(best.t1, t, sizeof(t));
            if (k == best.count - 1) std::memcpy(best.tN, t, sizeof(t));
        }
    }
    return best;
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

// SEH-snapshot up to `bytes` from a heap candidate into `out` (512-byte
// chunks; stops at the first unmapped chunk). Returns bytes captured.
static size_t SnapshotHeapGuarded(uint64_t src, std::vector<uint8_t>& out, size_t bytes) {
    out.resize(bytes);
    size_t got = 0;
    while (got + 512 <= bytes) {
        if (!PeekQwordsGuarded(reinterpret_cast<const void*>(src + got),
                               reinterpret_cast<uint64_t*>(out.data() + got), 64)) {
            break;
        }
        got += 512;
    }
    out.resize(got);
    return got;
}

// Copy `bytes` at `srcOff` of a buffer through a staging buffer created on
// the buffer's OWN device (which may differ from the resolver's device).
// Blocking Map -- acceptable for a capped one-shot diagnostic; the texture
// readback in bs_extraction does the same on this thread.
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

// Bytes per element for the typed-SRV formats BSGraphics plausibly uses for
// an instance stream; 0 = unknown (skip content readback, desc still logs).
static uint32_t DxgiElementSize(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:   return 16;
        case DXGI_FORMAT_R32G32B32_FLOAT:     return 12;
        case DXGI_FORMAT_R32G32_FLOAT:        return 8;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:  return 8;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:            return 4;
        default:                              return 0;
    }
}

static void LogXformRun(int n, int wrapOff, int bi, const char* src, const XformRun& r) {
    if (r.count < 3) {
        _MESSAGE("FO4RemixPlugin: [InstDiag] #%d       %s +%X.q%d no-run", n, src, wrapOff, bi);
        return;
    }
    _MESSAGE("FO4RemixPlugin: [InstDiag] #%d       %s +%X.q%d run%c s=%d p=0x%X c=%d "
             "w=%d l=%d z=%d t0=(%.1f,%.1f,%.1f) t1=(%.1f,%.1f,%.1f) tN=(%.1f,%.1f,%.1f)",
             n, src, wrapOff, bi, r.layout, r.stride, r.phase, r.count,
             r.nWorld, r.nLocal, r.nZero,
             r.t0[0], r.t0[1], r.t0[2], r.t1[0], r.t1[1], r.t1[2],
             r.tN[0], r.tN[1], r.tN[2]);
}

// Probe one heap-like wrapper member: d3d11 COM objects get QI + desc +
// bounded readback; everything else gets a 16KB SEH heap snapshot. Both
// paths feed the record-run detector. wpos = probed shape's world position
// (for run attribution by translation content).
static void InstDiagProbeMember(int n, int wrapOff, int bi, uint64_t member,
                                const uint64_t head[2], ID3D11Device* resolverDev,
                                const float wpos[3]) {
    constexpr uint32_t kGpuCap = 512 * 1024;
    std::vector<uint8_t> bytes;

    if (PtrInD3D11(head[0])) {
        void* raw = nullptr;
        if (QiGuarded(reinterpret_cast<void*>(member), &__uuidof(ID3D11Buffer), &raw)) {
            ID3D11Buffer* buf = static_cast<ID3D11Buffer*>(raw);
            D3D11_BUFFER_DESC bd = {};
            buf->GetDesc(&bd);
            ID3D11Device* bufDev = nullptr;
            buf->GetDevice(&bufDev);
            _MESSAGE("FO4RemixPlugin: [InstDiag] #%d     +%X.q%d=%016llX BUF bw=%u usage=%u "
                     "bind=0x%X cpu=0x%X misc=0x%X ss=%u dev=%s",
                     n, wrapOff, bi, (unsigned long long)member,
                     bd.ByteWidth, (unsigned)bd.Usage, bd.BindFlags,
                     bd.CPUAccessFlags, bd.MiscFlags, bd.StructureByteStride,
                     bufDev == resolverDev ? "same" : "OTHER");
            if (bufDev) bufDev->Release();
            const uint32_t want = bd.ByteWidth < kGpuCap ? bd.ByteWidth : kGpuCap;
            if (want >= 64 && ReadbackBufferSlice(buf, 0, want, bytes)) {
                LogXformRun(n, wrapOff, bi, "gpu",
                            FindBestXformRun(bytes.data(), bytes.size(), wpos));
                // Second look pinned to the buffer head: an instance stream
                // in its own buffer starts at phase ~0 even if some longer
                // accidental run exists deeper in a pooled buffer.
                XformRun headRun = FindBestXformRun(bytes.data(), bytes.size(), wpos, 256);
                if (headRun.count >= 3) LogXformRun(n, wrapOff, bi, "gpu-head", headRun);
            }
            buf->Release();
            return;
        }
        if (QiGuarded(reinterpret_cast<void*>(member),
                      &__uuidof(ID3D11ShaderResourceView), &raw)) {
            ID3D11ShaderResourceView* srv = static_cast<ID3D11ShaderResourceView*>(raw);
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
            srv->GetDesc(&sv);
            ID3D11Resource* res = nullptr;
            srv->GetResource(&res);
            ID3D11Buffer* buf = nullptr;
            if (res) res->QueryInterface(__uuidof(ID3D11Buffer),
                                         reinterpret_cast<void**>(&buf));
            D3D11_BUFFER_DESC bd = {};
            if (buf) buf->GetDesc(&bd);
            const uint32_t first = (sv.ViewDimension == D3D11_SRV_DIMENSION_BUFFER)
                                       ? sv.Buffer.FirstElement : 0;
            const uint32_t num   = (sv.ViewDimension == D3D11_SRV_DIMENSION_BUFFER)
                                       ? sv.Buffer.NumElements : 0;
            _MESSAGE("FO4RemixPlugin: [InstDiag] #%d     +%X.q%d=%016llX SRV fmt=%u dim=%u "
                     "first=%u num=%u -> buf bw=%u ss=%u bind=0x%X misc=0x%X",
                     n, wrapOff, bi, (unsigned long long)member,
                     (unsigned)sv.Format, (unsigned)sv.ViewDimension, first, num,
                     bd.ByteWidth, bd.StructureByteStride, bd.BindFlags, bd.MiscFlags);
            if (buf && num > 0) {
                const uint32_t es = bd.StructureByteStride ? bd.StructureByteStride
                                                           : DxgiElementSize(sv.Format);
                if (es > 0) {
                    uint64_t off = (uint64_t)first * es;
                    uint64_t len = (uint64_t)num * es;
                    if (off < bd.ByteWidth && len >= 64) {
                        if (off + len > bd.ByteWidth) len = bd.ByteWidth - off;
                        if (len > kGpuCap) len = kGpuCap;
                        if (ReadbackBufferSlice(buf, (uint32_t)off, (uint32_t)len, bytes)) {
                            LogXformRun(n, wrapOff, bi, "srv",
                                        FindBestXformRun(bytes.data(), bytes.size(), wpos));
                        }
                    }
                }
            }
            if (buf) buf->Release();
            if (res) res->Release();
            srv->Release();
            return;
        }
        _MESSAGE("FO4RemixPlugin: [InstDiag] #%d     +%X.q%d=%016llX d3d11-other vtbl=%016llX",
                 n, wrapOff, bi, (unsigned long long)member, (unsigned long long)head[0]);
        return;
    }

    // Heap-side candidate (game vtable or raw data): bounded-ish snapshot.
    const size_t got = SnapshotHeapGuarded(member, bytes, 16384);
    _MESSAGE("FO4RemixPlugin: [InstDiag] #%d     +%X.q%d=%016llX heap head=[%016llX %016llX] got=%zu",
             n, wrapOff, bi, (unsigned long long)member,
             (unsigned long long)head[0], (unsigned long long)head[1], got);
    if (got >= 64) {
        LogXformRun(n, wrapOff, bi, "heap",
                    FindBestXformRun(bytes.data(), bytes.size(), wpos));
    }
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

    // ---- Precombine / merge-instanced diagnostic (2026-07-03, capped) ----
    // "Bethesda-placed objects have incorrect transforms, worse with
    // precombines" (roads in Sanctuary, light poles, hedges). Precombined
    // geometry (BSMergeInstancedTriShape / BSMultiStreamInstanceTriShape)
    // carries per-instance placement in structures F4SE does not declare
    // (BSPackedCombinedGeomDataExtra is RTTI-only), while this resolver
    // renders every shape as "local verts x leaf world transform" -- correct
    // for plain refs, wrong for merged instances. This diagnostic gathers
    // the data needed to build the real fix:
    //   - the parsed vertex bounding box: local-space meshes center near the
    //     origin; combined-space baking shows up as a box offset/spanning
    //     hundreds-thousands of units;
    //   - leaf local vs world transform + two parents: shows what placement
    //     information survives on the scene graph;
    //   - the shape's NiExtraData entries (class + name + leading qwords):
    //     the NIF format stores precombined instance transforms in
    //     BSPackedCombinedGeomDataExtra, so if it is present at runtime its
    //     leading fields give us the layout anchor for a follow-up.
    {
        static std::atomic<int> sInstDiagLogs{0};
        char instLeaf[64] = "";
        SemanticCapture::GetLeafClassName(reinterpret_cast<void*>(tri),
                                          instLeaf, sizeof(instLeaf));
        const bool instancedClass =
            std::strstr(instLeaf, "MergeInstanced") != nullptr ||
            std::strstr(instLeaf, "MultiStreamInstance") != nullptr;
        if (instancedClass) {
            const int n = sInstDiagLogs.fetch_add(1, std::memory_order_relaxed);
            if (n < 12) {
                // v2 (2026-07-03): read mesh.vertices, not parsed.* -- the
                // build step above MOVES the parsed vectors into the mesh, so
                // v1 logged verts=0 and a sentinel bbox for every shape.
                float mn[3] = { 3.4e38f, 3.4e38f, 3.4e38f };
                float mx[3] = { -3.4e38f, -3.4e38f, -3.4e38f };
                for (const auto& v : mesh.vertices) {
                    for (int c = 0; c < 3; ++c) {
                        if (v.position[c] < mn[c]) mn[c] = v.position[c];
                        if (v.position[c] > mx[c]) mx[c] = v.position[c];
                    }
                }
                const NiTransform& wx = tri->m_worldTransform;
                const NiTransform& lx = *reinterpret_cast<const NiTransform*>(
                    reinterpret_cast<uintptr_t>(tri) + 0x30);  // m_localTransform
                _MESSAGE("FO4RemixPlugin: [InstDiag] #%d class=%s name=\"%s\" verts=%zu tris=%zu "
                         "vdesc=0x%016llX worldPos=(%.1f,%.1f,%.1f) worldRot0=(%.3f,%.3f,%.3f) "
                         "worldScale=%.3f localPos=(%.1f,%.1f,%.1f) localScale=%.3f "
                         "bboxMin=(%.1f,%.1f,%.1f) bboxMax=(%.1f,%.1f,%.1f)",
                         n, instLeaf, tri->m_name.c_str() ? tri->m_name.c_str() : "",
                         mesh.vertices.size(), mesh.indices.size() / 3,
                         (unsigned long long)parsed.vertexDesc,
                         wx.pos.x, wx.pos.y, wx.pos.z,
                         wx.rot.data[0][0], wx.rot.data[0][1], wx.rot.data[0][2],
                         wx.scale, lx.pos.x, lx.pos.y, lx.pos.z, lx.scale,
                         mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);

                // Parent-chain logging retired in v5: v2-v4 showed identity
                // world transforms on both parents for every merged shape.

                // v3 two-level chase (2026-07-03). v2 established the
                // BSMergeInstancedTriShape member picture:
                //   +0x170 ptr A -> per-shape buffer wrapper (q0 per-shape)
                //   +0x178 ptr B -> buffer wrapper whose q0 is the SAME heap
                //          object across all merged shapes (shared D3D pool)
                //   +0x190 hi-dword: power-of-two capacity that divides
                //          numTriangles exactly (e.g. 2-tri quad x 128,
                //          18 tris x 32) -> hardware instancing: small base
                //          piece replicated N times, per-instance transforms
                //          in a second stream the parser never reads.
                //   +0x1B8 constant 2 (stream count?), +0x1D0/+0x1D8 per-
                //          shape heap ptrs.
                // The v2 one-level float dump of A/B printed garbage because
                // the targets are WRAPPERS (first member = D3D buffer /
                // shared object), like BSGraphics::VertexBuffer whose pData
                // CPU copy the parser reads via pVB->pData. v3 dumps 8
                // qwords of each wrapper, then prints 24 floats behind each
                // heap-like member -- an instance-transform stream is
                // unmistakable (rotation values in [-1,1] interleaved with
                // world-range translations).
                {
                    uint64_t raw[16] = {};
                    if (PeekQwordsGuarded(
                            reinterpret_cast<const void*>(
                                reinterpret_cast<uintptr_t>(tri) + 0x160),
                            raw, 16)) {
                        _MESSAGE("FO4RemixPlugin: [InstDiag] #%d   raw+160=[%016llX %016llX %016llX %016llX "
                                 "%016llX %016llX %016llX %016llX]",
                                 n,
                                 (unsigned long long)raw[0], (unsigned long long)raw[1],
                                 (unsigned long long)raw[2], (unsigned long long)raw[3],
                                 (unsigned long long)raw[4], (unsigned long long)raw[5],
                                 (unsigned long long)raw[6], (unsigned long long)raw[7]);
                        _MESSAGE("FO4RemixPlugin: [InstDiag] #%d   raw+1A0=[%016llX %016llX %016llX %016llX "
                                 "%016llX %016llX %016llX %016llX]",
                                 n,
                                 (unsigned long long)raw[8],  (unsigned long long)raw[9],
                                 (unsigned long long)raw[10], (unsigned long long)raw[11],
                                 (unsigned long long)raw[12], (unsigned long long)raw[13],
                                 (unsigned long long)raw[14], (unsigned long long)raw[15]);

                        // Heap (not module/image) pointers only: this process
                        // allocates around 0x1D6..., images at 0x7FF7...
                        auto heapLike = [](uint64_t q) {
                            return q >= 0x100000000ULL &&
                                   q <  0x00007F0000000000ULL &&
                                   (q & 7ULL) == 0ULL;
                        };

                        // Wrappers of interest: A(+0x170)=raw[2], B(+0x178)=
                        // raw[3], and the per-shape unknowns +0x1D0/+0x1D8 =
                        // raw[14]/raw[15].
                        //
                        // v5: every heap-like member of each wrapper goes
                        // through InstDiagProbeMember -- d3d11 COM objects
                        // get QI + desc + bounded staging readback, heap
                        // objects get a 16KB SEH snapshot; both feed the
                        // strict record-run detector (see helpers above).
                        const float wpos[3] = { wx.pos.x, wx.pos.y, wx.pos.z };
                        const int wrapperIdx[4] = { 2, 3, 14, 15 };
                        for (int wi = 0; wi < 4; ++wi) {
                            const uint64_t wq = raw[wrapperIdx[wi]];
                            if (!heapLike(wq)) continue;
                            const int wrapOff = 0x160 + wrapperIdx[wi] * 8;
                            uint64_t wbody[8] = {};
                            if (!PeekQwordsGuarded(reinterpret_cast<const void*>(wq), wbody, 8)) {
                                continue;
                            }
                            _MESSAGE("FO4RemixPlugin: [InstDiag] #%d   wrap@+%X=%016llX "
                                     "q=[%016llX %016llX %016llX %016llX %016llX %016llX %016llX %016llX]",
                                     n, wrapOff, (unsigned long long)wq,
                                     (unsigned long long)wbody[0], (unsigned long long)wbody[1],
                                     (unsigned long long)wbody[2], (unsigned long long)wbody[3],
                                     (unsigned long long)wbody[4], (unsigned long long)wbody[5],
                                     (unsigned long long)wbody[6], (unsigned long long)wbody[7]);
                            for (int bi = 0; bi < 8; ++bi) {
                                if (!heapLike(wbody[bi])) continue;
                                uint64_t head[4] = {};
                                if (!PeekQwordsGuarded(reinterpret_cast<const void*>(wbody[bi]), head, 4)) {
                                    continue;
                                }
                                // Shared objects (the D3D pool, per-cell CPU
                                // structures) recur across shapes; probe each
                                // pointer once per session.
                                static std::unordered_set<uint64_t> sProbed;
                                if (!sProbed.insert(wbody[bi]).second) {
                                    _MESSAGE("FO4RemixPlugin: [InstDiag] #%d     +%X.q%d=%016llX seen",
                                             n, wrapOff, bi, (unsigned long long)wbody[bi]);
                                    continue;
                                }
                                InstDiagProbeMember(n, wrapOff, bi, wbody[bi], head, device, wpos);

                                // +0x1D0.q0 points at an array of 0xB8-strided
                                // descriptors (v4: *(q0)+0xB8 == q4 exactly,
                                // q1 == 4 == the +0x1A0 segment count). Chase
                                // one level into the element pool; logged as
                                // q8 to mark the synthetic slot.
                                if (wrapperIdx[wi] == 14 && bi == 0 && heapLike(head[0]) &&
                                    sProbed.insert(head[0]).second) {
                                    uint64_t ehead[2] = {};
                                    if (PeekQwordsGuarded(reinterpret_cast<const void*>(head[0]),
                                                          ehead, 2)) {
                                        InstDiagProbeMember(n, wrapOff, 8, head[0], ehead,
                                                            device, wpos);
                                    }
                                }
                            }
                        }
                    }
                }

                // NiExtraData entries. Leading qwords start at +0x18 (NiObject
                // 0x10 + BSFixedString m_name 8); precombined instance data
                // objects are large, so a 6-qword peek stays inside the
                // allocation for the entries we care about. (v1 result: NULL
                // on every merge-instanced shape -- kept for the MultiStream
                // variant, which has not been sampled yet.)
                tMutexArray<NiExtraData*>* xd = tri->m_extraData;
                if (xd && xd->entries && xd->count > 0) {
                    _MESSAGE("FO4RemixPlugin: [InstDiag] #%d   extraData count=%u", n, xd->count);
                    const UInt32 xdMax = xd->count < 6u ? xd->count : 6u;
                    for (UInt32 xi = 0; xi < xdMax; ++xi) {
                        NiExtraData* ed = xd->entries[xi];
                        if (!ed) continue;
                        char edLeaf[64] = "";
                        SemanticCapture::GetLeafClassName(reinterpret_cast<void*>(ed),
                                                          edLeaf, sizeof(edLeaf));
                        const uint64_t* q = reinterpret_cast<const uint64_t*>(
                            reinterpret_cast<uintptr_t>(ed) + 0x18);
                        _MESSAGE("FO4RemixPlugin: [InstDiag] #%d   xd[%u] class=%s name=\"%s\" "
                                 "q=[%016llX %016llX %016llX %016llX %016llX %016llX]",
                                 n, xi, edLeaf,
                                 ed->m_name.c_str() ? ed->m_name.c_str() : "",
                                 (unsigned long long)q[0], (unsigned long long)q[1],
                                 (unsigned long long)q[2], (unsigned long long)q[3],
                                 (unsigned long long)q[4], (unsigned long long)q[5]);
                    }
                }
            }
        }
    }

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

    // ---- Submit to Remix ----
    ResolverTrace::g_lastStep.store(Trace::kSubmitStart, std::memory_order_relaxed);
    auto status = RemixRenderer::SubmitDrawable(hash, mesh, newTextures);
    if (status != RemixRenderer::SubmitStatus::kSubmitted) {
        ResolverTrace::g_lastStep.store(Trace::kSubmitFailed, std::memory_order_relaxed);
        return false;
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
