#pragma once

#include "camera.h"
#include "scene_extractor.h"
#include <vector>
#include <cstdint>

struct OverlayData {
    std::vector<uint8_t> pixels;  // tightly packed RGBA/BGRA, 4 bpp
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t dxgiFormat = 0;      // DXGI_FORMAT of the captured backbuffer
    bool valid = false;
};

namespace RemixRenderer {
    bool Init();
    void OnFrame(const CameraState& cam, const OverlayData& overlay = {});
    void Shutdown();

    // Upload textures, create materials, and load meshes for a specific cell.
    // Called on the remix thread.
    void LoadCellScene(uint32_t cellFormID, ExtractionResult&& result);

    // Destroy all Remix handles for a specific cell.
    void UnloadCell(uint32_t cellFormID);

    // Destroy all Remix handles for all cells.
    void UnloadAllCells();
}
