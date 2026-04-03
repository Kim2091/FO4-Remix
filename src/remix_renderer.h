#pragma once

#include "camera.h"
#include "scene_extractor.h"
#include <vector>

namespace RemixRenderer {
    bool Init();
    void OnFrame(const CameraState& cam);
    void Shutdown();

    // Upload textures, create materials, and load meshes for a specific cell.
    // Called on the remix thread.
    void LoadCellScene(uint32_t cellFormID, ExtractionResult&& result);

    // Destroy all Remix handles for a specific cell.
    void UnloadCell(uint32_t cellFormID);

    // Destroy all Remix handles for all cells.
    void UnloadAllCells();
}
