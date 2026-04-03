#include "remix_api.h"

#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"

static remixapi_Interface g_remixInterface = {};
static HMODULE g_remixDll = nullptr;
static bool g_initialized = false;
static HWND g_remixWindow = nullptr;

static LRESULT CALLBACK RemixWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        // CRITICAL: Must handle WM_PAINT or Windows marks window as frozen
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND CreateRemixWindow(int width, int height) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = RemixWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.lpszClassName = L"FO4RemixWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        wc.lpszClassName,
        L"Fallout 4 - RTX Remix",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        100, 100, width, height,
        nullptr, nullptr, hInst, nullptr);

    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
    }

    return hwnd;
}

bool RemixAPI::Initialize(HWND gameWindow) {
    if (g_initialized) return true;

    remixapi_ErrorCode status = remixapi_lib_loadRemixDllAndInitialize(
        L"d3d9.dll", &g_remixInterface, &g_remixDll);

    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        _MESSAGE("FO4RemixPlugin: remixapi_lib_loadRemixDllAndInitialize failed (error %d)", status);
        return false;
    }
    _MESSAGE("FO4RemixPlugin: Remix library loaded and initialized");

    // Create a dedicated window for Remix (cannot share the game's DX11 HWND)
    g_remixWindow = CreateRemixWindow(1280, 720);
    if (!g_remixWindow) {
        _MESSAGE("FO4RemixPlugin: ERROR - Failed to create Remix window");
        return false;
    }
    _MESSAGE("FO4RemixPlugin: Remix window created (HWND=%p)", g_remixWindow);

    remixapi_StartupInfo startupInfo = {};
    startupInfo.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
    startupInfo.hwnd = g_remixWindow;
    startupInfo.disableSrgbConversionForOutput = 0;
    startupInfo.forceNoVkSwapchain = 0;
    startupInfo.editorModeEnabled = 0;

    status = g_remixInterface.Startup(&startupInfo);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        _MESSAGE("FO4RemixPlugin: Startup failed (error %d)", status);
        DestroyWindow(g_remixWindow);
        g_remixWindow = nullptr;
        FreeLibrary(g_remixDll);
        g_remixDll = nullptr;
        return false;
    }
    _MESSAGE("FO4RemixPlugin: Remix Startup complete");

    g_initialized = true;
    return true;
}

void RemixAPI::Shutdown() {
    if (!g_initialized) return;

    if (g_remixInterface.Shutdown) {
        g_remixInterface.Shutdown();
    }
    if (g_remixWindow) {
        DestroyWindow(g_remixWindow);
        g_remixWindow = nullptr;
    }
    if (g_remixDll) {
        FreeLibrary(g_remixDll);
        g_remixDll = nullptr;
    }
    g_initialized = false;
}

remixapi_Interface* RemixAPI::GetInterface() {
    return g_initialized ? &g_remixInterface : nullptr;
}

bool RemixAPI::IsInitialized() {
    return g_initialized;
}
