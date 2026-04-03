#pragma once

struct CameraState {
    float position[3];
    float forward[3];
    float up[3];
    float right[3];
    float fovY;         // degrees
    float aspectRatio;
    float nearPlane;
    float farPlane;
    bool valid;
};

namespace Camera {
    CameraState Get();
}
