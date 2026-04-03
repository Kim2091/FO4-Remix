#include "remix_renderer.h"
#include "remix_api.h"

#include "remix/remix_c.h"

#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"

#include <cstring>

static remixapi_MeshHandle g_testMesh = nullptr;
static remixapi_MaterialHandle g_testMaterial = nullptr;

static uint32_t PackColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (uint32_t(b)) | (uint32_t(g) << 8) | (uint32_t(r) << 16) | (uint32_t(a) << 24);
}

bool RemixRenderer::Init() {
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api) return false;

    // Match UnityRTX's CreateTestTriangle exactly:
    // - No material (nullptr)
    // - No indices (indices_count = 0)
    // - Vertices at Z=10, normal (0,0,-1), color white
    remixapi_HardcodedVertex vertices[3] = {};

    // Vertex 0: (5, -5, 10)
    vertices[0].position[0] = 5.0f;
    vertices[0].position[1] = -5.0f;
    vertices[0].position[2] = 10.0f;
    vertices[0].normal[0] = 0.0f;
    vertices[0].normal[1] = 0.0f;
    vertices[0].normal[2] = -1.0f;
    vertices[0].color = 0xFFFFFFFF;

    // Vertex 1: (0, 5, 10)
    vertices[1].position[0] = 0.0f;
    vertices[1].position[1] = 5.0f;
    vertices[1].position[2] = 10.0f;
    vertices[1].normal[0] = 0.0f;
    vertices[1].normal[1] = 0.0f;
    vertices[1].normal[2] = -1.0f;
    vertices[1].color = 0xFFFFFFFF;

    // Vertex 2: (-5, -5, 10)
    vertices[2].position[0] = -5.0f;
    vertices[2].position[1] = -5.0f;
    vertices[2].position[2] = 10.0f;
    vertices[2].normal[0] = 0.0f;
    vertices[2].normal[1] = 0.0f;
    vertices[2].normal[2] = -1.0f;
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

    _MESSAGE("FO4RemixPlugin: Test triangle created (mesh=%p)", g_testMesh);
    return true;
}

void RemixRenderer::OnFrame(const CameraState& cam) {
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api || !g_testMesh) return;

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
        // Test camera: origin, looking down +Z (matches UnityRTX's SetupTestCamera)
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

    remixapi_Transform identity = {};
    identity.matrix[0][0] = 1.0f;
    identity.matrix[1][1] = 1.0f;
    identity.matrix[2][2] = 1.0f;

    remixapi_InstanceInfo instance = {};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.mesh = g_testMesh;
    instance.transform = identity;
    instance.doubleSided = 1;
    instance.categoryFlags = 0;

    api->DrawInstance(&instance);

    remixapi_PresentInfo presentInfo = {};
    presentInfo.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
    presentInfo.hwndOverride = nullptr;

    api->Present(&presentInfo);
}

void RemixRenderer::Shutdown() {
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api) return;

    if (g_testMesh) {
        api->DestroyMesh(g_testMesh);
        g_testMesh = nullptr;
    }
}
