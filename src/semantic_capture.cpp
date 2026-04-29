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
#include <mutex>
#include <unordered_map>

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

std::mutex g_drawableMutex;
std::unordered_map<PassKey, SemanticCapture::DrawableState> g_drawableMap;

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

// Shared detour body. The per-target wrappers above pass their compile-time
// resolver kind; the body uses it to tag the DrawableState and to call
// through the right original-fn pointer for the tail call.
static void* DetourGetRenderPassesShared(void* self,
                                         void* geometry,
                                         uint32_t technique,
                                         void* arg4,
                                         SemanticCapture::ResolverKind kind,
                                         GetRenderPasses_t original) {
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
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 4; ++c) {
                    state.liveWorldTransform[r][c] = liveXf[r][c];
                }
            }
            state.liveTransformValid = true;
        }
    }

    g_totalFires.fetch_add(1, std::memory_order_relaxed);
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
    g_installed.store(false);
    _MESSAGE("FO4RemixPlugin: [SemCapture] uninstalled (final fires: %llu)",
             (unsigned long long)g_totalFires.load());
}

void SemanticCapture::Tick(ID3D11Device* device) {
    if (!g_installed.load()) return;

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
            // Freshness gate: only resolve drawables the engine touched this
            // frame or last frame. Stale pointers from older entries cause
            // the parse_start AVs we've been catching -- skipping them avoids
            // dereferencing freed BSGeometry memory entirely. Mirrors the
            // uint64-safe age computation used by the eviction sweep below.
            const uint64_t age = (currentFrame > state.lastSeenFrame)
                ? (currentFrame - state.lastSeenFrame) : 0;
            if (age > 1) continue;

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
