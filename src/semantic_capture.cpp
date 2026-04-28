#include "semantic_capture.h"
#include "config.h"
#include "fo4_diagnostics.h"
#include "resolvers/lighting_static.h"
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

// -------- Hook target (verified by Phase 0) --------
constexpr uintptr_t kHookTargetRVA = 0x02172540;

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
GetRenderPasses_t     g_originalGetRenderPasses = nullptr;
LPVOID                g_hookedTarget            = nullptr;
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
        Resolvers::Lighting::TryResolveStatic(*state, key, device);
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

void* __fastcall DetourGetRenderPasses(void* self,
                                       void* geometry,
                                       uint32_t technique,
                                       void* arg4) {
    // Material lives at [self + 0x48] on FO4 (port delta from Skyrim's 0x180).
    // Read defensively: if self is null somehow, skip capture and call original.
    void* material = nullptr;
    if (self) {
        material = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(self) + 0x48);
    }

    const PassKey key = ComputePassKey(geometry, self, material);
    const uint64_t now = Diagnostics::CurrentFrameIndex();

    {
        std::lock_guard<std::mutex> lock(g_drawableMutex);
        auto& state = g_drawableMap[key];  // inserts default-constructed if new
        if (state.firstSeenFrame == 0) {
            state.firstSeenFrame = now;
            // Capture pointers once on first-seen; they're stable until eviction
            // (TTL = 10 frames). Resolver reads them on the Remix thread.
            state.geometry = geometry;
            state.property = self;
            state.material = material;
        }
        state.lastSeenFrame      = now;
        state.fireCount         += 1;
        state.lastTechniqueFlags = technique;
    }

    g_totalFires.fetch_add(1, std::memory_order_relaxed);
    return g_originalGetRenderPasses(self, geometry, technique, arg4);
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
    g_hookedTarget = reinterpret_cast<LPVOID>(
        reinterpret_cast<uintptr_t>(hMod) + kHookTargetRVA);

    MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
        _MESSAGE("FO4RemixPlugin: [SemCapture] ERROR: MH_Initialize failed (%d)", (int)mhInit);
        return false;
    }
    if (MH_CreateHook(g_hookedTarget,
                      reinterpret_cast<LPVOID>(&DetourGetRenderPasses),
                      reinterpret_cast<LPVOID*>(&g_originalGetRenderPasses)) != MH_OK) {
        _MESSAGE("FO4RemixPlugin: [SemCapture] ERROR: MH_CreateHook failed");
        return false;
    }
    if (MH_EnableHook(g_hookedTarget) != MH_OK) {
        _MESSAGE("FO4RemixPlugin: [SemCapture] ERROR: MH_EnableHook failed");
        return false;
    }

    g_installed.store(true);
    _MESSAGE("FO4RemixPlugin: [SemCapture] installed at VA 0x%llX (RVA 0x%llX)",
             (unsigned long long)reinterpret_cast<uintptr_t>(g_hookedTarget),
             (unsigned long long)kHookTargetRVA);
    return true;
}

void SemanticCapture::Uninstall() {
    if (!g_installed.load() || !g_hookedTarget) return;
    MH_DisableHook(g_hookedTarget);
    MH_RemoveHook(g_hookedTarget);
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
        Resolvers::Lighting::Trace::SetStep(Resolvers::Lighting::Trace::kSubmit_GateVram);
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
                const int lastStep = Resolvers::Lighting::Trace::LastStep();
                const uint64_t lastHash = Resolvers::Lighting::Trace::LastHash();
                _MESSAGE("FO4RemixPlugin: [Resolver] CRASH CAUGHT key=0x%llX "
                         "trace_hash=0x%llX step=%s exception=0x%08lX "
                         "geo=%p prop=%p mat=%p -- skipping permanently",
                         (unsigned long long)key,
                         (unsigned long long)lastHash,
                         Resolvers::Lighting::Trace::StepName(lastStep),
                         excCode,
                         state.geometry, state.property, state.material);
                state.submittedToRemix = true;
                state.meshHash = 0;
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
                if (it->second.submittedToRemix) ++submittedCount;
                else                              ++pendingCount;
                ++it;
            }
        }
        unique = g_drawableMap.size();
    }

    _MESSAGE("FO4RemixPlugin: [SemCapture] uniqueDrawables=%zu totalFires=%llu "
             "evictedThisSweep=%u submitted=%u pending=%u",
             unique,
             (unsigned long long)g_totalFires.load(std::memory_order_relaxed),
             evicted, submittedCount, pendingCount);
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
