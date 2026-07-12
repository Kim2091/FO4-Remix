#include "remix_api.h"

#include "config.h"
#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"
#include <d3d9.h>

static remixapi_Interface g_remixInterface = {};
static HMODULE g_remixDll = nullptr;
static bool g_initialized = false;
static HWND g_remixWindow = nullptr;
static IDirect3D9Ex* g_d3d9 = nullptr;
static IDirect3DDevice9Ex* g_d3d9Device = nullptr;

// Window construction matched to Skyrim's working dual-window setup:
// DefWindowProcW (no custom WndProc), 0 ext style (no WS_EX_APPWINDOW),
// CW_USEDEFAULT position, no explicit ShowWindow/UpdateWindow.
static HWND CreateRemixWindow(int width, int height) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = L"FO4RemixWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Fallout 4 - RTX Remix",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        nullptr, nullptr, hInst, nullptr);

    return hwnd;
}

bool RemixAPI::Initialize(HWND gameWindow, uint32_t width, uint32_t height) {
    if (g_initialized) return true;

    remixapi_ErrorCode status = remixapi_lib_loadRemixDllAndInitialize(
        L"d3d9.dll", &g_remixInterface, &g_remixDll);

    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        _MESSAGE("FO4RemixPlugin: remixapi_lib_loadRemixDllAndInitialize failed (error %d)", status);
        return false;
    }
    _MESSAGE("FO4RemixPlugin: Remix library loaded and initialized");

    // Create a dedicated window for Remix matching the game resolution
    g_remixWindow = CreateRemixWindow(width, height);
    if (!g_remixWindow) {
        _MESSAGE("FO4RemixPlugin: ERROR - Failed to create Remix window");
        FreeLibrary(g_remixDll);
        g_remixDll = nullptr;
        return false;
    }
    _MESSAGE("FO4RemixPlugin: Remix window created (HWND=%p, gameHwnd=%p)",
             g_remixWindow, gameWindow);

    // Preferred path: explicitly create + register a D3D9 device through Remix's
    // dxvk extension. This skips Startup()'s default-init flow that spawns the
    // dev-menu overlay window -- whose RegisterRawInputDevices(keyboard, mouse)
    // would otherwise replace the game's own raw-input registrations (Win32
    // last-call-wins) and kill in-game input. Falls back to Startup() if the
    // dxvk extension isn't available or any setup step fails.
    bool deviceRegistered = false;
    if (g_remixInterface.dxvk_CreateD3D9) {
        status = g_remixInterface.dxvk_CreateD3D9(FALSE, &g_d3d9);
        if (status == REMIXAPI_ERROR_CODE_SUCCESS && g_d3d9) {
            D3DPRESENT_PARAMETERS pp = {};
            pp.BackBufferWidth = width;
            pp.BackBufferHeight = height;
            pp.BackBufferFormat = D3DFMT_A8R8G8B8;
            pp.BackBufferCount = 1;
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp.hDeviceWindow = g_remixWindow;
            pp.Windowed = TRUE;
            pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

            // D3DCREATE_MULTITHREADED is LOAD-BEARING (2026-07-12): dxvk's
            // D3D9DeviceLock -- the lock remixapi_Present holds across its
            // flush and fork_hooks::createTexture takes around its EmitCs --
            // compiles to a NO-OP without this flag. With it absent, a
            // game-thread create could interleave with the Remix thread's
            // Present flush mid CS-chunk swap (m_csChunk momentarily null:
            // the AV at dxvk_cs.h:171 that killed four sessions tonight).
            HRESULT hr = g_d3d9->CreateDeviceEx(
                D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_remixWindow,
                D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                &pp, nullptr, &g_d3d9Device);

            if (SUCCEEDED(hr) && g_d3d9Device) {
                status = g_remixInterface.dxvk_RegisterD3D9Device(g_d3d9Device);
                if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
                    _MESSAGE("FO4RemixPlugin: D3D9 device explicitly created and registered (Startup() bypassed)");
                    deviceRegistered = true;
                } else {
                    _MESSAGE("FO4RemixPlugin: dxvk_RegisterD3D9Device failed (error %d) -- falling back to Startup()", status);
                    g_d3d9Device->Release(); g_d3d9Device = nullptr;
                    g_d3d9->Release();       g_d3d9       = nullptr;
                }
            } else {
                _MESSAGE("FO4RemixPlugin: CreateDeviceEx failed (HRESULT=0x%08X) -- falling back to Startup()", (unsigned)hr);
                if (g_d3d9) { g_d3d9->Release(); g_d3d9 = nullptr; }
            }
        } else {
            _MESSAGE("FO4RemixPlugin: dxvk_CreateD3D9 failed (error %d) -- falling back to Startup()", status);
        }
    } else {
        _MESSAGE("FO4RemixPlugin: dxvk_CreateD3D9 not present in this runtime -- falling back to Startup()");
    }

    if (!deviceRegistered) {
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
    }
    _MESSAGE("FO4RemixPlugin: Remix Startup complete (%s)",
             deviceRegistered ? "via dxvk_RegisterD3D9Device" : "via Startup()");

    g_initialized = true;
    return true;
}

void RemixAPI::Shutdown() {
    if (!g_initialized) return;

    if (g_remixInterface.Shutdown) {
        g_remixInterface.Shutdown();
    }
    if (g_d3d9Device) { g_d3d9Device->Release(); g_d3d9Device = nullptr; }
    if (g_d3d9)       { g_d3d9->Release();       g_d3d9       = nullptr; }
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

void RemixAPI::RestoreLegacyKeyboardInput() {
    if (!g_config.restoreLegacyInput) return;

    static bool s_done = false;
    if (s_done) return;

    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;     // Generic Desktop
    rid.usUsage     = 0x06;     // Keyboard
    rid.dwFlags     = RIDEV_REMOVE;
    rid.hwndTarget  = nullptr;  // Required to be NULL when RIDEV_REMOVE is set

    if (RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE))) {
        _MESSAGE("FO4RemixPlugin: Restored legacy keyboard input (Remix dev-menu hotkeys disabled)");
        s_done = true;
    }
    // If it returns FALSE the registration may not be in place yet; leave
    // s_done=false so the next call retries. Caller (present hook) is
    // expected to invoke this on a delayed cadence (~2s) until it sticks.
}

void RemixAPI::RebindRawInputToGameWindow(HWND gameWindow) {
    if (!gameWindow) return;

    // Re-register every call (idempotent + cheap, microseconds). Multiple
    // actors in the runtime register raw input at different times -- the
    // overlay window thread spawns separately from Remix Startup and runs
    // its own RegisterRawInputDevices(RIDEV_INPUTSINK) on the overlay HWND.
    // A one-shot rebind here loses to whichever actor registers last.
    // Re-registering on every Present guarantees we eventually win and stay
    // won (no runtime code re-registers in a loop).
    RAWINPUTDEVICE rid[2] = {};
    // Keyboard
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage     = 0x06;
    rid[0].dwFlags     = 0;
    rid[0].hwndTarget  = gameWindow;
    // Mouse
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage     = 0x02;
    rid[1].dwFlags     = 0;
    rid[1].hwndTarget  = gameWindow;

    BOOL ok = RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));

    // Throttled logging: first success, then any change in success/error state.
    static bool s_loggedSuccess = false;
    static DWORD s_lastErr = 0;
    if (ok) {
        if (!s_loggedSuccess) {
            _MESSAGE("FO4RemixPlugin: Re-bound raw input (kbd+mouse) to game HWND %p", gameWindow);
            s_loggedSuccess = true;
            s_lastErr = 0;
        }
    } else {
        DWORD err = GetLastError();
        if (err != s_lastErr) {
            _MESSAGE("FO4RemixPlugin: RebindRawInputToGameWindow failed (GetLastError=%lu)", err);
            s_lastErr = err;
        }
    }
}
