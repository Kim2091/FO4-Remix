#include "config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "f4se/PluginAPI.h" // _MESSAGE

PluginConfig g_config = {};

static bool GetIniBool(const char* section, const char* key, bool def, const char* path) {
    // GetPrivateProfileIntA parses NUMBERS only -- the string "true" reads
    // as 0. That silently disabled every "= true" line in the shipped ini
    // (GlowMapsEnabled, EmissiveColorEnabled, and the 2026-07-08 [Skinning]
    // Enabled flip that made the skinning revival look dead). Read the
    // string and accept the word forms plus numerics.
    char buf[32] = {};
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
    if (buf[0] == '\0') return def;
    if (_stricmp(buf, "true") == 0 || _stricmp(buf, "yes") == 0 ||
        _stricmp(buf, "on") == 0)
        return true;
    if (_stricmp(buf, "false") == 0 || _stricmp(buf, "no") == 0 ||
        _stricmp(buf, "off") == 0)
        return false;
    return atoi(buf) != 0;
}

static float GetIniFloat(const char* section, const char* key, float def, const char* path) {
    char buf[64];
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
    if (buf[0] == '\0') return def;
    return (float)atof(buf);
}

static int GetIniInt(const char* section, const char* key, int def, const char* path) {
    return GetPrivateProfileIntA(section, key, def, path);
}

// Unsigned ini read. A negative value used to be cast straight to uint32_t
// and wrap to ~4.29e9 (e.g. LodChunkStaleFrames=-1 silently became
// "effectively disabled" instead of an error) -- treat it as a typo and
// fall back to the default, loudly.
static uint32_t GetIniUInt(const char* section, const char* key, uint32_t def, const char* path) {
    const int v = GetPrivateProfileIntA(section, key, (int)def, path);
    if (v < 0) {
        _MESSAGE("FO4RemixPlugin: Config - [%s] %s=%d is negative; using default %u",
                 section, key, v, def);
        return def;
    }
    return (uint32_t)v;
}

// Range-clamped float ini read for keys with a documented [lo, hi] domain.
// Out-of-range values (typos like "=10" on a 0..1 knob) fed straight into
// material/light math before; clamp and log instead.
static float GetIniFloatClamped(const char* section, const char* key, float def,
                                float lo, float hi, const char* path) {
    float v = GetIniFloat(section, key, def, path);
    if (v < lo || v > hi) {
        const float c = v < lo ? lo : hi;
        _MESSAGE("FO4RemixPlugin: Config - [%s] %s=%.3f outside [%.3f, %.3f]; clamped to %.3f",
                 section, key, v, lo, hi, c);
        v = c;
    }
    return v;
}

void LoadConfig() {
    // Build path: same directory as the DLL. On failure every key silently
    // falls back to its code default, which looks like a mysteriously
    // misconfigured session -- say so in the log.
    char dllPath[MAX_PATH] = {};
    HMODULE hm = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&LoadConfig, &hm) ||
        !GetModuleFileNameA(hm, dllPath, MAX_PATH)) {
        _MESSAGE("FO4RemixPlugin: WARNING - could not resolve plugin DLL path "
                 "(err=%lu); ALL config keys fall back to code defaults",
                 GetLastError());
        dllPath[0] = '\0';
    }

    // Replace .dll with .ini
    char* dot = strrchr(dllPath, '.');
    if (dot) strcpy(dot, ".ini");
    else strcat(dllPath, ".ini");

    _MESSAGE("FO4RemixPlugin: Loading config from %s", dllPath);

    // [Logging]
    g_config.logShapeInfo   = GetIniBool("Logging", "LogShapeInfo",   false, dllPath);
    g_config.logLargeShapes = GetIniBool("Logging", "LogLargeShapes", true,  dllPath);
    g_config.logRejections  = GetIniBool("Logging", "LogRejections",  true,  dllPath);
    g_config.logTextures    = GetIniBool("Logging", "LogTextures",    false, dllPath);
    g_config.logLights      = GetIniBool("Logging", "LogLights",      false, dllPath);
    g_config.logBoneDiag    = GetIniBool("Logging", "LogBoneDiag",    false, dllPath);

    // [Limits] MaxExtent retired 2026-07-10: the key was documented as a
    // shape-extent reject threshold but never consumed -- the resolvers use
    // a hard 1e6 NaN/garbage backstop. Wiring the shipped 10000 default in
    // would have started rejecting huge-local-extent LOD chunks, so the key
    // is dropped instead of silently changing behavior.

    // [Lights]
    g_config.lightsEnabled     = GetIniBool("Lights",  "Enabled",       true,  dllPath);
    g_config.lightIntensity    = GetIniFloatClamped("Lights", "Intensity", 1.0f, 0.0f, 1000.0f, dllPath);
    g_config.lightRadius       = GetIniFloatClamped("Lights", "RadiusMultiplier", 1.0f, 0.0f, 1000.0f, dllPath);
    g_config.lightColorStrength = GetIniFloatClamped("Lights", "ColorStrength", 1.0f, 0.0f, 1.0f, dllPath);
    g_config.lightsLiveUpdate  = GetIniBool("Lights", "LiveUpdate", true, dllPath);
    g_config.lightsNearCameraIgnoreVMUnits =
        GetIniFloatClamped("Lights", "NearCameraIgnoreViewModelUnits", 150.0f, 0.0f, 10000.0f, dllPath);
    _MESSAGE("FO4RemixPlugin: Lights - LiveUpdate=%d NearCameraIgnoreViewModelUnits=%.0f",
             g_config.lightsLiveUpdate, g_config.lightsNearCameraIgnoreVMUnits);

    // [Skinning]
    g_config.skinningEnabled = GetIniBool("Skinning", "Enabled", true, dllPath);
    g_config.viewModelEnabled = GetIniBool("ViewModel", "Enabled", true, dllPath);
    g_config.viewModelBoneConventionFix =
        GetIniBool("ViewModel", "BoneConventionFix", true, dllPath);
    g_config.viewModelSeparateCamera = GetIniBool("ViewModel", "SeparateCamera", true, dllPath);
    g_config.viewModelFovOverride =
        GetIniFloatClamped("ViewModel", "FovOverride", 0.0f, 0.0f, 179.0f, dllPath);
    g_config.viewModelCategoryTag = GetIniBool("ViewModel", "CategoryTag", true, dllPath);
    g_config.viewModelScreenRefreshFrames =
        GetIniUInt("ViewModel", "ScreenRefreshFrames", 12, dllPath);
    g_config.pipboyScreenFeed = GetIniBool("ViewModel", "PipboyScreenFeed", true, dllPath);
    g_config.pipboyScreenTintR =
        GetIniFloatClamped("ViewModel", "PipboyScreenTintR", 0.08f, 0.0f, 1.0f, dllPath);
    g_config.pipboyScreenTintG =
        GetIniFloatClamped("ViewModel", "PipboyScreenTintG", 1.00f, 0.0f, 1.0f, dllPath);
    g_config.pipboyScreenTintB =
        GetIniFloatClamped("ViewModel", "PipboyScreenTintB", 0.09f, 0.0f, 1.0f, dllPath);
    g_config.pipboyScreenEmissiveScale =
        GetIniFloatClamped("ViewModel", "PipboyScreenEmissiveScale", 1.5f, 0.0f, 100.0f, dllPath);
    g_config.overlayPipboyPanelFrac =
        GetIniFloatClamped("Overlay", "PipboyPanelHeightFrac", 0.55f, 0.0f, 1.0f, dllPath);
    g_config.overlayMultiLayer = GetIniBool("Overlay", "MultiLayerCapture", true, dllPath);
    _MESSAGE("FO4RemixPlugin: Overlay - MultiLayerCapture=%d PipboyPanelHeightFrac=%.2f",
             g_config.overlayMultiLayer, g_config.overlayPipboyPanelFrac);
    _MESSAGE("FO4RemixPlugin: ViewModel - SeparateCamera=%d FovOverride=%.1f CategoryTag=%d "
             "ScreenRefreshFrames=%u PipboyScreenFeed=%d tint=(%.2f,%.2f,%.2f) emissive=%.1f",
             g_config.viewModelSeparateCamera, g_config.viewModelFovOverride,
             g_config.viewModelCategoryTag, g_config.viewModelScreenRefreshFrames,
             g_config.pipboyScreenFeed, g_config.pipboyScreenTintR,
             g_config.pipboyScreenTintG, g_config.pipboyScreenTintB,
             g_config.pipboyScreenEmissiveScale);

    // [Emissive]
    g_config.emissiveGlowMapsEnabled = GetIniBool("Emissive", "GlowMapsEnabled", true, dllPath);
    g_config.emissiveColorEnabled    = GetIniBool("Emissive", "EmissiveColorEnabled", true, dllPath);
    g_config.emissiveIntensity       = GetIniFloatClamped("Emissive", "Intensity", 1.0f, 0.0f, 1000.0f, dllPath);
    g_config.logEmissive             = GetIniBool("Emissive", "LogEmissive", false, dllPath);

    // [Diagnostics]
    g_config.diagEnabled = GetIniBool("Diagnostics", "Enabled", true, dllPath);
    _MESSAGE("FO4RemixPlugin: Diagnostics - Enabled=%d", g_config.diagEnabled);

    // [SemanticCapture]
    g_config.semanticCaptureEnabled = GetIniBool("SemanticCapture", "Enabled", false, dllPath);
    g_config.resolveRetryWindowFrames = GetIniUInt("SemanticCapture", "ResolveRetryWindowFrames", 600, dllPath);
    _MESSAGE("FO4RemixPlugin: SemanticCapture - Enabled=%d ResolveRetryWindowFrames=%u",
             g_config.semanticCaptureEnabled, g_config.resolveRetryWindowFrames);

    // [Culling]
    g_config.cullingTextureLRUGraceFrames  = GetIniUInt("Culling", "TextureLRUGraceFrames",  600, dllPath);
    g_config.cullingTextureLRUSweepPeriod  = GetIniUInt("Culling", "TextureLRUSweepPeriod",  60,  dllPath);
    g_config.cullingTextureBudgetMiB       = GetIniUInt("Culling", "TextureBudgetMiB",       0,   dllPath);
    g_config.cullingMaterialLRUGraceFrames = GetIniUInt("Culling", "MaterialLRUGraceFrames", 600, dllPath);
    g_config.cullingLodChunkStaleFrames    = GetIniUInt("Culling", "LodChunkStaleFrames",    30,  dllPath);
    g_config.cullingLodChunkFarExtentRatio = GetIniFloat("Culling", "LodChunkFarExtentRatio", 0.0f, dllPath);
    g_config.cullingForceEvictVramPct      = GetIniUInt("Culling", "ForceEvictVramPct",      88,  dllPath);
    g_config.cullingForceEvictPerSweep     = GetIniUInt("Culling", "ForceEvictPerSweep",     512, dllPath);
    g_config.cullingForceEvictViewPct      = GetIniUInt("Culling", "ForceEvictViewPct",      60,  dllPath);
    g_config.cullingForceEvictBehindDistance = GetIniFloat("Culling", "ForceEvictBehindDistance", 8000.0f, dllPath);
    g_config.cullingForceEvictAlwaysBehindDistance = GetIniFloat("Culling", "ForceEvictAlwaysBehindDistance", 40000.0f, dllPath);
    g_config.cullingForceEvictLodBehindDistance = GetIniFloat("Culling", "ForceEvictLodBehindDistance", 12000.0f, dllPath);
    _MESSAGE("FO4RemixPlugin: Culling - TextureLRUGraceFrames=%u TextureLRUSweepPeriod=%u TextureBudgetMiB=%u MaterialLRUGraceFrames=%u LodChunkStaleFrames=%u LodChunkFarExtentRatio=%.1f ForceEvictVramPct=%u ForceEvictPerSweep=%u ForceEvictViewPct=%u ForceEvictBehindDistance=%.0f ForceEvictAlwaysBehindDistance=%.0f ForceEvictLodBehindDistance=%.0f",
             g_config.cullingTextureLRUGraceFrames,
             g_config.cullingTextureLRUSweepPeriod,
             g_config.cullingTextureBudgetMiB,
             g_config.cullingMaterialLRUGraceFrames,
             g_config.cullingLodChunkStaleFrames,
             g_config.cullingLodChunkFarExtentRatio,
             g_config.cullingForceEvictVramPct,
             g_config.cullingForceEvictPerSweep,
             g_config.cullingForceEvictViewPct,
             g_config.cullingForceEvictBehindDistance,
             g_config.cullingForceEvictAlwaysBehindDistance,
             g_config.cullingForceEvictLodBehindDistance);

    // [Materials]
    g_config.metalConversionEnabled = GetIniBool("Materials", "MetalConversionEnabled", true, dllPath);
    g_config.metalMetallicEnabled   = GetIniBool("Materials", "MetalMetallicEnabled",   false, dllPath);
    g_config.metalRoughnessEnabled  = GetIniBool("Materials", "MetalRoughnessEnabled",  false, dllPath);
    g_config.metalMetallic          = GetIniFloatClamped("Materials", "MetalMetallic",       0.85f, 0.0f, 1.0f, dllPath);
    g_config.metalAlbedoLumFloor    = GetIniFloatClamped("Materials", "MetalAlbedoLumFloor", 0.25f, 0.0f, 1.0f, dllPath);
    g_config.metalMinRoughness      = GetIniFloatClamped("Materials", "MetalMinRoughness",   0.15f, 0.0f, 1.0f, dllPath);
    g_config.roughnessMapsEnabled   = GetIniBool("Materials",  "RoughnessMapsEnabled",  true,  dllPath);
    g_config.roughnessMapFloor      = GetIniFloatClamped("Materials", "RoughnessMapFloor",   0.15f, 0.0f, 1.0f, dllPath);
    g_config.textureUpgradeOnApproach = GetIniBool("Materials", "TextureUpgradeOnApproach", false, dllPath);
    g_config.maxTextureDimension = GetIniUInt("Materials", "MaxTextureDimension", 2048, dllPath);
    g_config.diskTextureCache    = GetIniBool("Materials", "DiskTextureCache", true, dllPath);
    g_config.diskTextureCacheGiB = GetIniUInt("Materials", "DiskTextureCacheGiB", 8, dllPath);
    _MESSAGE("FO4RemixPlugin: Materials - MaxTextureDimension=%u DiskTextureCache=%d (cap %u GiB)",
             g_config.maxTextureDimension, g_config.diskTextureCache ? 1 : 0,
             g_config.diskTextureCacheGiB);
    _MESSAGE("FO4RemixPlugin: Materials - MetalConversionEnabled=%d MetalMetallicEnabled=%d MetalRoughnessEnabled=%d MetalMetallic=%.2f MetalAlbedoLumFloor=%.2f MetalMinRoughness=%.2f RoughnessMapsEnabled=%d RoughnessMapFloor=%.2f TextureUpgradeOnApproach=%d",
             g_config.metalConversionEnabled, g_config.metalMetallicEnabled,
             g_config.metalRoughnessEnabled, g_config.metalMetallic,
             g_config.metalAlbedoLumFloor, g_config.metalMinRoughness,
             g_config.roughnessMapsEnabled, g_config.roughnessMapFloor,
             g_config.textureUpgradeOnApproach);

    // [Camera]
    g_config.cameraFovFromFrustum = GetIniBool("Camera", "FovFromFrustum", true, dllPath);
    _MESSAGE("FO4RemixPlugin: Camera - FovFromFrustum=%d", g_config.cameraFovFromFrustum);

    // [Overlay]
    g_config.hudOverlayEnabled  = GetIniBool("Overlay", "HudOverlayEnabled",  false, dllPath);
    g_config.restoreLegacyInput = GetIniBool("Overlay", "RestoreLegacyInput", true,  dllPath);
    _MESSAGE("FO4RemixPlugin: Overlay - HudOverlayEnabled=%d RestoreLegacyInput=%d",
             g_config.hudOverlayEnabled, g_config.restoreLegacyInput);

    // [Performance]
    g_config.gpuInstancingEnabled = GetIniBool("Performance", "GpuInstancing", true, dllPath);
    g_config.batchedMirrorBase = GetIniBool("Performance", "BatchedMirrorBase", true, dllPath);
    g_config.remixMaxFPS = GetIniUInt("Performance", "RemixMaxFPS", 120, dllPath);
    g_config.maxPendingTextureReadbacks = GetIniUInt("Performance", "MaxPendingTextureReadbacks", 256, dllPath);
    g_config.resolveBudgetMs = GetIniFloatClamped("Performance", "ResolveBudgetMs", 3.0f, 0.0f, 1000.0f, dllPath);
    g_config.decodeWorkerPercent = GetIniUInt("Performance", "DecodeWorkerPercent", 25, dllPath);
    if (g_config.decodeWorkerPercent < 1)   g_config.decodeWorkerPercent = 1;
    if (g_config.decodeWorkerPercent > 100) g_config.decodeWorkerPercent = 100;
    g_config.decodeWorkerMax = GetIniUInt("Performance", "DecodeWorkerMax", 4, dllPath);
    g_config.cpuTextureCacheMiB = GetIniUInt("Performance", "CpuTextureCacheMiB", 1024, dllPath);
    g_config.maxUploadMiBPerTick = GetIniUInt("Performance", "MaxUploadMiBPerTick", 48, dllPath);
    g_config.suppressGameRaster = GetIniBool("Performance", "SuppressGameRaster", false, dllPath);
    g_config.deferHandleDestroyToLoad = GetIniBool("Performance", "DeferHandleDestroyToLoad", true, dllPath);
    _MESSAGE("FO4RemixPlugin: Performance - DeferHandleDestroyToLoad=%d",
             g_config.deferHandleDestroyToLoad);
    _MESSAGE("FO4RemixPlugin: Performance - GpuInstancing=%d BatchedMirrorBase=%d RemixMaxFPS=%u MaxPendingTextureReadbacks=%u ResolveBudgetMs=%.1f DecodeWorkerPercent=%u DecodeWorkerMax=%u CpuTextureCacheMiB=%u MaxUploadMiBPerTick=%u SuppressGameRaster=%d",
             g_config.gpuInstancingEnabled, g_config.batchedMirrorBase,
             g_config.remixMaxFPS, g_config.maxPendingTextureReadbacks,
             g_config.resolveBudgetMs, g_config.decodeWorkerPercent,
             g_config.decodeWorkerMax, g_config.cpuTextureCacheMiB,
             g_config.maxUploadMiBPerTick, g_config.suppressGameRaster);
    if (g_config.suppressGameRaster && !g_config.hudOverlayEnabled) {
        _MESSAGE("FO4RemixPlugin: WARNING - SuppressGameRaster=1 with "
                 "HudOverlayEnabled=0: the game window will stop updating and "
                 "no UI will be visible anywhere. Enable the HUD overlay.");
    }

    // [Window]
    g_config.windowOverlayMode = GetIniBool("Window", "OverlayMode", true, dllPath);
    // MenuHotkey accepts hex ("0x77") or decimal; GetPrivateProfileIntA
    // parses decimal only, so read the string and strtoul(base 0).
    {
        char hk[32] = {};
        GetPrivateProfileStringA("Window", "MenuHotkey", "0x77", hk, sizeof(hk), dllPath);
        g_config.windowMenuHotkey = (uint32_t)strtoul(hk, nullptr, 0);
    }
    _MESSAGE("FO4RemixPlugin: Window - OverlayMode=%d MenuHotkey=0x%02X",
             g_config.windowOverlayMode, g_config.windowMenuHotkey);

    // [Precombines]
    g_config.mergeInstanceExpansion = GetIniBool("Precombines", "MergeInstanceExpansion", true, dllPath);
    g_config.mergeInstanceRowVector = GetIniBool("Precombines", "MergeInstanceRowVector", true, dllPath);
    g_config.mergeInstanceConjugate = GetIniBool("Precombines", "MergeInstanceConjugate", false, dllPath);
    g_config.mergeInstanceDrawCapture = GetIniBool("Precombines", "MergeInstanceDrawCapture", true, dllPath);
    g_config.mergeTwoSided = GetIniBool("Precombines", "MergeTwoSided", true, dllPath);
    _MESSAGE("FO4RemixPlugin: Precombines - MergeInstanceExpansion=%d MergeInstanceRowVector=%d "
             "MergeInstanceConjugate=%d MergeInstanceDrawCapture=%d MergeTwoSided=%d",
             g_config.mergeInstanceExpansion, g_config.mergeInstanceRowVector,
             g_config.mergeInstanceConjugate, g_config.mergeInstanceDrawCapture,
             g_config.mergeTwoSided);

    _MESSAGE("FO4RemixPlugin: Config loaded - LogShapeInfo=%d LogLargeShapes=%d LogRejections=%d "
             "LogTextures=%d LogLights=%d",
             g_config.logShapeInfo, g_config.logLargeShapes, g_config.logRejections,
             g_config.logTextures, g_config.logLights);
    _MESSAGE("FO4RemixPlugin: Lights - Enabled=%d Intensity=%.2f RadiusMul=%.2f ColorStrength=%.2f",
             g_config.lightsEnabled, g_config.lightIntensity,
             g_config.lightRadius, g_config.lightColorStrength);
    _MESSAGE("FO4RemixPlugin: Skinning - Enabled=%d", g_config.skinningEnabled);
    _MESSAGE("FO4RemixPlugin: ViewModel - Enabled=%d BoneConventionFix=%d",
             g_config.viewModelEnabled, g_config.viewModelBoneConventionFix);
    _MESSAGE("FO4RemixPlugin: Emissive - GlowMaps=%d EmissiveColor=%d Intensity=%.2f LogEmissive=%d",
             g_config.emissiveGlowMapsEnabled, g_config.emissiveColorEnabled,
             g_config.emissiveIntensity, g_config.logEmissive);
}
