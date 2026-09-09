#include "camera.h"
#include "bs_extraction.h"
#include "config.h"
#include "semantic_capture.h"  // GetLeafClassName (SEH-guarded RTTI walk)

#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"
#include "f4se/GameCamera.h"
#include "f4se/NiNodes.h"
#include "f4se/NiObjects.h"
#include "f4se/NiTypes.h"

#include <cmath>
#include <cstring>

namespace {

constexpr float kPi = 3.14159265358979323846f;

// ---------------------------------------------------------------------------
// Live NiCamera frustum (FOV fix, 2026-07-03).
//
// The plugin used to pass PlayerCamera::fDefaultWorldFov (the
// fDefaultWorldFOV:Display ini value) straight into Remix's fovYInDegrees.
// That value is the game's HORIZONTAL FOV convention, while the runtime's
// SetupByHalfFovy treats fovYInDegrees as the full VERTICAL FOV -- so Remix
// rendered ~112 degrees horizontal for an 80-degree game setting ("fov is
// different than the original game", 2026-07-02). It is also a static config
// value: aim-down-sights zoom, first-person FOV, and FOV mods never reached
// Remix, and the aspect ratio was hardcoded 16:9.
//
// Ground truth is the live NiCamera's view frustum. F4SE does not declare
// NiCamera, but the FO4 layout is long-established (CommonLibF4 NiCamera):
//   NiAVObject                      // 0x000..0x120 (STATIC_ASSERT'd)
//   float     worldToCam[4][4];     // 0x120
//   NiFrustum viewFrustum;          // 0x160
//   float     minNearPlaneDist;     // 0x17C
//   float     maxFarNearRatio;      // 0x180
//   NiRect<float> port;             // 0x184
//   float     lodAdjust;            // 0x194
// Gamebryo/Creation frustum left/right/top/bottom are UNIT-DISTANCE slopes
// (tangents of the half-angles; engine defaults left/right = +/-1.333,
// top/bottom = +/-1.0). Every read below is sanity-gated so a layout or
// convention surprise degrades to the converted-ini fallback instead of a
// broken image.
// ---------------------------------------------------------------------------
constexpr uintptr_t kNiCameraFrustumOffset = 0x160;

NiAVObject* FindNiCameraChild(NiNode* node) {
    if (!node || !node->m_children.m_data) {
        return nullptr;
    }
    const UInt16 count = node->m_children.m_emptyRunStart;
    for (UInt16 i = 0; i < count && i < 8; ++i) {
        NiAVObject* child = node->m_children.m_data[i];
        if (!child) {
            continue;
        }
        char leaf[64] = "";
        SemanticCapture::GetLeafClassName(child, leaf, sizeof(leaf));
        if (std::strcmp(leaf, "NiCamera") == 0) {
            return child;
        }
    }
    return nullptr;
}

// Returns true and fills fovY/aspect/nearP/farP from the camera's live view
// frustum; false when no NiCamera child exists or the values fail sanity.
bool ReadFrustumFov(NiNode* cameraNode,
                    float& fovYDeg, float& aspect,
                    float& nearP, float& farP) {
    NiAVObject* cam = FindNiCameraChild(cameraNode);
    if (!cam) {
        return false;
    }

    const NiFrustum* fr = reinterpret_cast<const NiFrustum*>(
        reinterpret_cast<uintptr_t>(cam) + kNiCameraFrustumOffset);
    if (fr->m_bOrtho || !(fr->m_fNear > 0.0f) || !(fr->m_fFar > fr->m_fNear)) {
        return false;
    }

    // Unit-distance slopes first (the Gamebryo convention). If a runtime
    // surprise stores extents at the near plane instead, the unit read
    // produces an absurd FOV and the near-scaled read is tried before
    // giving up.
    float tanHalfY = (fr->m_fTop - fr->m_fBottom) * 0.5f;
    float tanHalfX = (fr->m_fRight - fr->m_fLeft) * 0.5f;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1) {
            tanHalfY /= fr->m_fNear;
            tanHalfX /= fr->m_fNear;
        }
        if (tanHalfY > 0.001f && tanHalfX > 0.001f) {
            const float vDeg = 2.0f * std::atan(tanHalfY) * (180.0f / kPi);
            const float asp  = tanHalfX / tanHalfY;
            if (vDeg >= 10.0f && vDeg <= 160.0f && asp >= 0.5f && asp <= 4.0f) {
                fovYDeg = vDeg;
                aspect  = asp;
                // Clamp near/far into a range the path tracer is happy with;
                // the game's fNear (~15) is fine, but never let a transient
                // 0-ish or astronomically large frustum through.
                nearP = fr->m_fNear < 0.1f ? 0.1f : (fr->m_fNear > 50.0f ? 50.0f : fr->m_fNear);
                farP  = fr->m_fFar  < 1000.0f ? 1000.0f
                      : (fr->m_fFar > 10000000.0f ? 10000000.0f : fr->m_fFar);
                return true;
            }
        }
    }
    return false;
}

// Fallback: convert the game's horizontal-convention FOV setting to the
// vertical FOV Remix expects, assuming the 16:9 reference aspect.
float HorizontalToVerticalFov(float hFovDeg, float aspect) {
    const float halfH = hFovDeg * 0.5f * (kPi / 180.0f);
    return 2.0f * std::atan(std::tan(halfH) / aspect) * (180.0f / kPi);
}

} // namespace

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
    state.fov1stY = 70.0f;
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
    // Swap X and Y for position and all rotation vectors
    state.position[0] = xform.pos.y;
    state.position[1] = xform.pos.x;
    state.position[2] = xform.pos.z;

    state.right[0]   = xform.rot.data[0][1];
    state.right[1]   = xform.rot.data[0][0];
    state.right[2]   = xform.rot.data[0][2];

    state.forward[0] = xform.rot.data[1][1];
    state.forward[1] = xform.rot.data[1][0];
    state.forward[2] = xform.rot.data[1][2];

    state.up[0]      = xform.rot.data[2][1];
    state.up[1]      = xform.rot.data[2][0];
    state.up[2]      = xform.rot.data[2][2];

    // Raw Beth-space transform for the viewmodel mapping (no swap).
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            state.rawRot[r][c] = xform.rot.data[r][c];
        }
    }
    state.rawPos[0] = xform.pos.x;
    state.rawPos[1] = xform.pos.y;
    state.rawPos[2] = xform.pos.z;

    // FOV source ladder (see the NiCamera comment block above):
    //   1. Live NiCamera view frustum -- exact vertical FOV + aspect + near/
    //      far, tracks ADS zoom and FOV mods per frame.
    //   2. fDefaultWorldFov converted horizontal->vertical at 16:9.
    //   3. Legacy raw value ([Camera] FovFromFrustum=0 preserves the old
    //      behavior of passing the setting through unconverted).
    bool fromFrustum = false;
    if (g_config.cameraFovFromFrustum) {
        fromFrustum = ReadFrustumFov(cameraNode, state.fovY, state.aspectRatio,
                                     state.nearPlane, state.farPlane);
    }
    if (!fromFrustum) {
        float hFov = playerCam->fDefaultWorldFov;
        if (hFov <= 0.0f || hFov > 170.0f) {
            hFov = 70.0f;
        }
        state.aspectRatio = 16.0f / 9.0f;
        state.fovY = g_config.cameraFovFromFrustum
            ? HorizontalToVerticalFov(hFov, state.aspectRatio)
            : hFov;  // legacy passthrough
        state.nearPlane = 5.0f;
        state.farPlane = 100000.0f;
    }

    // 1st-person FOV for the VIEW_MODEL camera. Same horizontal->vertical
    // conversion as the world fallback path: fDefault1stPersonFOV is the
    // game's horizontal convention (default 80). Sanity-gated; falls back
    // to the world FOV so a bad read can never distort the viewmodel more
    // than the legacy single-camera behavior did.
    {
        state.fov1stY = state.fovY;
        const float hFov1st = playerCam->fDefault1stPersonFOV;
        if (hFov1st >= 10.0f && hFov1st <= 170.0f && state.aspectRatio > 0.1f) {
            const float v = HorizontalToVerticalFov(hFov1st, state.aspectRatio);
            if (v >= 5.0f && v <= 160.0f) {
                state.fov1stY = v;
            }
        }
    }

    // Snapshot player world position (Beth coords, not swapped) for the
    // worldspace LOD chunk spatial filter in OnFrame. Cheap; reads
    // PlayerCharacter+0xD0 with internal null guards. Defaults to (0,0,0)
    // if player is unavailable.
    state.playerWorldPos[0] = 0.0f;
    state.playerWorldPos[1] = 0.0f;
    state.playerWorldPos[2] = 0.0f;
    BsExtraction::GetPlayerPosition(state.playerWorldPos[0],
                                    state.playerWorldPos[1],
                                    state.playerWorldPos[2]);

    state.valid = true;

    // Log camera state once every ~5 seconds (300 frames at 60fps)
    static int s_logCounter = 0;
    if (s_logCounter++ % 300 == 0) {
        _MESSAGE("FO4RemixPlugin: Camera pos=(%.1f, %.1f, %.1f) fwd=(%.3f, %.3f, %.3f) "
                 "up=(%.3f, %.3f, %.3f) right=(%.3f, %.3f, %.3f) fovY=%.1f aspect=%.3f "
                 "near=%.1f far=%.0f src=%s",
                 state.position[0], state.position[1], state.position[2],
                 state.forward[0], state.forward[1], state.forward[2],
                 state.up[0], state.up[1], state.up[2],
                 state.right[0], state.right[1], state.right[2],
                 state.fovY, state.aspectRatio, state.nearPlane, state.farPlane,
                 fromFrustum ? "frustum"
                             : (g_config.cameraFovFromFrustum ? "ini-converted" : "ini-raw"));
    }

    return state;
}
