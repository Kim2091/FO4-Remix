#include "config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "f4se/PluginAPI.h" // _MESSAGE

PluginConfig g_config = {};

static bool GetIniBool(const char* section, const char* key, bool def, const char* path) {
    return GetPrivateProfileIntA(section, key, def ? 1 : 0, path) != 0;
}

static float GetIniFloat(const char* section, const char* key, float def, const char* path) {
    char buf[64];
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
    if (buf[0] == '\0') return def;
    return (float)atof(buf);
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

    // [Limits]
    g_config.maxExtent = GetIniFloat("Limits", "MaxExtent", 10000.0f, dllPath);

    // [Lights]
    g_config.lightsEnabled     = GetIniBool("Lights",  "Enabled",       true,  dllPath);
    g_config.lightIntensity    = GetIniFloat("Lights", "Intensity",     1.0f,  dllPath);
    g_config.lightRadius       = GetIniFloat("Lights", "RadiusMultiplier", 1.0f, dllPath);
    g_config.lightColorStrength = GetIniFloat("Lights", "ColorStrength", 1.0f, dllPath);

    _MESSAGE("FO4RemixPlugin: Config loaded - LogShapeInfo=%d LogLargeShapes=%d LogRejections=%d "
             "LogTextures=%d LogLights=%d MaxExtent=%.0f",
             g_config.logShapeInfo, g_config.logLargeShapes, g_config.logRejections,
             g_config.logTextures, g_config.logLights, g_config.maxExtent);
    _MESSAGE("FO4RemixPlugin: Lights - Enabled=%d Intensity=%.2f RadiusMul=%.2f ColorStrength=%.2f",
             g_config.lightsEnabled, g_config.lightIntensity,
             g_config.lightRadius, g_config.lightColorStrength);
}
