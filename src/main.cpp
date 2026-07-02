#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"

#include "config.h"
#include "present_hook.h"
#include "remix_api.h"
#include "remix_renderer.h"
#include "semantic_capture.h"
#include "startup_diag.h"

#include <cstring>
#include <atomic>

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
        _MESSAGE("FO4RemixPlugin: GameDataReady (data=%p)", msg->data);
        g_gameDataReady = true;
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
        // Drop every tracked drawable + release Remix handles. The engine's
        // reload sequence stalls waiting for BSGeometry destructors that
        // cannot run while we hold +1 NiPointer refs; releasing here lets
        // the old world fully tear down. Submission resumes naturally as
        // hooks fire for the new world.
        SemanticCapture::ClearDrawableMap();
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

    // Phase 1A: install BSLightingShaderProperty event-capture hook if
    // [SemanticCapture] Enabled=1. No-op when disabled (default).
    // Builds DrawableMap with TTL eviction; no Remix submission yet.
    SemanticCapture::Install();

    g_messaging = static_cast<F4SEMessagingInterface*>(
        f4se->QueryInterface(kInterface_Messaging));
    if (!g_messaging) {
        _MESSAGE("FO4RemixPlugin: ERROR - Could not get messaging interface");
        return false;
    }

    g_messaging->RegisterListener(g_pluginHandle, "F4SE", OnF4SEMessage);
    _MESSAGE("FO4RemixPlugin: Registered for F4SE messages");
    return true;
}

} // extern "C"
