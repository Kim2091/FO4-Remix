#pragma once

#include "camera.h"

namespace RemixRenderer {
    bool Init();
    void OnFrame(const CameraState& cam);
    void Shutdown();
}
