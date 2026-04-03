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

    remixapi_MaterialInfo matInfo = {};
    matInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    matInfo.hash = 0xF04F04F04;

    remixapi_ErrorCode status = api->CreateMaterial(&matInfo, &g_testMaterial);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        _MESSAGE("FO4RemixPlugin: CreateMaterial failed (error %d)", status);
        return false;
    }

    remixapi_HardcodedVertex vertices[3] = {};

    vertices[0].position[0] = -100.0f;
    vertices[0].position[1] = 0.0f;
    vertices[0].position[2] = 0.0f;
    vertices[0].normal[0] = 0.0f;
    vertices[0].normal[1] = 0.0f;
    vertices[0].normal[2] = 1.0f;
    vertices[0].texcoord[0] = 0.0f;
    vertices[0].texcoord[1] = 0.0f;
    vertices[0].color = PackColor(255, 0, 0);

    vertices[1].position[0] = 100.0f;
    vertices[1].position[1] = 0.0f;
    vertices[1].position[2] = 0.0f;
    vertices[1].normal[0] = 0.0f;
    vertices[1].normal[1] = 0.0f;
    vertices[1].normal[2] = 1.0f;
    vertices[1].texcoord[0] = 1.0f;
    vertices[1].texcoord[1] = 0.0f;
    vertices[1].color = PackColor(0, 255, 0);

    vertices[2].position[0] = 0.0f;
    vertices[2].position[1] = 0.0f;
    vertices[2].position[2] = 200.0f;
    vertices[2].normal[0] = 0.0f;
    vertices[2].normal[1] = 0.0f;
    vertices[2].normal[2] = 1.0f;
    vertices[2].texcoord[0] = 0.5f;
    vertices[2].texcoord[1] = 1.0f;
    vertices[2].color = PackColor(0, 0, 255);

    uint32_t indices[3] = { 0, 1, 2 };

    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.vertices_values = vertices;
    surface.vertices_count = 3;
    surface.indices_values = indices;
    surface.indices_count = 3;
    surface.skinning_hasvalue = 0;
    surface.material = g_testMaterial;

    remixapi_MeshInfo meshInfo = {};
    meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    meshInfo.hash = 0xF04E5401;
    meshInfo.surfaces_values = &surface;
    meshInfo.surfaces_count = 1;

    status = api->CreateMesh(&meshInfo, &g_testMesh);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        _MESSAGE("FO4RemixPlugin: CreateMesh failed (error %d)", status);
        return false;
    }

    _MESSAGE("FO4RemixPlugin: Test triangle created (mesh=%p, material=%p)", g_testMesh, g_testMaterial);
    return true;
}

void RemixRenderer::OnFrame(const CameraState& cam) {
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api || !g_testMesh) return;

    remixapi_CameraInfoParameterizedEXT camParams = {};
    camParams.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    camParams.position    = { cam.position[0], cam.position[1], cam.position[2] };
    camParams.forward     = { cam.forward[0],  cam.forward[1],  cam.forward[2] };
    camParams.up          = { cam.up[0],       cam.up[1],       cam.up[2] };
    camParams.right       = { cam.right[0],    cam.right[1],    cam.right[2] };
    camParams.fovYInDegrees = cam.fovY;
    camParams.aspect      = cam.aspectRatio;
    camParams.nearPlane   = cam.nearPlane;
    camParams.farPlane    = cam.farPlane;

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
    if (g_testMaterial) {
        api->DestroyMaterial(g_testMaterial);
        g_testMaterial = nullptr;
    }
}
