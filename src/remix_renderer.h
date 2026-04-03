#pragma once

#include "camera.h"
#include "scene_extractor.h"
#include <vector>

namespace RemixRenderer {
    bool Init();
    void OnFrame(const CameraState& cam);
    void Shutdown();

    // Load extracted meshes into Remix (called on remix thread).
    // Replaces any previously loaded scene meshes.
    void LoadSceneMeshes(std::vector<ExtractedMesh>&& meshes);
}
