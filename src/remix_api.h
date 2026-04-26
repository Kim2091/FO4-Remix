#pragma once

#include "remix/remix_c.h"

namespace RemixAPI {
    bool Initialize(HWND gameWindow, uint32_t width = 1280, uint32_t height = 720);
    void Shutdown();
    remixapi_Interface* GetInterface();
    bool IsInitialized();

    // Override Remix's overlay-thread RegisterRawInputDevices(RIDEV_NOLEGACY)
    // for keyboard. Remix's dev-menu window registers raw input with NOLEGACY
    // on its overlay thread, which suppresses WM_KEYDOWN/UP for the entire
    // process -- including the game window. We RIDEV_REMOVE that registration
    // (process-wide, last-write-wins) so the game keyboard works again.
    // No-op if g_config.restoreLegacyInput is false. Safe to call repeatedly;
    // RIDEV_REMOVE on an already-removed device is a no-op return.
    void RestoreLegacyKeyboardInput();

    // Re-bind raw-input keyboard+mouse to the game window after Remix init.
    // Win32 raw-input is process-wide last-call-wins per device class: when
    // Remix registers raw input on its dev-menu overlay HWND, it replaces
    // FO4's prior registration on the game HWND, killing in-game input.
    // We re-register on the game HWND (no flags) to claim ownership back.
    // Trade-off: Remix dev-menu hotkeys stop receiving raw input.
    // Idempotent: returns immediately after the first successful call.
    void RebindRawInputToGameWindow(HWND gameWindow);
}
