#include "startup_diag.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include "f4se/PluginAPI.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string BaseName(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

std::string FormatFileTime(FILETIME ft) {
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02uZ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

struct FileFacts {
    bool exists = false;
    std::uint64_t size = 0;
    std::string mtime;
};

FileFacts StatFile(const std::string& path) {
    FileFacts out{};
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) {
        out.exists = true;
        out.size = (std::uint64_t(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        out.mtime = FormatFileTime(fad.ftLastWriteTime);
    }
    return out;
}

std::string GetModulePath(HMODULE hm) {
    char buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(hm, buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::string(buf, n) : std::string{};
}

std::string GetPluginDllPath() {
    HMODULE hm = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&GetPluginDllPath, &hm);
    return GetModulePath(hm);
}

std::string GetExePath() {
    return GetModulePath(GetModuleHandleA(nullptr));
}

void LogOSVersion() {
    // RtlGetVersion bypasses the GetVersion application-compatibility lie that
    // caps unprovisioned apps at 6.2. Without this, Win11 shows up as Win8.
    using RtlGetVersionFn = LONG (WINAPI*)(PRTL_OSVERSIONINFOEXW);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    RtlGetVersionFn fn = ntdll
        ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))
        : nullptr;

    RTL_OSVERSIONINFOEXW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn && fn(&vi) == 0) {
        _MESSAGE("FO4RemixPlugin: [StartupDiag] OS real=%lu.%lu.%lu sp=%u product=%u suite=0x%X",
                 vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber,
                 vi.wServicePackMajor, vi.wProductType, vi.wSuiteMask);
    } else {
        _MESSAGE("FO4RemixPlugin: [StartupDiag] RtlGetVersion unavailable");
    }
}

void LogProcessStats() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        _MESSAGE("FO4RemixPlugin: [StartupDiag] processMem workingSet=%zuMB privateBytes=%zuMB",
                 pmc.WorkingSetSize / (1024 * 1024),
                 pmc.PagefileUsage / (1024 * 1024));
    }

    // Thread count at startup — sanity signal, no hard expected value.
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    int threadCount = -1;
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        threadCount = 0;
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID == pid) ++threadCount;
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }
    _MESSAGE("FO4RemixPlugin: [StartupDiag] processThreads=%d", threadCount);
}

void LogPluginAndExe() {
    const std::string pluginPath = GetPluginDllPath();
    const std::string exePath = GetExePath();

    {
        FileFacts f = StatFile(pluginPath);
        _MESSAGE("FO4RemixPlugin: [StartupDiag] plugin path=\"%s\" size=%llu mtime=%s",
                 pluginPath.c_str(), (unsigned long long)f.size,
                 f.mtime.empty() ? "?" : f.mtime.c_str());
    }
    {
        FileFacts f = StatFile(exePath);
        _MESSAGE("FO4RemixPlugin: [StartupDiag] exe    path=\"%s\" size=%llu mtime=%s",
                 exePath.c_str(), (unsigned long long)f.size,
                 f.mtime.empty() ? "?" : f.mtime.c_str());
    }
}

struct KeyDll {
    const char* name;
    const char* role;  // short description for the log
};

// DLLs whose owner (standard Windows path vs. game folder) tells us whether
// a wrapper DLL is sitting in front of the real one. Game-folder d3d9.dll
// is expected (DXVK-Remix). Game-folder d3d11.dll is the ENB / Community
// Shaders signature. dxgi.dll in the game folder is ReShade.
void LogKeyDllOwners() {
    static const std::array<KeyDll, 6> keys = {{
        { "d3d9.dll",           "DXVK-Remix bridge (expected in game folder)" },
        { "d3d11.dll",          "ENB/Community Shaders wrapper if in game folder" },
        { "dxgi.dll",           "ReShade / FSR wrapper if in game folder" },
        { "dinput8.dll",        "F4SE or ASI loader if in game folder" },
        { "XInput9_1_0.dll",    "controller input" },
        { "d3dcompiler_47.dll", "HLSL runtime compiler" },
    }};

    const std::string exeDir = [] {
        std::string p = GetExePath();
        auto slash = p.find_last_of("/\\");
        return (slash == std::string::npos) ? std::string{} : p.substr(0, slash);
    }();

    for (const auto& k : keys) {
        HMODULE hm = GetModuleHandleA(k.name);
        if (!hm) {
            _MESSAGE("FO4RemixPlugin: [StartupDiag] module %-22s NOT_LOADED (%s)",
                     k.name, k.role);
            continue;
        }
        std::string path = GetModulePath(hm);
        const bool inGameDir = !exeDir.empty() &&
            ToLower(path).rfind(ToLower(exeDir), 0) == 0;
        _MESSAGE("FO4RemixPlugin: [StartupDiag] module %-22s %s path=\"%s\"",
                 k.name, inGameDir ? "GAME_DIR" : "system", path.c_str());
    }
}

// Known overlay / hook-injector modules whose presence is worth calling out.
// We match by case-insensitive substring against the loaded DLL's base name
// (not path) so shadow-loaded variants still match.
struct OverlaySig {
    const char* substring;   // lowercased substring matched against loaded DLL basenames
    const char* label;       // human label written to the log
};

void LogOverlayDetection() {
    static const std::array<OverlaySig, 14> sigs = {{
        { "rtsshooks",           "RivaTuner Statistics Server" },
        { "rivatuner",           "RivaTuner Statistics Server" },
        { "msiafterburner",      "MSI Afterburner" },
        { "afterburner",         "MSI Afterburner" },
        { "nvspcap",             "NVIDIA ShadowPlay/GeForce Experience overlay" },
        { "nvoptimusenable",     "NVIDIA Optimus" },
        { "gameoverlayrenderer", "Steam overlay" },
        { "discord_game_sdk",    "Discord GameSDK" },
        { "discordhook",         "Discord overlay" },
        { "reshade",             "ReShade" },
        { "specialk",            "Special K" },
        { "fraps",               "Fraps" },
        { "bandicam",            "Bandicam" },
        { "obs-hook",            "OBS game-capture hook" },
    }};

    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        _MESSAGE("FO4RemixPlugin: [StartupDiag] EnumProcessModules failed");
        return;
    }
    // `needed` reports the FULL module count even when it exceeds the
    // buffer; iterating past sizeof(mods) reads uninitialized stack.
    if (needed > sizeof(mods)) needed = sizeof(mods);
    const DWORD count = needed / sizeof(HMODULE);
    std::vector<std::string> hits;
    for (DWORD i = 0; i < count; ++i) {
        std::string path = GetModulePath(mods[i]);
        std::string base = ToLower(BaseName(path));
        for (const auto& s : sigs) {
            if (base.find(s.substring) != std::string::npos) {
                hits.push_back(std::string(s.label) + " (" + BaseName(path) + ")");
            }
        }
    }

    if (hits.empty()) {
        _MESSAGE("FO4RemixPlugin: [StartupDiag] overlays: none detected");
    } else {
        for (const auto& h : hits) {
            _MESSAGE("FO4RemixPlugin: [StartupDiag] overlay detected: %s", h.c_str());
        }
    }
    _MESSAGE("FO4RemixPlugin: [StartupDiag] totalLoadedModules=%u", (unsigned)count);
}

}  // namespace

namespace StartupDiag {

void DumpEnvironment() {
    _MESSAGE("FO4RemixPlugin: [StartupDiag] ===== startup diagnostic begin =====");
    LogOSVersion();
    LogProcessStats();
    LogPluginAndExe();
    LogKeyDllOwners();
    LogOverlayDetection();
    _MESSAGE("FO4RemixPlugin: [StartupDiag] ===== startup diagnostic end =====");
}

}  // namespace StartupDiag
