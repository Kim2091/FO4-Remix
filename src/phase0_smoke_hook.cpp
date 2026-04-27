#include "phase0_smoke_hook.h"
#include "config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "f4se/PluginAPI.h"  // _MESSAGE
#include "MinHook.h"

#include <atomic>

namespace {

// FO4 Fallout4.exe RVA of BSLightingShaderProperty vtable slot 0x2B
// (the GetRenderPasses-equivalent). Verified by static disassembly +
// live-trace during Phase 0 RE (see
// docs/superpowers/research/2026-04-26-fo4-vtable-surface.md).
constexpr uintptr_t kCandidateSlotRVA = 0x02172540;

// FO4 GetRenderPasses-equivalent signature (inferred from disasm):
//   void* __fastcall(BSLightingShaderProperty* self,
//                    BSGeometry* geo,
//                    uint32_t technique,
//                    void* arg4)
// Returns the head of a freshly built BSRenderPass list (`[this+0x38]`).
// We declare 4 args so the x64 ABI preserves r8/r9 across the trampoline.
typedef void* (__fastcall *GetRenderPasses_t)(void* self,
                                              void* geometry,
                                              uint32_t technique,
                                              void* arg4);

GetRenderPasses_t g_originalGetRenderPasses = nullptr;
std::atomic<uint64_t> g_fireCount{0};
std::atomic<bool>     g_installed{false};
LPVOID                g_hookedTarget = nullptr;

void* __fastcall DetourGetRenderPasses(void* self,
                                       void* geometry,
                                       uint32_t technique,
                                       void* arg4) {
    const uint64_t count = g_fireCount.fetch_add(1, std::memory_order_relaxed) + 1;

    // Throttle log output: emit one line every 1000 fires so we can
    // observe the rate without flooding.
    if ((count % 1000) == 0) {
        _MESSAGE("FO4RemixPlugin: [Phase0Hook] GetRenderPasses fired %llu times (technique=0x%X)",
                 (unsigned long long)count, (unsigned int)technique);
    }

    return g_originalGetRenderPasses(self, geometry, technique, arg4);
}

}  // namespace

bool Phase0SmokeHook::Install() {
    if (!g_config.diagPhase0SmokeHook) {
        _MESSAGE("FO4RemixPlugin: [Phase0Hook] disabled by config");
        return false;
    }
    if (g_installed.load()) return true;

    HMODULE hMod = GetModuleHandleA("Fallout4.exe");
    if (!hMod) {
        _MESSAGE("FO4RemixPlugin: [Phase0Hook] GetModuleHandle(Fallout4.exe) failed");
        return false;
    }
    g_hookedTarget = reinterpret_cast<LPVOID>(reinterpret_cast<uintptr_t>(hMod) + kCandidateSlotRVA);

    MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
        _MESSAGE("FO4RemixPlugin: [Phase0Hook] MH_Initialize failed (%d)", (int)mhInit);
        return false;
    }
    if (MH_CreateHook(g_hookedTarget,
                      reinterpret_cast<LPVOID>(&DetourGetRenderPasses),
                      reinterpret_cast<LPVOID*>(&g_originalGetRenderPasses)) != MH_OK) {
        _MESSAGE("FO4RemixPlugin: [Phase0Hook] MH_CreateHook failed");
        return false;
    }
    if (MH_EnableHook(g_hookedTarget) != MH_OK) {
        _MESSAGE("FO4RemixPlugin: [Phase0Hook] MH_EnableHook failed");
        return false;
    }

    g_installed.store(true);
    _MESSAGE("FO4RemixPlugin: [Phase0Hook] installed at VA 0x%llX (RVA 0x%llX, module base 0x%llX)",
             (unsigned long long)reinterpret_cast<uintptr_t>(g_hookedTarget),
             (unsigned long long)kCandidateSlotRVA,
             (unsigned long long)reinterpret_cast<uintptr_t>(hMod));
    return true;
}

void Phase0SmokeHook::Uninstall() {
    if (!g_installed.load() || !g_hookedTarget) return;
    MH_DisableHook(g_hookedTarget);
    MH_RemoveHook(g_hookedTarget);
    g_installed.store(false);
    _MESSAGE("FO4RemixPlugin: [Phase0Hook] uninstalled (final fire count: %llu)",
             (unsigned long long)g_fireCount.load());
}
