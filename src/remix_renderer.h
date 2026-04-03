#pragma once

#include "camera.h"
#include "scene_extractor.h"
#include <vector>

namespace RemixRenderer {
    bool Init();
    void OnFrame(const CameraState& cam);
    void Shutdown();

    // Upload textures, create materials, and load meshes into Remix.
    // Replaces any previously loaded scene. Called on the remix thread.
    void LoadScene(ExtractionResult&& result);
}
