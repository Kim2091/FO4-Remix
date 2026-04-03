#pragma once

#include "remix/remix_c.h"

namespace RemixAPI {
    bool Initialize(HWND gameWindow, uint32_t width = 1280, uint32_t height = 720);
    void Shutdown();
    remixapi_Interface* GetInterface();
    bool IsInitialized();
}
