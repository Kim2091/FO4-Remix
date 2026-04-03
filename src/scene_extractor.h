#pragma once

#include "remix/remix_c.h"
#include <vector>
#include <cstdint>

struct ExtractedMesh {
    uint64_t hash;
    std::vector<remixapi_HardcodedVertex> vertices;
    std::vector<uint32_t> indices;
    float worldTransform[3][4]; // row-major 3x4 for remixapi_Transform
};

namespace SceneExtractor {
    // Extract all BSTriShape meshes reachable from loaded references
    // in the player's current cell. Must be called on the main thread.
    std::vector<ExtractedMesh> ExtractPlayerCell();
}
