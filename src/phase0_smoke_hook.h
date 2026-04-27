#pragma once

namespace Phase0SmokeHook {
    // Install the MinHook detour on the BSLightingShaderProperty
    // GetRenderPasses-equivalent slot identified during Phase 0 RE
    // (slot 0x2B at Fallout4.exe RVA 0x02172540). Resolved at install
    // time against the runtime module base so ASLR doesn't break us.
    // No-op if [Diagnostics] Phase0SmokeHook=0. Returns true on success.
    bool Install();

    // Remove the detour. Idempotent.
    void Uninstall();
}
