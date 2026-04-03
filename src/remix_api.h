#pragma once

#include "remix/remix_c.h"

namespace RemixAPI {
    bool Initialize(HWND gameWindow);
    void Shutdown();
    remixapi_Interface* GetInterface();
    bool IsInitialized();
}
