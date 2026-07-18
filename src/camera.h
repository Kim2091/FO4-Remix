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
    // Raw Beth-space cameraNode world transform (NO axis swap): the engine's
    // NiTransform rotation rows (row-vector basis) and translation as-is.
    // Consumed by the viewmodel synthetic->world mapping, which solves
    // S = camBone^-1 * cameraNode and needs the true NiTransform rather
    // than the P-swapped direction vectors above.
    float rawRot[3][3];
    float rawPos[3];
    bool valid;
};

namespace Camera {
    CameraState Get();
}
