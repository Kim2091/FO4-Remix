#include "semantic_capture.h"
#include "config.h"
#include "fo4_diagnostics.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "f4se/PluginAPI.h"  // _MESSAGE
#include "MinHook.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace {

// -------- Hook target (verified by Phase 0) --------
constexpr uintptr_t kHookTargetRVA = 0x02172540;

// -------- TTL + sweep cadence (Skyrim defaults) --------
constexpr uint64_t kTTLFrames           = 10;
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

void SemanticCapture::Tick() {
    if (!g_installed.load()) return;

    const uint32_t counter = g_sweepCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (counter < kSweepPeriodFrames) return;
    g_sweepCounter.store(0, std::memory_order_relaxed);

    const uint64_t now = Diagnostics::CurrentFrameIndex();
    uint32_t evicted = 0;
    size_t   unique  = 0;

    {
        std::lock_guard<std::mutex> lock(g_drawableMutex);
        for (auto it = g_drawableMap.begin(); it != g_drawableMap.end();) {
            const uint64_t age = (now > it->second.lastSeenFrame)
                ? (now - it->second.lastSeenFrame) : 0;
            if (age > kTTLFrames) {
                it = g_drawableMap.erase(it);
                ++evicted;
            } else {
                ++it;
            }
        }
        unique = g_drawableMap.size();
    }

    _MESSAGE("FO4RemixPlugin: [SemCapture] uniqueDrawables=%zu totalFires=%llu evictedThisSweep=%u",
             unique,
             (unsigned long long)g_totalFires.load(std::memory_order_relaxed),
             evicted);
}
