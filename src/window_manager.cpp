#include "window_manager.h"
#include "config.h"
#include "remix/remix_c.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

HWND g_remixHwnd = nullptr;
HWND g_gameHwnd  = nullptr;

bool g_overlayApplied = false;   // overlay style installed on the Remix window
bool g_interactive    = false;   // overlay currently accepts input (menu open)
bool g_overlayHidden  = false;   // hidden because game is minimized/backgrounded
RECT g_glueRect       = {};
bool g_haveGlueRect   = false;

std::atomic<bool> g_menuOpen{false};
bool g_hotkeyWasDown = false;

WNDPROC g_prevRemixProc = nullptr;

// ---------------------------------------------------------------------------
// Startup handoff diagnostics (BetaRT describeWindow port). One line per
// window naming class/title/pid/tid/root/owner -- the exact facts needed
// when "game started but input goes nowhere" reports come in (Discord and
// other foreground thieves included).
// ---------------------------------------------------------------------------
void DescribeWindow(HWND w, char* buf, size_t n) {
    if (!w) { snprintf(buf, n, "<null>"); return; }
    if (!IsWindow(w)) { snprintf(buf, n, "%p invalid", (void*)w); return; }
    char cls[96] = "", title[128] = "";
    GetClassNameA(w, cls, (int)sizeof(cls));
    GetWindowTextA(w, title, (int)sizeof(title));
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(w, &pid);
    const HWND root  = GetAncestor(w, GA_ROOT);
    const HWND owner = GetWindow(w, GW_OWNER);
    snprintf(buf, n, "%p class='%s' title='%s' pid=%lu tid=%lu root=%p owner=%p",
             (void*)w, cls, title, pid, tid,
             (root != w ? (void*)root : nullptr), (void*)owner);
}

void LogHandoff(const char* phase) {
    char game[320], remix[320], fore[320];
    DescribeWindow(g_gameHwnd, game, sizeof(game));
    DescribeWindow(g_remixHwnd, remix, sizeof(remix));
    DescribeWindow(GetForegroundWindow(), fore, sizeof(fore));
    _MESSAGE("FO4RemixPlugin: [Window] handoff %s:\n"
             "  game       = %s\n"
             "  remix      = %s\n"
             "  foreground = %s",
             phase, game, remix, fore);
}

// ---------------------------------------------------------------------------
// Remix-window subclass. Two jobs:
//   - WM_SETCURSOR while pass-through: hide the OS arrow so it can't float
//     over the path-traced view during gameplay (the game's own cursor
//     state applies to ITS windows, not this one). Interactive frames pass
//     through so the runtime's ImGui hook can set its cursor.
//   - WM_CLOSE: ignore. The overlay has no chrome, but Alt+F4 on a focused
//     interactive overlay would otherwise destroy the swapchain window out
//     from under the runtime.
// Installed AFTER the runtime's own subclass (device creation), so this
// proc runs first and forwards to the runtime's.
// ---------------------------------------------------------------------------
LRESULT CALLBACK RemixWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SETCURSOR:
        if (!g_interactive && g_config.windowOverlayMode) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        return 0;
    default:
        break;
    }
    return g_prevRemixProc
        ? CallWindowProcW(g_prevRemixProc, hwnd, msg, wParam, lParam)
        : DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool GetGameClientRectScreen(RECT& out) {
    if (!g_gameHwnd || !IsWindow(g_gameHwnd)) return false;
    RECT rc{};
    if (!GetClientRect(g_gameHwnd, &rc)) return false;
    POINT tl{rc.left, rc.top}, br{rc.right, rc.bottom};
    if (!ClientToScreen(g_gameHwnd, &tl) || !ClientToScreen(g_gameHwnd, &br)) return false;
    out = {tl.x, tl.y, br.x, br.y};
    return out.right > out.left && out.bottom > out.top;
}

void ApplyOverlayStyle() {
    LONG_PTR style = GetWindowLongPtrW(g_remixHwnd, GWL_STYLE);
    style &= ~(LONG_PTR)(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                         WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_POPUP;
    SetWindowLongPtrW(g_remixHwnd, GWL_STYLE, style);

    LONG_PTR ex = GetWindowLongPtrW(g_remixHwnd, GWL_EXSTYLE);
    // TOOLWINDOW: no taskbar entry (the game window keeps that role).
    // NOACTIVATE: clicks never steal activation from the game window, so
    // foreground-routed raw input keeps flowing to the game. Deliberately
    // NOT WS_EX_TRANSPARENT/LAYERED: FO4 reads mouse via raw input (cursor
    // position + button state), so hit-test pass-through isn't needed, and
    // layered styles are a known hazard for swapchain windows.
    ex |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    SetWindowLongPtrW(g_remixHwnd, GWL_EXSTYLE, ex);
    g_overlayApplied = true;
    g_haveGlueRect = false;  // force a SWP_FRAMECHANGED reposition next tick
}

void SetInteractive(bool on) {
    if (!g_remixHwnd) return;
    LONG_PTR ex = GetWindowLongPtrW(g_remixHwnd, GWL_EXSTYLE);
    if (on) ex &= ~(LONG_PTR)WS_EX_NOACTIVATE;
    else    ex |=  (LONG_PTR)WS_EX_NOACTIVATE;
    SetWindowLongPtrW(g_remixHwnd, GWL_EXSTYLE, ex);
    SetWindowPos(g_remixHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED |
                 (on ? 0 : SWP_NOACTIVATE));

    if (on) {
        // Overlay owns input while the menu is open. This window belongs to
        // the calling (Remix) thread, so Active/Focus are direct calls.
        SetForegroundWindow(g_remixHwnd);
        SetActiveWindow(g_remixHwnd);
        SetFocus(g_remixHwnd);
        _MESSAGE("FO4RemixPlugin: [Window] menu open -> overlay interactive + focused");
    } else {
        // Hand focus back to the game window (different thread: attach input
        // queues for the handoff, the BetaRT bringWindowToForeground recipe).
        if (g_gameHwnd && IsWindow(g_gameHwnd)) {
            const DWORD gameTid = GetWindowThreadProcessId(g_gameHwnd, nullptr);
            const DWORD myTid = GetCurrentThreadId();
            const bool attached = gameTid && gameTid != myTid &&
                                  AttachThreadInput(myTid, gameTid, TRUE);
            SetForegroundWindow(g_gameHwnd);
            if (attached) AttachThreadInput(myTid, gameTid, FALSE);
        }
        _MESSAGE("FO4RemixPlugin: [Window] menu closed -> overlay pass-through, game refocused");
    }
    g_interactive = on;
}

// True when either of our two windows (or a child of them) is foreground.
bool ProcessHasForeground() {
    const HWND fore = GetForegroundWindow();
    if (!fore) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fore, &pid);
    return pid == GetCurrentProcessId();
}

}  // namespace

namespace WindowManager {

void Init(HWND remixHwnd, HWND gameHwnd) {
    g_remixHwnd = remixHwnd;
    g_gameHwnd  = gameHwnd;
    if (!g_remixHwnd) return;

    LogHandoff("startup");

    // Subclass after the runtime installed its own proc at device creation
    // (we forward to it), from the thread that owns the window.
    g_prevRemixProc = (WNDPROC)SetWindowLongPtrW(
        g_remixHwnd, GWLP_WNDPROC, (LONG_PTR)&RemixWindowProc);

    if (g_config.windowOverlayMode) {
        ApplyOverlayStyle();
        _MESSAGE("FO4RemixPlugin: [Window] overlay mode active (glued to game client rect)");
    }
}

void Tick(remixapi_Interface* api) {
    if (!g_remixHwnd || !IsWindow(g_remixHwnd)) return;

    // ---- Menu state + hotkey ----
    bool open = g_menuOpen.load(std::memory_order_relaxed);
    if (api && api->GetUIState) {
        open = api->GetUIState() != REMIXAPI_UI_STATE_NONE;
    }
    if (api && api->SetUIState && g_config.windowMenuHotkey != 0) {
        const bool down =
            (GetAsyncKeyState((int)g_config.windowMenuHotkey) & 0x8000) != 0;
        if (down && !g_hotkeyWasDown && ProcessHasForeground()) {
            const remixapi_UIState next =
                open ? REMIXAPI_UI_STATE_NONE : REMIXAPI_UI_STATE_ADVANCED;
            api->SetUIState(next);
            open = next != REMIXAPI_UI_STATE_NONE;
            _MESSAGE("FO4RemixPlugin: [Window] menu hotkey -> %s",
                     open ? "open" : "closed");
        }
        g_hotkeyWasDown = down;
    }
    g_menuOpen.store(open, std::memory_order_relaxed);

    if (!g_config.windowOverlayMode) {
        return;  // legacy free-floating window; menu state still tracked
    }
    if (!g_overlayApplied) {
        ApplyOverlayStyle();
    }

    // ---- Visibility: follow the game, yield to other apps ----
    // Hidden when the game window is minimized OR neither of our windows is
    // foreground (alt-tab to another app must not leave a topmost overlay
    // covering it). Menu-open keeps it visible: the overlay itself is the
    // foreground window then.
    const bool gameGone = !g_gameHwnd || !IsWindow(g_gameHwnd) || IsIconic(g_gameHwnd);
    const bool shouldHide = gameGone || (!ProcessHasForeground() && !g_interactive);
    if (shouldHide) {
        if (!g_overlayHidden) {
            ShowWindow(g_remixHwnd, SW_HIDE);
            g_overlayHidden = true;
            _MESSAGE("FO4RemixPlugin: [Window] overlay hidden (%s)",
                     gameGone ? "game minimized/gone" : "process lost foreground");
        }
        if (g_interactive) SetInteractive(false);
        return;
    }
    if (g_overlayHidden) {
        ShowWindow(g_remixHwnd, SW_SHOWNOACTIVATE);
        g_overlayHidden = false;
        g_haveGlueRect = false;  // re-glue on return
        _MESSAGE("FO4RemixPlugin: [Window] overlay restored");
    }

    // ---- Glue to the game client rect (only when it changed) ----
    RECT rc{};
    if (GetGameClientRectScreen(rc)) {
        if (!g_haveGlueRect || memcmp(&rc, &g_glueRect, sizeof(rc)) != 0) {
            SetWindowPos(g_remixHwnd, HWND_TOPMOST,
                         rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
            g_glueRect = rc;
            g_haveGlueRect = true;
        }
    }

    // ---- Interactivity follows the menu ----
    if (open != g_interactive) {
        SetInteractive(open);
    }
}

bool IsMenuOpen() {
    return g_menuOpen.load(std::memory_order_relaxed);
}

void Shutdown() {
    if (g_remixHwnd && IsWindow(g_remixHwnd) && g_prevRemixProc) {
        SetWindowLongPtrW(g_remixHwnd, GWLP_WNDPROC, (LONG_PTR)g_prevRemixProc);
    }
    g_prevRemixProc = nullptr;
    g_remixHwnd = nullptr;
    g_gameHwnd = nullptr;
    g_menuOpen.store(false, std::memory_order_relaxed);
}

}  // namespace WindowManager
