#include "semantic_capture.h"
#include "config.h"
#include "fo4_diagnostics.h"
#include "resolvers/lighting_static.h"
#include "resolvers/water.h"
#include "remix_renderer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "f4se/PluginAPI.h"  // _MESSAGE
#include "f4se/NiTypes.h"    // NiTransform, NiMatrix33, NiPoint3
#include "MinHook.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

// -------- TTL + sweep cadence (Skyrim defaults) --------
// Drawable eviction TTL. The engine does NOT re-fire GetRenderPasses every
// frame for cached static draws -- distant statics typically fire on
// visibility/cell-page-in events, not per-frame. A short TTL races that
// cadence: distant geometry submits once, then ages out and the mesh handle
// is destroyed before the engine re-fires for it, leaving the world looking
// "empty in the distance" after the first camera rotation.
//
// 18000 frames = 5 minutes at 60fps -- effectively unbounded for normal play
// but a backstop for multi-hour sessions so the SemCapture map and the
// Remix-side handle caches don't grow forever.
//
// TODO: replace with VRAM-pressured force-eviction (oldest lastDrawnFrame
// first) so the LRU material/texture sweeps actually have something to
// reclaim under memory pressure.
constexpr uint64_t kTTLFrames           = 18000;
constexpr uint32_t kSweepPeriodFrames   = 60;

// -------- PassKey: 64-bit FNV-1a hash of (geo*, prop*, mat*) --------
using PassKey = uint64_t;

PassKey ComputePassKey(const void* geo, const void* prop, const void* mat) {
    constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ULL;
    constexpr uint64_t kFnvPrime  = 0x100000001B3ULL;
    uint64_t h = kFnvOffset;
    const uintptr_t inputs[3] = {
        reinterpret_cast<uintptr_t>(geo),
        reinterpret_cast<uintptr_t>(prop),
        reinterpret_cast<uintptr_t>(mat),
    };
    for (int i = 0; i < 3; ++i) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&inputs[i]);
        for (int b = 0; b < 8; ++b) {
            h ^= p[b];
            h *= kFnvPrime;
        }
    }
    return h;
}

// -------- BuildRemixTransform -- shared by resolvers --------
// Defined here so resolvers link against semantic_capture.obj rather than
// pulling in the full bs_extraction.obj.

} // namespace (close anonymous so we define in SemanticCapture:: scope)

void SemanticCapture::BuildRemixTransform(const NiTransform& xf, float out[3][4]) {
    const float scale = xf.scale;
    for (int r = 0; r < 3; ++r) {
        out[r][0] = xf.rot.data[r][1] * scale;
        out[r][1] = xf.rot.data[r][0] * scale;
        out[r][2] = xf.rot.data[r][2] * scale;
    }
    out[0][3] = xf.pos.y;
    out[1][3] = xf.pos.x;
    out[2][3] = xf.pos.z;
}

namespace { // reopen anonymous namespace

// -------- Detour signature (from Phase 0 disasm + live trace) --------
typedef void* (__fastcall *GetRenderPasses_t)(void* self,
                                              void* geometry,
                                              uint32_t technique,
                                              void* arg4);

// -------- File-scope shared state --------
// Per-hook-target state. One entry per registered shader property class.
// Populated by Install(); read by detours to call the original via the
// matching original-fn pointer.
struct HookTarget {
    uintptr_t                rva;
    LPVOID                   address;        // hMod + rva, set in Install
    GetRenderPasses_t        original;       // populated by MH_CreateHook
    LPVOID                   detour;         // function pointer
    SemanticCapture::ResolverKind kind;
};

// Forward declarations -- actual detour functions defined below.
void* __fastcall DetourGetRenderPasses_Lighting(void* self, void* geometry,
                                                uint32_t technique, void* arg4);
void* __fastcall DetourGetRenderPasses_Water(void* self, void* geometry,
                                             uint32_t technique, void* arg4);

// Hook target registry. Static array, initialised in Install(); ordered so the
// detour functions can index into it by their compile-time constant.
constexpr size_t kHookTargetCount = 2;
static HookTarget g_hookTargets[kHookTargetCount] = {
    { 0x02172540, nullptr, nullptr,
      reinterpret_cast<LPVOID>(&DetourGetRenderPasses_Lighting),
      SemanticCapture::ResolverKind::Lighting },
    { 0x021D15A0, nullptr, nullptr,
      reinterpret_cast<LPVOID>(&DetourGetRenderPasses_Water),
      SemanticCapture::ResolverKind::Water },
};

std::atomic<bool>     g_installed{false};
std::atomic<uint64_t> g_totalFires{0};

// -------- Perf counters (cumulative; consumers diff across windows) --------
// fireNs measures ONLY our detour body, not the engine's original
// GetRenderPasses. tickNs measures the whole Tick (resolve loop + sweep).
// All relaxed: these are statistics, not synchronization.
std::atomic<uint64_t> g_fireNs{0};
std::atomic<uint64_t> g_tickCount{0};
std::atomic<uint64_t> g_tickNs{0};

std::mutex g_drawableMutex;
std::unordered_map<PassKey, SemanticCapture::DrawableState> g_drawableMap;

// Keys whose liveWorldTransform changed since the last DrainDirtyPoses.
// Guarded by g_drawableMutex; deduped via DrawableState::poseDirty.
std::vector<PassKey> g_dirtyPoses;

// -------- Sweep cadence counter (Tick() rate-limits via this) --------
std::atomic<uint32_t> g_sweepCounter{0};

// SEH wrapper for the resolver call. C++ destructors cannot live in a
// __try scope (MSVC C2712), so we isolate the call in a non-throwing helper
// with C-style locals only. Returns 0 on normal completion (resolver
// returned, success or not), 1 if SEH was caught (access violation etc).
// On SEH catch, *outExceptionCode receives GetExceptionCode().
static int CallResolverGuarded(SemanticCapture::DrawableState* state,
                               uint64_t key,
                               ID3D11Device* device,
                               unsigned long* outExceptionCode) {
    __try {
        switch (state->resolverKind) {
            case SemanticCapture::ResolverKind::Lighting:
                Resolvers::Lighting::TryResolveStatic(*state, key, device);
                break;
            case SemanticCapture::ResolverKind::Water:
                Resolvers::Water::TryResolve(*state, key, device);
                break;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outExceptionCode = GetExceptionCode();
        return 1;
    }
}

// SEH wrapper for ReleaseDrawable. ReleaseDrawable internally takes a
// std::lock_guard, so we cannot put __try inside it (C2712). Instead the
// caller wraps the entire call. Returns 0 on normal completion, 1 if SEH
// was caught with *outExceptionCode filled.
static int CallReleaseDrawableGuarded(uint64_t meshHash,
                                      unsigned long* outExceptionCode) {
    __try {
        RemixRenderer::ReleaseDrawable(meshHash);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outExceptionCode = GetExceptionCode();
        return 1;
    }
}

// -------- Parent-chain RTTI diagnostic --------
// Set in Install() once the module handle is resolved; read from the
// GetRenderPasses detour to walk vtable->COL->TypeDescriptor and recover
// MSVC mangled class names. kParentChainLogCap caps log volume; first N
// fires get a chain + matrix dump.
uintptr_t              g_moduleBase        = 0;
constexpr size_t       kParentChainDepth   = 6;
constexpr uint64_t     kParentChainLogCap  = 200;
std::atomic<uint64_t>  g_parentChainLogs{0};

// Walk geometry's NiAVObject::m_parent chain (offset +0x28) up to kDepth
// levels, recovering each level's class name via MSVC RTTI: vtable[-1] -> COL
// -> typedescriptor RVA -> module_base + RVA + 0x10 = mangled name. Strips
// the ".?AV"/".?AU" prefix and trims at "@@" so "BSTriShape" prints clean.
// Logs the leaf's m_worldTransform alongside the chain. SEH-guarded since
// every step dereferences an engine pointer that could race with scene-graph
// mutation; on a fault the chain entry stops and we keep what we got.
static int LogParentChainGuarded(uint64_t logN, void* geometry,
                                 uintptr_t modBase, unsigned long* outExc) {
    __try {
        char names[kParentChainDepth][96];
        for (size_t i = 0; i < kParentChainDepth; ++i) names[i][0] = '\0';

        void* obj = geometry;
        for (size_t i = 0; i < kParentChainDepth; ++i) {
            if (!obj) {
                continue;
            }
            void* vtable = *reinterpret_cast<void**>(obj);
            if (vtable) {
                void* col = *reinterpret_cast<void**>(
                    reinterpret_cast<uintptr_t>(vtable) - 8);
                if (col) {
                    uint32_t typeRva = *reinterpret_cast<uint32_t*>(
                        reinterpret_cast<uintptr_t>(col) + 0x0C);
                    const char* mangled = reinterpret_cast<const char*>(
                        modBase + typeRva + 0x10);
                    const char* p = mangled;
                    if (p[0] == '.' && p[1] == '?' && p[2] == 'A') p += 4;
                    size_t k = 0;
                    while (k < 95 && p[k] != '\0' && p[k] != '@') {
                        names[i][k] = p[k];
                        ++k;
                    }
                    names[i][k] = '\0';
                } else {
                    names[i][0] = '?'; names[i][1] = '\0';
                }
            } else {
                names[i][0] = '?'; names[i][1] = '\0';
            }
            obj = *reinterpret_cast<void**>(
                reinterpret_cast<uintptr_t>(obj) + 0x28);
        }

        const NiTransform* xf = reinterpret_cast<const NiTransform*>(
            reinterpret_cast<uintptr_t>(geometry) + 0x70);
        _MESSAGE("FO4RemixPlugin: [ParentChain] #%llu geom=%p "
                 "pos=(%.1f, %.1f, %.1f) "
                 "rot=[%.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f] s=%.4f "
                 "chain=[%s | %s | %s | %s | %s | %s]",
                 (unsigned long long)logN, geometry,
                 xf->pos.x, xf->pos.y, xf->pos.z,
                 xf->rot.data[0][0], xf->rot.data[0][1], xf->rot.data[0][2],
                 xf->rot.data[1][0], xf->rot.data[1][1], xf->rot.data[1][2],
                 xf->rot.data[2][0], xf->rot.data[2][1], xf->rot.data[2][2],
                 xf->scale,
                 names[0], names[1], names[2], names[3], names[4], names[5]);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outExc = GetExceptionCode();
        return 1;
    }
}

// SEH-guarded leaf class name lookup. Same vtable->COL->TypeDescriptor walk
// as the parent-chain logger, but for a single object. Writes empty string
// on null input, "?" on fault.
static int GetLeafClassNameGuarded(void* obj, uintptr_t modBase,
                                   char* out, size_t outSize) {
    __try {
        if (!obj) {
            out[0] = '\0';
            return 0;
        }
        void* vtable = *reinterpret_cast<void**>(obj);
        if (!vtable) {
            out[0] = '?'; out[1] = '\0';
            return 0;
        }
        void* col = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(vtable) - 8);
        if (!col) {
            out[0] = '?'; out[1] = '\0';
            return 0;
        }
        uint32_t typeRva = *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uintptr_t>(col) + 0x0C);
        const char* mangled = reinterpret_cast<const char*>(
            modBase + typeRva + 0x10);
        const char* p = mangled;
        if (p[0] == '.' && p[1] == '?' && p[2] == 'A') p += 4;
        size_t k = 0;
        while (k + 1 < outSize && p[k] != '\0' && p[k] != '@') {
            out[k] = p[k];
            ++k;
        }
        out[k] = '\0';
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outSize > 0) {
            out[0] = '?';
            if (outSize > 1) out[1] = '\0';
        }
        return 1;
    }
}

} // close anonymous namespace before exporting public function

void SemanticCapture::GetLeafClassName(void* obj, char* out, size_t outSize) {
    if (outSize == 0 || !out) return;
    out[0] = '\0';
    GetLeafClassNameGuarded(obj, g_moduleBase, out, outSize);
}

namespace { // reopen anonymous namespace

// Shared detour body. The per-target wrappers above pass their compile-time
// resolver kind; the body uses it to tag the DrawableState and to call
// through the right original-fn pointer for the tail call.
static void* DetourGetRenderPassesShared(void* self,
                                         void* geometry,
                                         uint32_t technique,
                                         void* arg4,
                                         SemanticCapture::ResolverKind kind,
                                         GetRenderPasses_t original) {
    const auto tFire0 = std::chrono::steady_clock::now();

    // Material lives at [self + 0x58] for ALL BSShaderProperty subclasses
    // (Lighting / Water / Effect) -- the slot is inherited from the base
    // class. F4SE NiProperties.h:108-136 confirms layout. Previous code
    // read +0x48 (which is BSFadeNode* pFadeNode); see findings doc
    // 2026-04-29-bswatershaderproperty-re.md for the bug analysis.
    void* material = nullptr;
    if (self) {
        material = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(self) + 0x58);
    }

    const PassKey key = ComputePassKey(geometry, self, material);
    const uint64_t now = Diagnostics::CurrentFrameIndex();

    // Snapshot live NiAVObject::flags (offset 0x108 -- verified against
    // f4se NiObjects.h). Safe to read here: engine is mid-call into
    // GetRenderPasses on this geometry, so the object is live.
    uint64_t niFlags = 0;
    void* p1 = nullptr;
    void* p2 = nullptr;
    // Live world transform (NiAVObject::m_worldTransform at offset 0x70 --
    // verified against f4se NiObjects.h). Read at hook time because the
    // engine has already evaluated scene-graph controllers (animated
    // statics: doors, gates, etc.) and the leaf BSGeometry's transform
    // reflects the current pose. Converted to Remix row-major 3x4 via
    // BuildRemixTransform (Beth X/Y swap built in).
    float liveXf[3][4] = {};
    bool  liveXfValid  = false;
    float capturedPosX = 0.0f;
    float capturedPosY = 0.0f;
    if (geometry) {
        niFlags = *reinterpret_cast<uint64_t*>(
            reinterpret_cast<uintptr_t>(geometry) + 0x108);
        // Walk the parent chain two levels (NiAVObject::m_parent at +0x28).
        // Used for the up-close-overlap diagnostic -- the LOD-or-similar
        // marker may sit on a grouping parent rather than the leaf shape.
        p1 = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(geometry) + 0x28);
        if (p1) {
            p2 = *reinterpret_cast<void**>(
                reinterpret_cast<uintptr_t>(p1) + 0x28);
        }
        const NiTransform& worldXf = *reinterpret_cast<const NiTransform*>(
            reinterpret_cast<uintptr_t>(geometry) + 0x70);
        SemanticCapture::BuildRemixTransform(worldXf, liveXf);
        liveXfValid = true;
        capturedPosX = worldXf.pos.x;
        capturedPosY = worldXf.pos.y;
    }

    // Parent-chain RTTI diag (capped). One line per qualifying fire up to
    // kParentChainLogCap. Position filter skips player-attached meshes
    // (Pip-Boy, body parts, weapons) whose m_worldTransform has small
    // local-space coords -- those flood the budget during loading and char
    // creation before the user reaches the dock area we want to inspect.
    constexpr float kFarFromOriginThreshold = 5000.0f;
    const bool farFromOrigin =
        capturedPosX >  kFarFromOriginThreshold ||
        capturedPosX < -kFarFromOriginThreshold ||
        capturedPosY >  kFarFromOriginThreshold ||
        capturedPosY < -kFarFromOriginThreshold;
    if (geometry && g_moduleBase && farFromOrigin &&
        g_parentChainLogs.load(std::memory_order_relaxed) < kParentChainLogCap) {
        const uint64_t logN = g_parentChainLogs.fetch_add(1, std::memory_order_relaxed);
        // Re-check after the increment: two threads can pass the pre-check
        // simultaneously; only tickets below the cap may log.
        if (logN < kParentChainLogCap) {
            unsigned long exc = 0;
            LogParentChainGuarded(logN, geometry, g_moduleBase, &exc);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_drawableMutex);
        auto& state = g_drawableMap[key];  // inserts default-constructed if new
        if (state.firstSeenFrame == 0) {
            state.firstSeenFrame = now;
            state.geometry = geometry;
            state.property = self;
            state.material = material;
            state.initialFlags = niFlags;
            state.parent1 = p1;
            state.parent2 = p2;
            state.resolverKind = kind;  // tag once on first-seen
        }
        state.lastSeenFrame      = now;
        state.lastFlags          = niFlags;
        state.fireCount         += 1;
        state.lastTechniqueFlags = technique;
        if (liveXfValid) {
            // Store + queue only on actual change (bitwise). Static geometry
            // re-fires with an identical transform and costs one memcmp;
            // animating objects (doors, gates) differ every frame and get
            // queued for OnFrame's DrainDirtyPoses.
            const bool changed = !state.liveTransformValid ||
                std::memcmp(state.liveWorldTransform, liveXf, sizeof(liveXf)) != 0;
            if (changed) {
                std::memcpy(state.liveWorldTransform, liveXf, sizeof(liveXf));
                state.liveTransformValid = true;
                if (!state.poseDirty) {
                    state.poseDirty = true;
                    g_dirtyPoses.push_back(key);
                }
            }
        }
    }

    g_totalFires.fetch_add(1, std::memory_order_relaxed);
    g_fireNs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - tFire0).count(), std::memory_order_relaxed);
    return original(self, geometry, technique, arg4);
}

// Per-target wrappers. Each captures its kind as a compile-time constant
// and calls through with the matching original-fn pointer from g_hookTargets.
void* __fastcall DetourGetRenderPasses_Lighting(void* self, void* geometry,
                                                uint32_t technique, void* arg4) {
    return DetourGetRenderPassesShared(self, geometry, technique, arg4,
                                       SemanticCapture::ResolverKind::Lighting,
                                       g_hookTargets[0].original);
}

void* __fastcall DetourGetRenderPasses_Water(void* self, void* geometry,
                                             uint32_t technique, void* arg4) {
    static std::atomic<uint64_t> sFireCount{0};
    const uint64_t n = sFireCount.fetch_add(1, std::memory_order_relaxed);
    if (n < 10) {
        _MESSAGE("FO4RemixPlugin: [DetourWater] fire #%llu self=%p geom=%p",
                 (unsigned long long)n, self, geometry);
    }
    return DetourGetRenderPassesShared(self, geometry, technique, arg4,
                                       SemanticCapture::ResolverKind::Water,
                                       g_hookTargets[1].original);
}

// -------- Diagnostic hooks: rotation-bug investigation --------
// SetupGeo hook (RVA 0x02233730) = BSLightingShader::SetupGeometry; reads
// pass->geometry (rdx+0x18) and that geometry's m_worldTransform (+0x70).
// CBWrite hook (RVA 0x022347D0) = the per-instance constant-buffer writer
// SetupGeometry calls with the world transform it's about to upload. The
// matrix here is the last view of NiTransform before the engine transposes
// it row-major -> column-major, applies camera-relative shift, and writes
// to the GPU vertex CB. Comparing both against GetRenderPasses' capture
// tells us whether anything composes onto the matrix between the two.
// Logging is capped per-hook to keep the F4SE log readable.

typedef void (__fastcall *SetupGeometry_t)(void* self, void* pass, uint32_t flags);
typedef void (__fastcall *WriteWorldXform_t)(void* state, void* xform);

constexpr uintptr_t kSetupGeometryRVA   = 0x02233730;
constexpr uintptr_t kWriteWorldXformRVA = 0x022347D0;
constexpr uint64_t  kDiagLogCap         = 200;

LPVOID                g_addrSetupGeo     = nullptr;
LPVOID                g_addrWriteXform   = nullptr;
SetupGeometry_t       g_origSetupGeo     = nullptr;
WriteWorldXform_t     g_origWriteXform   = nullptr;
std::atomic<uint64_t> g_setupGeoFires{0};
std::atomic<uint64_t> g_writeXformFires{0};

static int LogSetupGeoGuarded(uint64_t n, void* pass, uint32_t flags,
                              unsigned long* outExc) {
    __try {
        void* geometry = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pass) + 0x18);
        void* property = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pass) + 0x10);
        if (!geometry) return 0;
        const NiTransform* p = reinterpret_cast<const NiTransform*>(
            reinterpret_cast<uintptr_t>(geometry) + 0x70);
        _MESSAGE("FO4RemixPlugin: [SetupGeo] #%llu geom=%p prop=%p flags=0x%x "
                 "pos=(%.3f, %.3f, %.3f) "
                 "rot=[%.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f] s=%.4f",
                 (unsigned long long)n, geometry, property, flags,
                 p->pos.x, p->pos.y, p->pos.z,
                 p->rot.data[0][0], p->rot.data[0][1], p->rot.data[0][2],
                 p->rot.data[1][0], p->rot.data[1][1], p->rot.data[1][2],
                 p->rot.data[2][0], p->rot.data[2][1], p->rot.data[2][2],
                 p->scale);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outExc = GetExceptionCode();
        return 1;
    }
}

static int LogWriteXformGuarded(uint64_t n, void* xform, unsigned long* outExc) {
    __try {
        if (!xform) return 0;
        const NiTransform* p = reinterpret_cast<const NiTransform*>(xform);
        _MESSAGE("FO4RemixPlugin: [CBWrite] #%llu xform=%p "
                 "pos=(%.3f, %.3f, %.3f) "
                 "rot=[%.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f] s=%.4f",
                 (unsigned long long)n, xform,
                 p->pos.x, p->pos.y, p->pos.z,
                 p->rot.data[0][0], p->rot.data[0][1], p->rot.data[0][2],
                 p->rot.data[1][0], p->rot.data[1][1], p->rot.data[1][2],
                 p->rot.data[2][0], p->rot.data[2][1], p->rot.data[2][2],
                 p->scale);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outExc = GetExceptionCode();
        return 1;
    }
}

void __fastcall DetourSetupGeometry_Lighting(void* self, void* pass, uint32_t flags) {
    const uint64_t n = g_setupGeoFires.fetch_add(1, std::memory_order_relaxed);
    if (n < kDiagLogCap && pass) {
        unsigned long exc = 0;
        LogSetupGeoGuarded(n, pass, flags, &exc);
    }
    g_origSetupGeo(self, pass, flags);
}

void __fastcall DetourWriteWorldXform(void* state, void* xform) {
    const uint64_t n = g_writeXformFires.fetch_add(1, std::memory_order_relaxed);
    if (n < kDiagLogCap && xform) {
        unsigned long exc = 0;
        LogWriteXformGuarded(n, xform, &exc);
    }
    g_origWriteXform(state, xform);
}

}  // namespace

bool SemanticCapture::Install() {
    if (!g_config.semanticCaptureEnabled) {
        _MESSAGE("FO4RemixPlugin: [SemCapture] disabled by config");
        return false;
    }
    if (g_installed.load()) return true;

    HMODULE hMod = GetModuleHandleA("Fallout4.exe");
    if (!hMod) {
        _MESSAGE("FO4RemixPlugin: [SemCapture] ERROR: GetModuleHandle(Fallout4.exe) failed");
        return false;
    }
    g_moduleBase = reinterpret_cast<uintptr_t>(hMod);

    MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
        _MESSAGE("FO4RemixPlugin: [SemCapture] ERROR: MH_Initialize failed (%d)", (int)mhInit);
        return false;
    }

    size_t installedCount = 0;
    for (size_t i = 0; i < kHookTargetCount; ++i) {
        HookTarget& t = g_hookTargets[i];
        t.address = reinterpret_cast<LPVOID>(
            reinterpret_cast<uintptr_t>(hMod) + t.rva);
        if (MH_CreateHook(t.address, t.detour,
                          reinterpret_cast<LPVOID*>(&t.original)) != MH_OK) {
            _MESSAGE("FO4RemixPlugin: [SemCapture] ERROR: MH_CreateHook failed for target %zu (RVA 0x%llX)",
                     i, (unsigned long long)t.rva);
            continue;
        }
        if (MH_EnableHook(t.address) != MH_OK) {
            _MESSAGE("FO4RemixPlugin: [SemCapture] ERROR: MH_EnableHook failed for target %zu (RVA 0x%llX)",
                     i, (unsigned long long)t.rva);
            // Clean up the trampoline allocated by MH_CreateHook above.
            // Uninstall would handle this via the t.address gate, but doing
            // it locally keeps the partial-install state explicit and lets
            // a retried Install() see this slot as not-yet-created.
            MH_RemoveHook(t.address);
            t.address = nullptr;
            continue;
        }
        ++installedCount;
        _MESSAGE("FO4RemixPlugin: [SemCapture] installed target %zu kind=%d at VA 0x%llX (RVA 0x%llX)",
                 i, (int)t.kind,
                 (unsigned long long)reinterpret_cast<uintptr_t>(t.address),
                 (unsigned long long)t.rva);
    }

    if (installedCount == 0) {
        _MESSAGE("FO4RemixPlugin: [SemCapture] FATAL: zero hooks installed; aborting");
        return false;
    }

    // Diagnostic hooks (best-effort; never abort the install path).
    g_addrSetupGeo = reinterpret_cast<LPVOID>(
        reinterpret_cast<uintptr_t>(hMod) + kSetupGeometryRVA);
    if (MH_CreateHook(g_addrSetupGeo,
                      reinterpret_cast<LPVOID>(&DetourSetupGeometry_Lighting),
                      reinterpret_cast<LPVOID*>(&g_origSetupGeo)) != MH_OK) {
        _MESSAGE("FO4RemixPlugin: [SemCapture] WARN: SetupGeo create failed (RVA 0x%llX)",
                 (unsigned long long)kSetupGeometryRVA);
        g_addrSetupGeo = nullptr;
    } else if (MH_EnableHook(g_addrSetupGeo) != MH_OK) {
        _MESSAGE("FO4RemixPlugin: [SemCapture] WARN: SetupGeo enable failed");
        MH_RemoveHook(g_addrSetupGeo);
        g_addrSetupGeo = nullptr;
    } else {
        _MESSAGE("FO4RemixPlugin: [SemCapture] installed SetupGeo diag hook at RVA 0x%llX",
                 (unsigned long long)kSetupGeometryRVA);
    }

    g_addrWriteXform = reinterpret_cast<LPVOID>(
        reinterpret_cast<uintptr_t>(hMod) + kWriteWorldXformRVA);
    if (MH_CreateHook(g_addrWriteXform,
                      reinterpret_cast<LPVOID>(&DetourWriteWorldXform),
                      reinterpret_cast<LPVOID*>(&g_origWriteXform)) != MH_OK) {
        _MESSAGE("FO4RemixPlugin: [SemCapture] WARN: CBWrite create failed (RVA 0x%llX)",
                 (unsigned long long)kWriteWorldXformRVA);
        g_addrWriteXform = nullptr;
    } else if (MH_EnableHook(g_addrWriteXform) != MH_OK) {
        _MESSAGE("FO4RemixPlugin: [SemCapture] WARN: CBWrite enable failed");
        MH_RemoveHook(g_addrWriteXform);
        g_addrWriteXform = nullptr;
    } else {
        _MESSAGE("FO4RemixPlugin: [SemCapture] installed CBWrite diag hook at RVA 0x%llX",
                 (unsigned long long)kWriteWorldXformRVA);
    }

    g_installed.store(true);
    return true;
}

void SemanticCapture::Uninstall() {
    if (!g_installed.load()) return;
    for (size_t i = 0; i < kHookTargetCount; ++i) {
        HookTarget& t = g_hookTargets[i];
        if (!t.address) continue;
        MH_DisableHook(t.address);
        MH_RemoveHook(t.address);
    }
    if (g_addrSetupGeo) {
        MH_DisableHook(g_addrSetupGeo);
        MH_RemoveHook(g_addrSetupGeo);
        g_addrSetupGeo = nullptr;
    }
    if (g_addrWriteXform) {
        MH_DisableHook(g_addrWriteXform);
        MH_RemoveHook(g_addrWriteXform);
        g_addrWriteXform = nullptr;
    }
    g_installed.store(false);
    _MESSAGE("FO4RemixPlugin: [SemCapture] uninstalled (final fires: %llu, "
             "setupGeo=%llu cbWrite=%llu)",
             (unsigned long long)g_totalFires.load(),
             (unsigned long long)g_setupGeoFires.load(),
             (unsigned long long)g_writeXformFires.load());
}

void SemanticCapture::Tick(ID3D11Device* device) {
    if (!g_installed.load()) return;

    // RAII so every return path accumulates. tickCount also serves as the
    // game-frame counter for normalizing fires/frame (Tick runs once per
    // hkPresent).
    g_tickCount.fetch_add(1, std::memory_order_relaxed);
    struct TickNsGuard {
        std::chrono::steady_clock::time_point t0;
        ~TickNsGuard() {
            g_tickNs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed);
        }
    } tickGuard{ std::chrono::steady_clock::now() };

    // Note: g_sweepCounter (atomic, relaxed) interleaves with g_drawableMutex
    // (mutex). Single-caller invariant holds today (only hkPresent calls Tick).
    // If multiple callers ever appeared, the sweep cadence would jitter by a
    // frame but no data race occurs; the map access is mutex-serialized.

    // Single read of the frame index, shared by the resolve-loop freshness
    // gate and the eviction sweep below. Keeps the two reads consistent.
    const uint64_t currentFrame = Diagnostics::CurrentFrameIndex();

    // VRAM gate: skip resolves if we're close to the device budget. dxvk-remix
    // is observed to throw C++ exceptions out of CreateX calls under VRAM
    // pressure, and those throws corrupt its internal Vulkan command queue.
    // Conservative threshold: skip if used > 90% of driver budget.
    //
    // Eviction below this gate STILL runs -- eviction frees handles, which
    // is exactly what we want when VRAM is tight.
    bool vramOk = true;
    {
        RemixRenderer::VramStats vs{};
        if (RemixRenderer::GetVramStats(&vs) && vs.driverBudgetBytes > 0) {
            const uint64_t threshold = (vs.driverBudgetBytes * 9) / 10;  // 90%
            if (vs.totalAllocatedBytes > threshold) {
                vramOk = false;
            }
        }
    }

    // ---- Resolve loop: every call, attempt one resolve per unsubmitted drawable ----
    // Skyrim's pattern: cheap when state.submittedToRemix is true (early exit).
    if (!vramOk) {
        Resolvers::Trace::SetStep(Resolvers::Trace::kSubmit_GateVram);
        // Skip resolve loop; eviction sweep below still runs.
    } else {
        // Per-Tick submission budget removed: VRAM gate above and input
        // validation in SubmitDrawable are now the load-bearing protection
        // against the dxvk-remix freeze. The budget cap (was 4) starved
        // streaming during normal play (proven by the Pip-Boy-only mesh-
        // load symptom). If the freeze recurs without those gates being
        // sufficient, reintroduce a much higher cap (~64) here.

        std::lock_guard<std::mutex> lock(g_drawableMutex);
        for (auto& [key, state] : g_drawableMap) {
            if (state.submittedToRemix) continue;
            // Freshness gate (widened 2026-07-02, was age > 1). The engine
            // fires GetRenderPasses roughly once per cell ATTACH -- passes
            // are cached afterwards. Loading a save into a different area
            // fires the destination cell's geometry DURING the load screen,
            // when extraction fails (texture rendererData/resources not
            // backed yet); with a 2-frame window those drawables aged out
            // before becoming resolvable and, since the engine never
            // re-fires cached passes, the player's own cell stayed
            // permanently empty while later-attaching neighbor cells
            // appeared (user-reported symptom). The async texture readback
            // (Pending on first attempt) needs the wider window for the
            // same reason.
            //
            // Pointer-safety: the old tight gate was a pre-filter against
            // dereferencing freed BSGeometry (parse_start AVs). Entries
            // whose cell detaches inside the window are now caught by the
            // CallResolverGuarded SEH backstop below and marked permanently
            // skipped -- bounded risk, and the window is ini-tunable
            // ([SemanticCapture] ResolveRetryWindowFrames).
            const uint64_t age = (currentFrame > state.lastSeenFrame)
                ? (currentFrame - state.lastSeenFrame) : 0;
            if (age > g_config.resolveRetryWindowFrames) continue;

            // Note: the resolver may take a few hundred microseconds for
            // texture readbacks. We hold the mutex during the call, which
            // serializes resolver work with hot-path captures. If profile
            // shows hot-path back-pressure, move this loop to a snapshot+
            // unlock+resolve+lock-to-update pattern. Default first.
            //
            // SEH-guarded: stale geometry pointers from TTL-aged entries
            // can dereference freed memory. Catch the AV, log the failing
            // drawable + last resolver step, and mark the entry as
            // submitted so we don't retry it endlessly.
            unsigned long excCode = 0;
            if (CallResolverGuarded(&state, key, device, &excCode) != 0) {
                const int lastStep = Resolvers::Trace::LastStep();
                const uint64_t lastHash = Resolvers::Trace::LastHash();
                _MESSAGE("FO4RemixPlugin: [Resolver] CRASH CAUGHT key=0x%llX "
                         "trace_hash=0x%llX step=%s exception=0x%08lX "
                         "geo=%p prop=%p mat=%p -- skipping permanently",
                         (unsigned long long)key,
                         (unsigned long long)lastHash,
                         Resolvers::Trace::StepName(lastStep),
                         excCode,
                         state.geometry, state.property, state.material);
                state.submittedToRemix = true;
                state.meshHash = 0;
            }

            // Diagnostic: log every water entry's post-resolver state so we
            // can pinpoint exactly where it exits.
            if (state.resolverKind == SemanticCapture::ResolverKind::Water) {
                static std::atomic<uint64_t> sWaterExitCnt{0};
                const uint64_t en = sWaterExitCnt.fetch_add(1, std::memory_order_relaxed);
                if (en < 15) {
                    _MESSAGE("FO4RemixPlugin: [WaterExit] #%llu key=0x%llX submitted=%d step=%s excCode=0x%08lX",
                             (unsigned long long)en,
                             (unsigned long long)key,
                             state.submittedToRemix ? 1 : 0,
                             Resolvers::Trace::StepName(Resolvers::Trace::LastStep()),
                             excCode);
                }
            }

            // Snapshot the gate that rejected this drawable so the periodic
            // stats can break `pending` down by reason. Successful submits and
            // SEH-caught crashes both flip submittedToRemix=true and bypass.
            if (!state.submittedToRemix) {
                state.lastFailedResolverStep =
                    Resolvers::Trace::LastStep();
            }
        }
    }

    // ---- Sweep cadence: every kSweepPeriodFrames calls ----
    const uint32_t counter = g_sweepCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (counter < kSweepPeriodFrames) return;
    g_sweepCounter.store(0, std::memory_order_relaxed);

    const uint64_t now = currentFrame;
    uint32_t evicted = 0;
    uint32_t submittedCount = 0;
    uint32_t pendingCount = 0;
    size_t   unique  = 0;
    size_t   distinctGeoPtrs = 0;
    size_t   entriesWithGeo = 0;

    // Per-gate breakdown of pending entries. Mirrors the resolver's
    // Trace::Step enum -- each non-submitted drawable lands in exactly one
    // bucket according to its lastFailedResolverStep.
    uint32_t pendNotResolved   = 0;  // step==kIdle (resolver freshness gate skipped it)
    uint32_t pendNotTriShape   = 0;  // step==kEntered (cast or BSTriShape check failed)
    uint32_t pendSkinned       = 0;  // step==kSkinSkipped
    uint32_t pendLod           = 0;  // step==kLODSkipped (kFlagIsMeshLOD filter)
    uint32_t pendParseFailed   = 0;  // step==kParseStart (ParseShapeGeometry returned false)
    uint32_t pendExtentReject  = 0;  // step==kExtentRejected
    uint32_t pendNoMaterial    = 0;  // step==kBuildMeshOK (mat fetch returned null)
    uint32_t pendLandscape     = 0;  // step==kLandscapeSkipped
    uint32_t pendNoDiffuse     = 0;  // step==kMaterialFetched (diffuseTextureHash==0)
    uint32_t pendSubmitFailed  = 0;  // step==kSubmitFailed
    uint32_t pendOther         = 0;

    {
        std::lock_guard<std::mutex> lock(g_drawableMutex);
        for (auto it = g_drawableMap.begin(); it != g_drawableMap.end();) {
            const uint64_t age = (now > it->second.lastSeenFrame)
                ? (now - it->second.lastSeenFrame) : 0;
            if (age > kTTLFrames) {
                if (it->second.submittedToRemix && it->second.meshHash != 0) {
                    unsigned long excCode = 0;
                    if (CallReleaseDrawableGuarded(it->second.meshHash, &excCode) != 0) {
                        _MESSAGE("FO4RemixPlugin: [Sweep] CRASH CAUGHT in ReleaseDrawable "
                                 "hash=0x%llX exception=0x%08lX -- continuing eviction; "
                                 "Remix-side handles may leak",
                                 (unsigned long long)it->second.meshHash, excCode);
                    }
                }
                it = g_drawableMap.erase(it);
                ++evicted;
            } else {
                if (it->second.submittedToRemix) {
                    ++submittedCount;
                } else {
                    ++pendingCount;
                    using Resolvers::Trace::Step;
                    switch (it->second.lastFailedResolverStep) {
                        case Step::kIdle:              ++pendNotResolved;  break;
                        case Step::kEntered:           ++pendNotTriShape;  break;
                        case Step::kSkinSkipped:       ++pendSkinned;      break;
                        case Step::kLODSkipped:        ++pendLod;          break;
                        case Step::kParseStart:        ++pendParseFailed;  break;
                        case Step::kExtentRejected:    ++pendExtentReject; break;
                        case Step::kBuildMeshOK:       ++pendNoMaterial;   break;
                        case Step::kLandscapeSkipped:  ++pendLandscape;    break;
                        case Step::kMaterialFetched:   ++pendNoDiffuse;    break;
                        case Step::kSubmitFailed:      ++pendSubmitFailed; break;
                        default:                       ++pendOther;        break;
                    }
                }
                ++it;
            }
        }
        unique = g_drawableMap.size();

        // Geometry-pointer dedup count: how many map entries share a
        // geometry pointer. >0 means the same BSGeometry is being submitted
        // under multiple PassKeys (different property/material variants),
        // which would render N times -- a candidate cause for the "two
        // versions of the same object" overlap symptom.
        std::unordered_set<void*> distinct;
        distinct.reserve(g_drawableMap.size());
        for (const auto& [k, st] : g_drawableMap) {
            if (st.geometry) {
                distinct.insert(st.geometry);
                ++entriesWithGeo;
            }
        }
        distinctGeoPtrs = distinct.size();
    }

    const size_t geoDups = (entriesWithGeo > distinctGeoPtrs)
        ? (entriesWithGeo - distinctGeoPtrs) : 0;
    _MESSAGE("FO4RemixPlugin: [SemCapture] uniqueDrawables=%zu totalFires=%llu "
             "evictedThisSweep=%u submitted=%u pending=%u "
             "distinctGeoPtrs=%zu entriesWithGeo=%zu geoDups=%zu",
             unique,
             (unsigned long long)g_totalFires.load(std::memory_order_relaxed),
             evicted, submittedCount, pendingCount,
             distinctGeoPtrs, entriesWithGeo, geoDups);

    _MESSAGE("FO4RemixPlugin: [SemCapture] pendingByGate "
             "notResolved=%u notTriShape=%u skinned=%u lod=%u "
             "parseFailed=%u extentRejected=%u noMaterial=%u "
             "landscape=%u noDiffuse=%u submitFailed=%u other=%u",
             pendNotResolved, pendNotTriShape, pendSkinned, pendLod,
             pendParseFailed, pendExtentReject, pendNoMaterial,
             pendLandscape, pendNoDiffuse, pendSubmitFailed, pendOther);
}

void SemanticCapture::ClearDrawableMap() {
    std::lock_guard<std::mutex> lock(g_drawableMutex);

    const size_t totalCount = g_drawableMap.size();
    size_t submittedCount = 0;

    // Release Remix-side handles for every submitted entry first. ~NiPointer
    // below will only release engine refs; it doesn't know about g_drawables
    // / g_meshCache / g_materialCache / g_textureHandles.
    for (auto& [key, state] : g_drawableMap) {
        if (state.submittedToRemix && state.meshHash != 0) {
            unsigned long excCode = 0;
            if (CallReleaseDrawableGuarded(state.meshHash, &excCode) != 0) {
                _MESSAGE("FO4RemixPlugin: [Reload] CRASH CAUGHT in ReleaseDrawable "
                         "hash=0x%llX exception=0x%08lX -- continuing wipe",
                         (unsigned long long)state.meshHash, excCode);
            }
            ++submittedCount;
        }
    }

    // ~DrawableState -> ~NiPointer -> DecRef on every entry. For drawables
    // where the engine's already released its refs (refcount == 1, just us),
    // the engine destructor runs inline here and frees the BSGeometry.
    g_drawableMap.clear();
    g_dirtyPoses.clear();

    _MESSAGE("FO4RemixPlugin: [Reload] cleared %zu drawables (%zu submitted) on PreLoadGame",
             totalCount, submittedCount);
}

void SemanticCapture::SnapshotActiveDrawables(uint64_t currentFrame,
                                              uint64_t maxAge,
                                              std::unordered_set<uint64_t>& out,
                                              ActiveFlagStats* stats,
                                              std::unordered_map<uint64_t, std::array<float, 12>>* livePoses) {
    std::lock_guard<std::mutex> lock(g_drawableMutex);
    out.reserve(g_drawableMap.size());
    if (livePoses) livePoses->reserve(g_drawableMap.size());
    for (const auto& [hash, state] : g_drawableMap) {
        if (!state.submittedToRemix) continue;
        const uint64_t age = (currentFrame > state.lastSeenFrame)
            ? (currentFrame - state.lastSeenFrame) : 0;
        if (age > maxAge) continue;
        out.insert(hash);
        if (livePoses && state.liveTransformValid) {
            std::array<float, 12> pose;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 4; ++c) {
                    pose[r * 4 + c] = state.liveWorldTransform[r][c];
                }
            }
            livePoses->emplace(hash, pose);
        }
        if (stats) {
            const uint64_t f = state.lastFlags;
            ++stats->total;
            if (f & (1ULL << 12)) ++stats->isLod;
            if (f & (1ULL << 37)) ++stats->fadedIn;
            if (f & (1ULL << 39)) ++stats->notVisible;
            if (f & (1ULL << 36)) ++stats->lodFadingOut;
            if (f & (1ULL << 38)) ++stats->forcedFadeOut;
        }
    }
}

void SemanticCapture::DrainDirtyPoses(std::unordered_map<uint64_t, std::array<float, 12>>& out) {
    std::lock_guard<std::mutex> lock(g_drawableMutex);
    for (const PassKey key : g_dirtyPoses) {
        auto it = g_drawableMap.find(key);
        if (it == g_drawableMap.end()) continue;  // evicted while queued
        DrawableState& state = it->second;
        state.poseDirty = false;
        std::array<float, 12> pose;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                pose[r * 4 + c] = state.liveWorldTransform[r][c];
            }
        }
        out.emplace(key, pose);
    }
    g_dirtyPoses.clear();
}

SemanticCapture::PerfCounters SemanticCapture::GetPerfCounters() {
    PerfCounters c;
    c.fires  = g_totalFires.load(std::memory_order_relaxed);
    c.fireNs = g_fireNs.load(std::memory_order_relaxed);
    c.ticks  = g_tickCount.load(std::memory_order_relaxed);
    c.tickNs = g_tickNs.load(std::memory_order_relaxed);
    return c;
}
