#pragma once

// Window manager (2026-07-18, BetaRT overlay-mode port).
//
// The plugin runs a dual-window setup: the game window keeps running the
// engine (raster or live-UI-over-stale-scene with SuppressGameRaster) while
// the plugin-created Remix window shows the path-traced output. Historically
// the Remix window was a free-floating WS_OVERLAPPEDWINDOW the user arranged
// by hand, with focus juggling between the two.
//
// [Window] OverlayMode=1 turns the Remix window into a borderless, topmost,
// non-activating overlay glued to the game window's client rect: the game
// keeps focus (raw input keeps flowing to it -- see
// RemixAPI::RebindRawInputToGameWindow), clicks on the overlay don't steal
// activation, and the two windows read as one. When the Remix dev menu opens
// (GetUIState != NONE) the overlay becomes interactive and takes focus so
// ImGui gets the mouse; closing the menu restores the pass-through style and
// refocuses the game window.
//
// Independent of overlay mode, Tick also polls a configurable hotkey
// ([Window] MenuHotkey, default F8) that toggles the dev menu via
// SetUIState -- reachable even with [Overlay] RestoreLegacyInput=1, whose
// trade-off was losing the runtime's own raw-input menu hotkeys.
//
// Threading: Init/Tick/Shutdown run on the Remix thread, which owns the
// Remix window and its message pump. IsMenuOpen is readable from any thread.

#include <Windows.h>

typedef struct remixapi_Interface remixapi_Interface;

namespace WindowManager {
    // Remix thread. Call once after RemixAPI::Initialize succeeded; logs the
    // startup window-handoff diagnostic (game/remix/foreground windows).
    void Init(HWND remixHwnd, HWND gameHwnd);

    // Remix thread, once per frame loop iteration. Applies/maintains the
    // overlay glue, syncs interactivity with GetUIState, and services the
    // menu-toggle hotkey. Null api degrades to overlay glue only.
    void Tick(remixapi_Interface* api);

    // True while the Remix dev menu is open. Any thread. present_hook uses
    // this to pause the per-present raw-input rebind so the menu's input
    // path isn't stomped while the user is in it.
    bool IsMenuOpen();

    void Shutdown();
}
