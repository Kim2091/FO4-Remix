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

void LoadConfig() {
    // Build path: same directory as the DLL
    char dllPath[MAX_PATH] = {};
    HMODULE hm = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&LoadConfig, &hm);
    GetModuleFileNameA(hm, dllPath, MAX_PATH);

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

    // [Limits]
    g_config.maxExtent = GetIniFloat("Limits", "MaxExtent", 10000.0f, dllPath);

    // [Lights]
    g_config.lightsEnabled     = GetIniBool("Lights",  "Enabled",       true,  dllPath);
    g_config.lightIntensity    = GetIniFloat("Lights", "Intensity",     1.0f,  dllPath);
    g_config.lightRadius       = GetIniFloat("Lights", "RadiusMultiplier", 1.0f, dllPath);
    g_config.lightColorStrength = GetIniFloat("Lights", "ColorStrength", 1.0f, dllPath);

    // [Skinning]
    g_config.skinningEnabled = GetIniBool("Skinning", "Enabled", true, dllPath);

    // [Emissive]
    g_config.emissiveGlowMapsEnabled = GetIniBool("Emissive", "GlowMapsEnabled", true, dllPath);
    g_config.emissiveColorEnabled    = GetIniBool("Emissive", "EmissiveColorEnabled", true, dllPath);
    g_config.emissiveIntensity       = GetIniFloat("Emissive", "Intensity", 1.0f, dllPath);
    g_config.logEmissive             = GetIniBool("Emissive", "LogEmissive", false, dllPath);

    // [Diagnostics]
    g_config.diagEnabled = GetIniBool("Diagnostics", "Enabled", true, dllPath);
    _MESSAGE("FO4RemixPlugin: Diagnostics - Enabled=%d", g_config.diagEnabled);

    // [SemanticCapture]
    g_config.semanticCaptureEnabled = GetIniBool("SemanticCapture", "Enabled", false, dllPath);
    g_config.resolveRetryWindowFrames = (uint32_t)GetIniInt("SemanticCapture", "ResolveRetryWindowFrames", 600, dllPath);
    _MESSAGE("FO4RemixPlugin: SemanticCapture - Enabled=%d ResolveRetryWindowFrames=%u",
             g_config.semanticCaptureEnabled, g_config.resolveRetryWindowFrames);

    // [Culling]
    g_config.cullingTextureLRUGraceFrames  = (uint32_t)GetIniInt("Culling", "TextureLRUGraceFrames",  600, dllPath);
    g_config.cullingTextureLRUSweepPeriod  = (uint32_t)GetIniInt("Culling", "TextureLRUSweepPeriod",  60,  dllPath);
    g_config.cullingTextureBudgetMiB       = (uint32_t)GetIniInt("Culling", "TextureBudgetMiB",       0,   dllPath);
    g_config.cullingMaterialLRUGraceFrames = (uint32_t)GetIniInt("Culling", "MaterialLRUGraceFrames", 600, dllPath);
    g_config.cullingLodChunkStaleFrames    = (uint32_t)GetIniInt("Culling", "LodChunkStaleFrames",    30,  dllPath);
    _MESSAGE("FO4RemixPlugin: Culling - TextureLRUGraceFrames=%u TextureLRUSweepPeriod=%u TextureBudgetMiB=%u MaterialLRUGraceFrames=%u LodChunkStaleFrames=%u",
             g_config.cullingTextureLRUGraceFrames,
             g_config.cullingTextureLRUSweepPeriod,
             g_config.cullingTextureBudgetMiB,
             g_config.cullingMaterialLRUGraceFrames,
             g_config.cullingLodChunkStaleFrames);

    // [Materials]
    g_config.metalConversionEnabled = GetIniBool("Materials", "MetalConversionEnabled", true, dllPath);
    g_config.metalMetallicEnabled   = GetIniBool("Materials", "MetalMetallicEnabled",   false, dllPath);
    g_config.metalRoughnessEnabled  = GetIniBool("Materials", "MetalRoughnessEnabled",  false, dllPath);
    g_config.metalMetallic          = GetIniFloat("Materials", "MetalMetallic",         0.85f, dllPath);
    g_config.metalAlbedoLumFloor    = GetIniFloat("Materials", "MetalAlbedoLumFloor",   0.25f, dllPath);
    g_config.metalMinRoughness      = GetIniFloat("Materials", "MetalMinRoughness",     0.15f, dllPath);
    g_config.roughnessMapsEnabled   = GetIniBool("Materials",  "RoughnessMapsEnabled",  true,  dllPath);
    g_config.roughnessMapFloor      = GetIniFloat("Materials", "RoughnessMapFloor",     0.15f, dllPath);
    g_config.textureUpgradeOnApproach = GetIniBool("Materials", "TextureUpgradeOnApproach", false, dllPath);
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
    g_config.remixMaxFPS = (uint32_t)GetIniInt("Performance", "RemixMaxFPS", 120, dllPath);
    g_config.maxPendingTextureReadbacks = (uint32_t)GetIniInt("Performance", "MaxPendingTextureReadbacks", 256, dllPath);
    g_config.resolveBudgetMs = GetIniFloat("Performance", "ResolveBudgetMs", 3.0f, dllPath);
    _MESSAGE("FO4RemixPlugin: Performance - GpuInstancing=%d BatchedMirrorBase=%d RemixMaxFPS=%u MaxPendingTextureReadbacks=%u ResolveBudgetMs=%.1f",
             g_config.gpuInstancingEnabled, g_config.batchedMirrorBase,
             g_config.remixMaxFPS, g_config.maxPendingTextureReadbacks,
             g_config.resolveBudgetMs);

    // [Precombines]
    g_config.mergeInstanceExpansion = GetIniBool("Precombines", "MergeInstanceExpansion", true, dllPath);
    g_config.mergeInstanceRowVector = GetIniBool("Precombines", "MergeInstanceRowVector", true, dllPath);
    g_config.mergeInstanceConjugate = GetIniBool("Precombines", "MergeInstanceConjugate", false, dllPath);
    g_config.mergeInstanceDrawCapture = GetIniBool("Precombines", "MergeInstanceDrawCapture", true, dllPath);
    _MESSAGE("FO4RemixPlugin: Precombines - MergeInstanceExpansion=%d MergeInstanceRowVector=%d "
             "MergeInstanceConjugate=%d MergeInstanceDrawCapture=%d",
             g_config.mergeInstanceExpansion, g_config.mergeInstanceRowVector,
             g_config.mergeInstanceConjugate, g_config.mergeInstanceDrawCapture);

    _MESSAGE("FO4RemixPlugin: Config loaded - LogShapeInfo=%d LogLargeShapes=%d LogRejections=%d "
             "LogTextures=%d LogLights=%d MaxExtent=%.0f",
             g_config.logShapeInfo, g_config.logLargeShapes, g_config.logRejections,
             g_config.logTextures, g_config.logLights, g_config.maxExtent);
    _MESSAGE("FO4RemixPlugin: Lights - Enabled=%d Intensity=%.2f RadiusMul=%.2f ColorStrength=%.2f",
             g_config.lightsEnabled, g_config.lightIntensity,
             g_config.lightRadius, g_config.lightColorStrength);
    _MESSAGE("FO4RemixPlugin: Skinning - Enabled=%d", g_config.skinningEnabled);
    _MESSAGE("FO4RemixPlugin: Emissive - GlowMaps=%d EmissiveColor=%d Intensity=%.2f LogEmissive=%d",
             g_config.emissiveGlowMapsEnabled, g_config.emissiveColorEnabled,
             g_config.emissiveIntensity, g_config.logEmissive);
}
