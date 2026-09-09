#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"

#include "config.h"
#include "crash_diag.h"
#include "draw_capture.h"
#include "present_hook.h"
#include "remix_api.h"
#include "remix_renderer.h"
#include "semantic_capture.h"
#include "startup_diag.h"

#include <cstring>
#include <atomic>
#include <exception>  // std::set_terminate (uncaught-exception breadcrumb)

static PluginHandle g_pluginHandle = kPluginHandle_Invalid;
static F4SEMessagingInterface* g_messaging = nullptr;

static bool g_hookInstalled = false;

// Signals that game forms are loaded and ready for scene extraction.
// Set by the F4SE GameDataReady message; read by present_hook.cpp.
std::atomic<bool> g_gameDataReady { false };

void TryInstallHook() {
    if (g_hookInstalled) return;
    _MESSAGE("FO4RemixPlugin: Attempting to install Present hook...");
    if (PresentHook::Install()) {
        _MESSAGE("FO4RemixPlugin: Present hook installed successfully");
        g_hookInstalled = true;
    } else {
        _MESSAGE("FO4RemixPlugin: ERROR - Failed to install Present hook");
    }
}

void OnF4SEMessage(F4SEMessagingInterface::Message* msg) {
    _MESSAGE("FO4RemixPlugin: Received F4SE message type=%d, dataLen=%d", msg->type, msg->dataLen);

    switch (msg->type) {
    case F4SEMessagingInterface::kMessage_GameDataReady:
        // data IS the readiness flag: F4SE dispatches (void*)isReady, false
        // BEFORE the form DB loads and true when it finishes
        // (Hooks_GameData.cpp). Treating the first (false) dispatch as
        // ready opened the extraction gate while LookupFormByID still
        // returned null -- exactly the mid-load window the gate exists for.
        // A false re-dispatch (data rebuild) closes the gate again.
        _MESSAGE("FO4RemixPlugin: GameDataReady (data=%p)", msg->data);
        g_gameDataReady = (msg->data != nullptr);
        TryInstallHook();
        break;
    case F4SEMessagingInterface::kMessage_GameLoaded:
        _MESSAGE("FO4RemixPlugin: GameLoaded");
        TryInstallHook();
        break;
    case F4SEMessagingInterface::kMessage_InputLoaded:
        _MESSAGE("FO4RemixPlugin: InputLoaded");
        TryInstallHook();
        break;
    case F4SEMessagingInterface::kMessage_PreLoadGame:
        _MESSAGE("FO4RemixPlugin: PreLoadGame - resetting extraction state");
        // Gate resolves BEFORE the wipe so no resolve loop races the
        // engine's teardown/rebuild of the destination world (mid-load
        // parse AVs). PostLoadGame lifts the gate.
        SemanticCapture::SetLoadingScreenActive(true);
        PresentHook::ResetExtractionState();
        // Drop every tracked drawable + release Remix handles. DrawableState
        // holds raw engine pointers only (no refs) -- the wipe exists so no
        // resolve or capture trusts pointers across the world swap.
        // Submission resumes naturally as hooks fire for the new world.
        SemanticCapture::ClearDrawableMap();
        // Captured merge-shape draws index into the engine's shared
        // geometry pools, which the destination world repacks: a capture
        // served across a reload slices the wrong pool region (run-3's
        // progressive corruption per save reload). Drop every watch.
        DrawCapture::ResetAll();
        // The release wave above parked a pile of Remix handles; the load
        // screen is the safe window to actually destroy them (see
        // DeferHandleDestroyToLoad).
        RemixRenderer::RequestDestroyDrain();
        break;
    case F4SEMessagingInterface::kMessage_NewGame:
        // New Game is a world swap too, but F4SE dispatches it INSTEAD of
        // PreLoadGame/PostLoadGame (LoadGame_Hook never runs), so without
        // this case the entire reset wave was skipped: stale captured pool
        // offsets served across the swap were exactly run-3's progressive
        // geometry corruption. Same wave as PreLoadGame EXCEPT the resolve
        // gate: nothing would ever lift it (PostLoadGame doesn't fire for
        // New Game) and a stuck gate blanks extraction for the ~60s
        // failsafe window.
        _MESSAGE("FO4RemixPlugin: NewGame - resetting extraction state");
        PresentHook::ResetExtractionState();
        SemanticCapture::ClearDrawableMap();
        DrawCapture::ResetAll();
        RemixRenderer::RequestDestroyDrain();
        break;
    case F4SEMessagingInterface::kMessage_PostLoadGame:
        // data is a bool: true = load succeeded, false = load failed/aborted.
        // Lift the gate either way -- whatever world is now active is stable.
        _MESSAGE("FO4RemixPlugin: PostLoadGame (success=%d) - resuming resolves",
                 msg->data ? 1 : 0);
        SemanticCapture::SetLoadingScreenActive(false);
        break;
    }
}

extern "C" {

__declspec(dllexport) F4SEPluginVersionData F4SEPlugin_Version = {
    F4SEPluginVersionData::kVersion,

    1,                          // plugin version
    "FO4RemixPlugin",           // plugin name
    "FO4Remix",                 // author

    F4SEPluginVersionData::kAddressIndependence_AddressLibrary_1_10_980
        | F4SEPluginVersionData::kAddressIndependence_AddressLibrary_1_11_137,
    F4SEPluginVersionData::kStructureIndependence_1_10_980Layout
        | F4SEPluginVersionData::kStructureIndependence_1_11_137Layout,
    { RUNTIME_VERSION_1_10_163, RUNTIME_VERSION_1_10_980, RUNTIME_VERSION_1_11_191, 0 },

    0,  // minimum F4SE version
    0, 0, {}  // reserved
};

__declspec(dllexport) bool F4SEPlugin_Load(const F4SEInterface* f4se) {
    g_pluginHandle = f4se->GetPluginHandle();

    _MESSAGE("FO4RemixPlugin: Loading (F4SE v%d, runtime v%08X)",
             f4se->f4seVersion, f4se->runtimeVersion);

    LoadConfig();
    StartupDiag::DumpEnvironment();

    // Interface checks FIRST: returning false makes F4SE FreeLibrary this
    // DLL (PluginManager.cpp), so every process-global install below --
    // set_terminate, the vectored handler, the MinHook exit detours, the
    // engine hooks -- would be left pointing into unmapped memory, and the
    // next exception dispatch or process exit would jump into freed pages.
    // Nothing irreversible may precede this block.
    g_messaging = static_cast<F4SEMessagingInterface*>(
        f4se->QueryInterface(kInterface_Messaging));
    if (!g_messaging) {
        _MESSAGE("FO4RemixPlugin: ERROR - Could not get messaging interface");
        return false;
    }

    // Uncaught-C++-exception breadcrumb (2026-07-12). WER events from the
    // 0xc0000409 fast-fail class resolve to abort() in THIS module's static
    // CRT (linker map): some thread's uncaught exception reached
    // std::terminate, and the default handler dies without saying which
    // thread or what it was unwinding. abort() runs through this module's
    // CRT precisely because the throw traversed our EH frames, so OUR
    // set_terminate handler is the one invoked: log the thread, write an
    // all-thread-stacks dump, then die as before.
    std::set_terminate([]() {
        _MESSAGE("FO4RemixPlugin: [Terminate] uncaught C++ exception on "
                 "thread %lu -- writing diagnostic dump", GetCurrentThreadId());
        RemixRenderer::WriteDiagDump("terminate");
        std::abort();
    });

    // Breadcrumbs for the deaths that leave NOTHING (no WER, no terminate,
    // log just stops): stack overflow / heap corruption via a vectored
    // handler, and deliberate ExitProcess/TerminateProcess calls with the
    // caller identified. See crash_diag.h for the full ladder.
    CrashDiag::Install();

    // Phase 1A: install BSLightingShaderProperty event-capture hook if
    // [SemanticCapture] Enabled=1. No-op when disabled (default).
    // Builds DrawableMap with TTL eviction; no Remix submission yet.
    SemanticCapture::Install();

    g_messaging->RegisterListener(g_pluginHandle, "F4SE", OnF4SEMessage);
    _MESSAGE("FO4RemixPlugin: Registered for F4SE messages");
    return true;
}

} // extern "C"
