#include "remix_renderer.h"
#include "remix_api.h"

#include "remix/remix_c.h"

#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <d3d11.h>

// ---------------------------------------------------------------------------
// DXGI -> remixapi_Format mapping
// ---------------------------------------------------------------------------
static remixapi_Format DxgiToRemixFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_BC1_UNORM:         return REMIXAPI_FORMAT_BC1_RGB_UNORM;
        case DXGI_FORMAT_BC1_UNORM_SRGB:    return REMIXAPI_FORMAT_BC1_RGB_SRGB;
        case DXGI_FORMAT_BC3_UNORM:         return REMIXAPI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC3_UNORM_SRGB:    return REMIXAPI_FORMAT_BC3_SRGB;
        case DXGI_FORMAT_BC5_UNORM:         return REMIXAPI_FORMAT_BC5_UNORM;
        case DXGI_FORMAT_BC5_SNORM:         return REMIXAPI_FORMAT_BC5_UNORM; // best available
        case DXGI_FORMAT_BC7_UNORM:         return REMIXAPI_FORMAT_BC7_UNORM;
        case DXGI_FORMAT_BC7_UNORM_SRGB:    return REMIXAPI_FORMAT_BC7_SRGB;
        case DXGI_FORMAT_R8G8B8A8_UNORM:    return REMIXAPI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return REMIXAPI_FORMAT_R8G8B8A8_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM:    return REMIXAPI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return REMIXAPI_FORMAT_B8G8R8A8_SRGB;
        default: return (remixapi_Format)0;
    }
}

// ---------------------------------------------------------------------------
// Hash -> wstring path for Remix texture references
// ---------------------------------------------------------------------------
static std::wstring HashToPath(uint64_t hash) {
    wchar_t buf[32];
    swprintf(buf, 32, L"0x%llX", (unsigned long long)hash);
    return std::wstring(buf);
}

// ---------------------------------------------------------------------------
// Scene tracking state
// ---------------------------------------------------------------------------
struct SceneMeshInstance {
    remixapi_MeshHandle handle;
    remixapi_Transform  transform;
};

static std::vector<SceneMeshInstance> g_sceneMeshes;
static std::unordered_map<uint64_t, remixapi_TextureHandle> g_textureHandles;
static std::unordered_map<uint64_t, remixapi_MaterialHandle> g_materialHandles;
static std::vector<remixapi_LightHandle> g_lightHandles;

// Fallback triangle (keeps path tracing alive when no scene meshes are loaded)
static remixapi_MeshHandle g_fallbackMesh = nullptr;

bool RemixRenderer::Init() {
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api) return false;

    // Create fallback triangle
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
    meshInfo.hash = 0xFA11BAC0;
    meshInfo.surfaces_values = &surface;
    meshInfo.surfaces_count = 1;

    api->CreateMesh(&meshInfo, &g_fallbackMesh);

    _MESSAGE("FO4RemixPlugin: Renderer initialized");
    return true;
}

// ---------------------------------------------------------------------------
// Load extracted textures, materials, and meshes into Remix
// ---------------------------------------------------------------------------
void RemixRenderer::LoadScene(ExtractionResult&& result) {
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api) return;

    // ----- Destroy previous state -----
    for (auto& inst : g_sceneMeshes) {
        if (inst.handle) api->DestroyMesh(inst.handle);
    }
    g_sceneMeshes.clear();

    for (auto& [hash, handle] : g_materialHandles) {
        if (handle) api->DestroyMaterial(handle);
    }
    g_materialHandles.clear();

    for (auto& [hash, handle] : g_textureHandles) {
        if (handle) api->DestroyTexture(handle);
    }
    g_textureHandles.clear();

    for (auto& handle : g_lightHandles) {
        if (handle) api->DestroyLight(handle);
    }
    g_lightHandles.clear();

    // ----- Upload textures -----
    uint32_t texCreated = 0, texFailed = 0, texSkipped = 0;

    for (auto& tex : result.textures) {
        remixapi_Format remixFmt = DxgiToRemixFormat(tex.dxgiFormat);
        if (remixFmt == (remixapi_Format)0) {
            _MESSAGE("FO4RemixPlugin: Skipping texture 0x%llX - unsupported DXGI format %u",
                     (unsigned long long)tex.hash, (unsigned int)tex.dxgiFormat);
            texSkipped++;
            continue;
        }

        remixapi_TextureInfo texInfo = {};
        texInfo.sType     = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
        texInfo.pNext     = nullptr;
        texInfo.hash      = tex.hash;
        texInfo.width     = tex.width;
        texInfo.height    = tex.height;
        texInfo.depth     = 1;
        texInfo.mipLevels = 1;
        texInfo.format    = remixFmt;
        texInfo.data      = tex.pixels.data();
        texInfo.dataSize  = tex.pixels.size();

        remixapi_TextureHandle handle = nullptr;
        remixapi_ErrorCode status = api->CreateTexture(&texInfo, &handle);
        if (status != REMIXAPI_ERROR_CODE_SUCCESS || !handle) {
            _MESSAGE("FO4RemixPlugin: Failed to upload texture 0x%llX (error %d)",
                     (unsigned long long)tex.hash, (int)status);
            texFailed++;
            continue;
        }

        g_textureHandles[tex.hash] = handle;
        texCreated++;
    }

    _MESSAGE("FO4RemixPlugin: Textures - uploaded %u, failed %u, skipped %u",
             texCreated, texFailed, texSkipped);

    // ----- Create materials (one per unique diffuseTextureHash) -----
    std::unordered_set<uint64_t> materialHashes;
    for (auto& mesh : result.meshes) {
        if (mesh.diffuseTextureHash != 0)
            materialHashes.insert(mesh.diffuseTextureHash);
    }

    uint32_t matCreated = 0, matFailed = 0;

    for (uint64_t diffHash : materialHashes) {
        std::wstring hashPath = HashToPath(diffHash);

        remixapi_MaterialInfoOpaqueEXT opaqueExt = {};
        opaqueExt.sType             = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
        opaqueExt.pNext             = nullptr;
        opaqueExt.roughnessTexture  = nullptr;
        opaqueExt.metallicTexture   = nullptr;
        opaqueExt.heightTexture     = nullptr;
        opaqueExt.albedoConstant    = { 1.0f, 1.0f, 1.0f };
        opaqueExt.opacityConstant   = 1.0f;
        opaqueExt.roughnessConstant = 0.5f;
        opaqueExt.metallicConstant  = 0.0f;
        opaqueExt.alphaTestType     = 7; // AlphaTestType::kAlways - 0 is kNever which discards all pixels!

        remixapi_MaterialInfo matInfo = {};
        matInfo.sType              = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
        matInfo.pNext              = &opaqueExt;
        matInfo.hash               = diffHash;
        matInfo.albedoTexture      = hashPath.c_str();
        matInfo.normalTexture      = nullptr;
        matInfo.tangentTexture     = nullptr;
        matInfo.emissiveTexture    = nullptr;
        matInfo.emissiveIntensity  = 0.0f;
        matInfo.emissiveColorConstant = { 0.0f, 0.0f, 0.0f };
        matInfo.spriteSheetRow     = 1;
        matInfo.spriteSheetCol     = 1;
        matInfo.spriteSheetFps     = 0;
        matInfo.filterMode         = 1;
        matInfo.wrapModeU          = 0;
        matInfo.wrapModeV          = 0;

        remixapi_MaterialHandle handle = nullptr;
        remixapi_ErrorCode status = api->CreateMaterial(&matInfo, &handle);
        if (status != REMIXAPI_ERROR_CODE_SUCCESS || !handle) {
            _MESSAGE("FO4RemixPlugin: Failed to create material for texture 0x%llX (error %d)",
                     (unsigned long long)diffHash, (int)status);
            matFailed++;
            continue;
        }

        g_materialHandles[diffHash] = handle;
        matCreated++;
    }

    _MESSAGE("FO4RemixPlugin: Materials - created %u, failed %u", matCreated, matFailed);

    // ----- Create meshes -----
    uint32_t meshCreated = 0, meshFailed = 0;

    for (auto& mesh : result.meshes) {
        if (mesh.vertices.empty()) continue;

        // Look up material handle for this mesh's diffuse texture
        remixapi_MaterialHandle matHandle = nullptr;
        if (mesh.diffuseTextureHash != 0) {
            auto it = g_materialHandles.find(mesh.diffuseTextureHash);
            if (it != g_materialHandles.end())
                matHandle = it->second;
        }

        remixapi_MeshInfoSurfaceTriangles surface = {};
        surface.vertices_values = mesh.vertices.data();
        surface.vertices_count = (uint32_t)mesh.vertices.size();
        surface.indices_values = mesh.indices.empty() ? nullptr : mesh.indices.data();
        surface.indices_count = (uint32_t)mesh.indices.size();
        surface.skinning_hasvalue = 0;
        surface.material = matHandle;

        remixapi_MeshInfo meshInfo = {};
        meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
        meshInfo.hash = mesh.hash;
        meshInfo.surfaces_values = &surface;
        meshInfo.surfaces_count = 1;

        remixapi_MeshHandle handle = nullptr;
        remixapi_ErrorCode status = api->CreateMesh(&meshInfo, &handle);
        if (status != REMIXAPI_ERROR_CODE_SUCCESS || !handle) {
            meshFailed++;
            continue;
        }

        SceneMeshInstance inst;
        inst.handle = handle;
        memcpy(&inst.transform.matrix, mesh.worldTransform, sizeof(mesh.worldTransform));

        g_sceneMeshes.push_back(inst);
        meshCreated++;
    }

    _MESSAGE("FO4RemixPlugin: Meshes - created %u, failed %u", meshCreated, meshFailed);

    // ----- Create lights -----
    uint32_t lightCreated = 0, lightFailed = 0;

    for (auto& light : result.lights) {
        remixapi_LightInfoSphereEXT sphere = {};
        sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
        sphere.pNext = nullptr;
        sphere.position = { light.position[0], light.position[1], light.position[2] };
        sphere.radius = 0.1f; // Small emitter sphere (not the attenuation range)
        sphere.volumetricRadianceScale = 1.0f;

        if (light.isSpotLight && light.spotFOV > 0.0f) {
            sphere.shaping_hasvalue = true;
            sphere.shaping_value.direction = {
                light.spotDirection[0],
                light.spotDirection[1],
                light.spotDirection[2]
            };
            sphere.shaping_value.coneAngleDegrees = light.spotFOV * 0.5f; // FO4 FOV is full angle
            sphere.shaping_value.coneSoftness = light.spotSoftness;
            sphere.shaping_value.focusExponent = 0.0f;
        } else {
            sphere.shaping_hasvalue = false;
        }

        remixapi_LightInfo info = {};
        info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
        info.pNext = &sphere;
        info.hash = light.hash;
        info.radiance = { light.radiance[0], light.radiance[1], light.radiance[2] };
        info.isDynamic = false;
        info.ignoreViewModel = false;

        remixapi_LightHandle handle = nullptr;
        remixapi_ErrorCode status = api->CreateLight(&info, &handle);
        if (status != REMIXAPI_ERROR_CODE_SUCCESS || !handle) {
            lightFailed++;
            continue;
        }

        g_lightHandles.push_back(handle);
        lightCreated++;
    }

    _MESSAGE("FO4RemixPlugin: Lights - created %u, failed %u", lightCreated, lightFailed);
}

// ---------------------------------------------------------------------------
// Per-frame rendering
// ---------------------------------------------------------------------------
void RemixRenderer::OnFrame(const CameraState& cam) {
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api) return;

    // Camera setup
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
        camParams.position      = { 0.0f, 0.0f, 0.0f };
        camParams.forward       = { 0.0f, 0.0f, 1.0f };
        camParams.up            = { 0.0f, 1.0f, 0.0f };
        camParams.right         = { 1.0f, 0.0f, 0.0f };
        camParams.fovYInDegrees = 75.0f;
        camParams.aspect        = 1280.0f / 720.0f;
        camParams.nearPlane     = 0.1f;
        camParams.farPlane      = 1000.0f;
    }

    remixapi_CameraInfo camInfo = {};
    camInfo.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    camInfo.pNext = &camParams;
    camInfo.type = REMIXAPI_CAMERA_TYPE_WORLD;
    api->SetupCamera(&camInfo);

    // Draw scene meshes
    if (!g_sceneMeshes.empty()) {
        for (auto& inst : g_sceneMeshes) {
            remixapi_InstanceInfo instance = {};
            instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
            instance.mesh = inst.handle;
            instance.transform = inst.transform;
            instance.doubleSided = 1;
            instance.categoryFlags = 0;
            api->DrawInstance(&instance);
        }

        // Draw lights
        for (auto& handle : g_lightHandles) {
            api->DrawLightInstance(handle);
        }
    } else if (g_fallbackMesh) {
        // Fallback: place triangle in front of camera to keep path tracing alive
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
        instance.mesh = g_fallbackMesh;
        instance.transform = xform;
        instance.doubleSided = 1;
        instance.categoryFlags = 0;
        api->DrawInstance(&instance);
    }

    // Present
    remixapi_PresentInfo presentInfo = {};
    presentInfo.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
    presentInfo.hwndOverride = nullptr;
    api->Present(&presentInfo);
}

void RemixRenderer::Shutdown() {
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api) return;

    for (auto& inst : g_sceneMeshes) {
        if (inst.handle) api->DestroyMesh(inst.handle);
    }
    g_sceneMeshes.clear();

    for (auto& [hash, handle] : g_materialHandles) {
        if (handle) api->DestroyMaterial(handle);
    }
    g_materialHandles.clear();

    for (auto& [hash, handle] : g_textureHandles) {
        if (handle) api->DestroyTexture(handle);
    }
    g_textureHandles.clear();

    for (auto& handle : g_lightHandles) {
        if (handle) api->DestroyLight(handle);
    }
    g_lightHandles.clear();

    if (g_fallbackMesh) {
        api->DestroyMesh(g_fallbackMesh);
        g_fallbackMesh = nullptr;
    }
}
