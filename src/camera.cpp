#include "camera.h"

#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"
#include "f4se/GameCamera.h"
#include "f4se/NiNodes.h"
#include "f4se/NiObjects.h"
#include "f4se/NiTypes.h"

static CameraState MakeFallback() {
    CameraState state = {};
    state.position[0] = 0.0f;
    state.position[1] = 0.0f;
    state.position[2] = 200.0f;
    state.forward[0] = 0.0f;
    state.forward[1] = 1.0f;
    state.forward[2] = 0.0f;
    state.up[0] = 0.0f;
    state.up[1] = 0.0f;
    state.up[2] = 1.0f;
    state.right[0] = 1.0f;
    state.right[1] = 0.0f;
    state.right[2] = 0.0f;
    state.fovY = 70.0f;
    state.aspectRatio = 16.0f / 9.0f;
    state.nearPlane = 1.0f;
    state.farPlane = 100000.0f;
    state.valid = false;
    return state;
}

CameraState Camera::Get() {
    // Guard against accessing engine data during load/unload transitions
    // g_playerCamera is a RelocPtr — dereference carefully
    PlayerCamera** ppCam = reinterpret_cast<PlayerCamera**>(g_playerCamera.GetPtr());
    if (!ppCam || !*ppCam) {
        return MakeFallback();
    }

    PlayerCamera* playerCam = *ppCam;
    NiNode* cameraNode = playerCam->cameraNode;
    if (!cameraNode) {
        return MakeFallback();
    }

    const NiTransform& xform = cameraNode->m_worldTransform;

    CameraState state = {};
    state.position[0] = xform.pos.x;
    state.position[1] = xform.pos.y;
    state.position[2] = xform.pos.z;

    state.right[0]   = xform.rot.data[0][0];
    state.right[1]   = xform.rot.data[0][1];
    state.right[2]   = xform.rot.data[0][2];

    state.forward[0] = xform.rot.data[1][0];
    state.forward[1] = xform.rot.data[1][1];
    state.forward[2] = xform.rot.data[1][2];

    state.up[0]      = xform.rot.data[2][0];
    state.up[1]      = xform.rot.data[2][1];
    state.up[2]      = xform.rot.data[2][2];

    state.fovY = playerCam->fDefaultWorldFov;
    if (state.fovY <= 0.0f || state.fovY > 170.0f) {
        state.fovY = 70.0f;
    }

    state.aspectRatio = 16.0f / 9.0f;
    state.nearPlane = 5.0f;
    state.farPlane = 100000.0f;
    state.valid = true;

    return state;
}
