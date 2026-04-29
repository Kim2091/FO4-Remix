#pragma once

struct CameraState {
    float position[3];        // Remix coords (Beth y/x swapped)
    float forward[3];
    float up[3];
    float right[3];
    float fovY;               // degrees
    float aspectRatio;
    float nearPlane;
    float farPlane;
    // Player world position in raw Bethesda coords (NOT swapped). Used by
    // the worldspace LOD chunk spatial filter so we can compare against
    // chunk world positions (which are in Beth coords) without re-swapping.
    float playerWorldPos[3];
    bool valid;
};

namespace Camera {
    CameraState Get();
}
