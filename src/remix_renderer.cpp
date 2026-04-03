#include "remix_renderer.h"
#include "remix_api.h"

#include "remix/remix_c.h"

#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"

static remixapi_MeshHandle g_testMesh = nullptr;

bool RemixRenderer::Init() {
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api) return false;

    // Remix needs at least one DrawInstance per frame to trigger path tracing.
    // Create a small triangle in local space (centered near origin, facing -Y).
    // The instance transform will place it in front of the camera each frame.
    remixapi_HardcodedVertex vertices[3] = {};

    vertices[0].position[0] =  50.0f; vertices[0].position[1] = 0.0f; vertices[0].position[2] = -50.0f;
    vertices[0].normal[0] = 0.0f; vertices[0].normal[1] = -1.0f; vertices[0].normal[2] = 0.0f;
    vertices[0].color = 0xFFFFFFFF;

    vertices[1].position[0] =   0.0f; vertices[1].position[1] = 0.0f; vertices[1].position[2] =  50.0f;
    vertices[1].normal[0] = 0.0f; vertices[1].normal[1] = -1.0f; vertices[1].normal[2] = 0.0f;
    vertices[1].color = 0xFFFFFFFF;

    vertices[2].position[0] = -50.0f; vertices[2].position[1] = 0.0f; vertices[2].position[2] = -50.0f;
    vertices[2].normal[0] = 0.0f; vertices[2].normal[1] = -1.0f; vertices[2].normal[2] = 0.0f;
    vertices[2].color = 0xFFFFFFFF;

    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.vertices_values = vertices;
    surface.vertices_count = 3;
    surface.indices_values = nullptr;
    surface.indices_count = 0;
    surface.skinning_hasvalue = 0;
    surface.material = nullptr;

    remixapi_MeshInfo meshInfo = {};
    meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    meshInfo.hash = 0x1;
    meshInfo.surfaces_values = &surface;
    meshInfo.surfaces_count = 1;

    remixapi_ErrorCode status = api->CreateMesh(&meshInfo, &g_testMesh);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        _MESSAGE("FO4RemixPlugin: CreateMesh failed (error %d)", status);
        return false;
    }

    _MESSAGE("FO4RemixPlugin: Renderer initialized, test mesh created");
    return true;
}

void RemixRenderer::OnFrame(const CameraState& cam) {
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api) return;

    remixapi_CameraInfoParameterizedEXT camParams = {};
    camParams.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;

    if (cam.valid) {
        camParams.position    = { cam.position[0], cam.position[1], cam.position[2] };
        camParams.forward     = { cam.forward[0],  cam.forward[1],  cam.forward[2] };
        camParams.up          = { cam.up[0],       cam.up[1],       cam.up[2] };
        camParams.right       = { cam.right[0],    cam.right[1],    cam.right[2] };
        camParams.fovYInDegrees = cam.fovY;
        camParams.aspect      = cam.aspectRatio;
        camParams.nearPlane   = cam.nearPlane;
        camParams.farPlane    = cam.farPlane;
    } else {
        camParams.position    = { 0.0f, 0.0f, 0.0f };
        camParams.forward     = { 0.0f, 0.0f, 1.0f };
        camParams.up          = { 0.0f, 1.0f, 0.0f };
        camParams.right       = { 1.0f, 0.0f, 0.0f };
        camParams.fovYInDegrees = 75.0f;
        camParams.aspect      = 1280.0f / 720.0f;
        camParams.nearPlane   = 0.1f;
        camParams.farPlane    = 1000.0f;
    }

    remixapi_CameraInfo camInfo = {};
    camInfo.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    camInfo.pNext = &camParams;
    camInfo.type = REMIXAPI_CAMERA_TYPE_WORLD;

    api->SetupCamera(&camInfo);

    // Place test triangle 200 units in front of camera
    if (g_testMesh) {
        float px = camParams.position.x + camParams.forward.x * 200.0f;
        float py = camParams.position.y + camParams.forward.y * 200.0f;
        float pz = camParams.position.z + camParams.forward.z * 200.0f;

        remixapi_Transform xform = {};
        xform.matrix[0][0] = 1.0f;
        xform.matrix[1][1] = 1.0f;
        xform.matrix[2][2] = 1.0f;
        xform.matrix[0][3] = px;
        xform.matrix[1][3] = py;
        xform.matrix[2][3] = pz;

        remixapi_InstanceInfo instance = {};
        instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
        instance.mesh = g_testMesh;
        instance.transform = xform;
        instance.doubleSided = 1;
        instance.categoryFlags = 0;

        api->DrawInstance(&instance);
    }

    remixapi_PresentInfo presentInfo = {};
    presentInfo.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
    presentInfo.hwndOverride = nullptr;

    api->Present(&presentInfo);
}

void RemixRenderer::Shutdown() {
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (api && g_testMesh) {
        api->DestroyMesh(g_testMesh);
        g_testMesh = nullptr;
    }
}
