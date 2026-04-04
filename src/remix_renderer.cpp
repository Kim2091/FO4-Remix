#include "remix_renderer.h"
#include "config.h"
#include "remix_api.h"

#include "remix/remix_c.h"

#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"

#include <algorithm>
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
    bool isSkinned = false;
    uint64_t meshHash = 0;
};

struct CellSceneData {
    std::vector<SceneMeshInstance> meshes;
    std::unordered_map<uint64_t, remixapi_MaterialHandle> materials;
    std::vector<remixapi_LightHandle> lights;
    std::unordered_set<uint64_t> textureHashes;
};

static std::unordered_map<uint32_t, CellSceneData> g_cellScenes;

// Textures are shared across cells (same texture may appear in multiple cells)
// so keep them global with a reference count
struct TextureRef {
    remixapi_TextureHandle handle;
    uint32_t refCount;
};
static std::unordered_map<uint64_t, TextureRef> g_textureHandles;

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
// Unload all Remix handles for a specific cell
// ---------------------------------------------------------------------------
void RemixRenderer::UnloadCell(uint32_t cellFormID) {
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api) return;

    auto it = g_cellScenes.find(cellFormID);
    if (it == g_cellScenes.end()) return;

    CellSceneData& cell = it->second;

    for (auto& inst : cell.meshes) {
        if (inst.handle) api->DestroyMesh(inst.handle);
    }

    for (auto& [hash, handle] : cell.materials) {
        if (handle) api->DestroyMaterial(handle);
    }

    for (auto& handle : cell.lights) {
        if (handle) api->DestroyLight(handle);
    }

    // Decrement texture refcounts; destroy if refCount hits 0
    for (uint64_t texHash : cell.textureHashes) {
        auto texIt = g_textureHandles.find(texHash);
        if (texIt != g_textureHandles.end()) {
            texIt->second.refCount--;
            if (texIt->second.refCount == 0) {
                if (texIt->second.handle) api->DestroyTexture(texIt->second.handle);
                g_textureHandles.erase(texIt);
            }
        }
    }

    g_cellScenes.erase(it);
}

// ---------------------------------------------------------------------------
// Unload all cells and destroy all Remix handles
// ---------------------------------------------------------------------------
void RemixRenderer::UnloadAllCells() {
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api) return;

    for (auto& [cellID, cell] : g_cellScenes) {
        for (auto& inst : cell.meshes) {
            if (inst.handle) api->DestroyMesh(inst.handle);
        }
        for (auto& [hash, handle] : cell.materials) {
            if (handle) api->DestroyMaterial(handle);
        }
        for (auto& handle : cell.lights) {
            if (handle) api->DestroyLight(handle);
        }
    }
    g_cellScenes.clear();

    for (auto& [hash, texRef] : g_textureHandles) {
        if (texRef.handle) api->DestroyTexture(texRef.handle);
    }
    g_textureHandles.clear();
}

// ---------------------------------------------------------------------------
// Load extracted textures, materials, and meshes for a specific cell
// ---------------------------------------------------------------------------
void RemixRenderer::LoadCellScene(uint32_t cellFormID, ExtractionResult&& result) {
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api) return;

    // If this cell already exists, clean up old data first.
    // Preserve existing lights during refreshes (empty lights in new data = refresh).
    std::vector<remixapi_LightHandle> preservedLights;
    if (g_cellScenes.find(cellFormID) != g_cellScenes.end()) {
        if (result.lights.empty()) {
            preservedLights = std::move(g_cellScenes[cellFormID].lights);
            g_cellScenes[cellFormID].lights.clear();
        }
        UnloadCell(cellFormID);
    }

    CellSceneData cellData;

    // ----- Upload textures (shared across cells with refcounting) -----
    uint32_t texCreated = 0, texFailed = 0, texSkipped = 0;

    for (auto& tex : result.textures) {
        auto existing = g_textureHandles.find(tex.hash);
        if (existing != g_textureHandles.end()) {
            // Texture already loaded by another cell, just bump refcount
            existing->second.refCount++;
            cellData.textureHashes.insert(tex.hash);
            continue;
        }

        remixapi_Format remixFmt = DxgiToRemixFormat(tex.dxgiFormat);
        if (remixFmt == (remixapi_Format)0) {
            _MESSAGE("FO4RemixPlugin: [Cell 0x%X] Skipping texture 0x%llX - unsupported DXGI format %u",
                     cellFormID, (unsigned long long)tex.hash, (unsigned int)tex.dxgiFormat);
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
            _MESSAGE("FO4RemixPlugin: [Cell 0x%X] Failed to upload texture 0x%llX (error %d)",
                     cellFormID, (unsigned long long)tex.hash, (int)status);
            texFailed++;
            continue;
        }

        g_textureHandles[tex.hash] = { handle, 1 };
        cellData.textureHashes.insert(tex.hash);
        texCreated++;
    }

    _MESSAGE("FO4RemixPlugin: [Cell 0x%X] Textures - uploaded %u, failed %u, skipped %u",
             cellFormID, texCreated, texFailed, texSkipped);

    // ----- Create materials (one per unique texture combination) -----
    // Key materials by a combined hash of diffuse+normal+roughness so meshes
    // with different PBR maps get separate materials.
    struct MaterialKey {
        uint64_t diffuse;
        uint64_t normal;
        uint64_t roughness;
        bool alphaTestEnabled;
        int alphaTestType;       // Remix/VkCompareOp
        uint8_t alphaTestRef;
        uint64_t combined() const {
            uint64_t h = diffuse;
            h ^= normal  * 0x517CC1B727220A95ULL;
            h ^= roughness * 0x6C62272E07BB0142ULL;
            return h;
        }
    };

    std::unordered_map<uint64_t, MaterialKey> materialKeys;
    for (auto& mesh : result.meshes) {
        if (mesh.diffuseTextureHash == 0) continue;
        MaterialKey key { mesh.diffuseTextureHash, mesh.normalTextureHash, mesh.roughnessTextureHash,
                          mesh.alphaTestEnabled, mesh.alphaTestType, mesh.alphaTestRef };
        auto it = materialKeys.find(key.combined());
        if (it == materialKeys.end()) {
            materialKeys.emplace(key.combined(), key);
        } else if (mesh.alphaTestEnabled && !it->second.alphaTestEnabled) {
            // If any mesh using this material needs alpha testing, enable it
            it->second.alphaTestEnabled = true;
            it->second.alphaTestType = mesh.alphaTestType;
            it->second.alphaTestRef = mesh.alphaTestRef;
        }
    }

    uint32_t matCreated = 0, matFailed = 0;

    for (auto& [combinedHash, key] : materialKeys) {
        std::wstring diffPath = HashToPath(key.diffuse);
        std::wstring normalPath = key.normal ? HashToPath(key.normal) : L"";
        std::wstring roughPath = key.roughness ? HashToPath(key.roughness) : L"";

        remixapi_MaterialInfoOpaqueEXT opaqueExt = {};
        opaqueExt.sType             = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
        opaqueExt.pNext             = nullptr;
        opaqueExt.roughnessTexture  = key.roughness ? roughPath.c_str() : nullptr;
        opaqueExt.metallicTexture   = nullptr;
        opaqueExt.heightTexture     = nullptr;
        opaqueExt.albedoConstant    = { 1.0f, 1.0f, 1.0f };
        opaqueExt.opacityConstant   = 1.0f;
        opaqueExt.roughnessConstant = key.roughness ? 0.5f : 0.8f;
        opaqueExt.metallicConstant  = 0.0f;
        opaqueExt.alphaTestType     = key.alphaTestEnabled ? key.alphaTestType : 7;
        opaqueExt.alphaReferenceValue = key.alphaTestEnabled ? key.alphaTestRef : 0;

        remixapi_MaterialInfo matInfo = {};
        matInfo.sType              = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
        matInfo.pNext              = &opaqueExt;
        matInfo.hash               = combinedHash;
        matInfo.albedoTexture      = diffPath.c_str();
        matInfo.normalTexture      = key.normal ? normalPath.c_str() : nullptr;
        matInfo.tangentTexture     = nullptr;
        matInfo.emissiveTexture    = nullptr;
        matInfo.emissiveIntensity  = 0.0f;
        matInfo.emissiveColorConstant = { 0.0f, 0.0f, 0.0f };
        matInfo.spriteSheetRow     = 1;
        matInfo.spriteSheetCol     = 1;
        matInfo.spriteSheetFps     = 0;
        matInfo.filterMode         = 1;
        matInfo.wrapModeU          = 1;  // Repeat (0 = Clamp)
        matInfo.wrapModeV          = 1;

        remixapi_MaterialHandle handle = nullptr;
        remixapi_ErrorCode status = api->CreateMaterial(&matInfo, &handle);
        if (status != REMIXAPI_ERROR_CODE_SUCCESS || !handle) {
            _MESSAGE("FO4RemixPlugin: [Cell 0x%X] Failed to create material 0x%llX (error %d)",
                     cellFormID, (unsigned long long)combinedHash, (int)status);
            matFailed++;
            continue;
        }

        cellData.materials[combinedHash] = handle;
        matCreated++;
    }

    _MESSAGE("FO4RemixPlugin: [Cell 0x%X] Materials - created %u, failed %u",
             cellFormID, matCreated, matFailed);

    // ----- Create meshes -----
    uint32_t meshCreated = 0, meshFailed = 0;

    for (auto& mesh : result.meshes) {
        if (mesh.vertices.empty()) continue;

        // Look up material handle by combined texture hash
        remixapi_MaterialHandle matHandle = nullptr;
        if (mesh.diffuseTextureHash != 0) {
            uint64_t combinedHash = mesh.diffuseTextureHash;
            combinedHash ^= mesh.normalTextureHash    * 0x517CC1B727220A95ULL;
            combinedHash ^= mesh.roughnessTextureHash * 0x6C62272E07BB0142ULL;
            auto it = cellData.materials.find(combinedHash);
            if (it != cellData.materials.end())
                matHandle = it->second;
        }

        remixapi_MeshInfoSurfaceTriangles surface = {};
        surface.vertices_values = mesh.vertices.data();
        surface.vertices_count = (uint32_t)mesh.vertices.size();
        surface.indices_values = mesh.indices.empty() ? nullptr : mesh.indices.data();
        surface.indices_count = (uint32_t)mesh.indices.size();
        surface.material = matHandle;

        if (mesh.bonesPerVertex > 0 && !mesh.blendWeights.empty()) {
            surface.skinning_hasvalue = 1;
            surface.skinning_value.bonesPerVertex = mesh.bonesPerVertex;
            surface.skinning_value.blendWeights_values = mesh.blendWeights.data();
            surface.skinning_value.blendWeights_count = (uint32_t)mesh.blendWeights.size();
            surface.skinning_value.blendIndices_values = mesh.blendIndices.data();
            surface.skinning_value.blendIndices_count = (uint32_t)mesh.blendIndices.size();
        } else {
            surface.skinning_hasvalue = 0;
        }

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
        inst.isSkinned = (mesh.bonesPerVertex > 0);
        inst.meshHash = mesh.hash;

        cellData.meshes.push_back(inst);
        meshCreated++;
    }

    _MESSAGE("FO4RemixPlugin: [Cell 0x%X] Meshes - created %u, failed %u",
             cellFormID, meshCreated, meshFailed);

    // ----- Create lights -----
    uint32_t lightCreated = 0, lightFailed = 0;

    if (!g_config.lightsEnabled) {
        _MESSAGE("FO4RemixPlugin: Lights disabled by config");
    }

    for (auto& light : result.lights) {
        if (!g_config.lightsEnabled) break;

        remixapi_LightInfoSphereEXT sphere = {};
        sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
        sphere.pNext = nullptr;
        sphere.position = { light.position[0], light.position[1], light.position[2] };
        sphere.radius = (std::max)(light.radius * 0.025f * g_config.lightRadius, 0.5f);
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

        // Apply intensity multiplier and color strength
        float cs = g_config.lightColorStrength;
        float intensity = g_config.lightIntensity;
        float r = light.radiance[0], g = light.radiance[1], b = light.radiance[2];
        if (cs < 1.0f) {
            // Lerp toward white (average luminance) as color strength decreases
            float avg = (r + g + b) / 3.0f;
            r = avg + (r - avg) * cs;
            g = avg + (g - avg) * cs;
            b = avg + (b - avg) * cs;
        }

        remixapi_LightInfo info = {};
        info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
        info.pNext = &sphere;
        info.hash = light.hash;
        info.radiance = { r * intensity, g * intensity, b * intensity };
        info.isDynamic = false;
        info.ignoreViewModel = false;

        if (lightCreated < 3) {
            _MESSAGE("FO4RemixPlugin: [Cell 0x%X] Light[%u] configIntensity=%.4f origRadiance=(%.1f,%.1f,%.1f) "
                     "finalRadiance=(%.1f,%.1f,%.1f) radius=%.1f",
                     cellFormID, lightCreated, intensity,
                     light.radiance[0], light.radiance[1], light.radiance[2],
                     info.radiance.x, info.radiance.y, info.radiance.z,
                     sphere.radius);
        }

        remixapi_LightHandle handle = nullptr;
        remixapi_ErrorCode status = api->CreateLight(&info, &handle);
        if (status != REMIXAPI_ERROR_CODE_SUCCESS || !handle) {
            lightFailed++;
            continue;
        }

        cellData.lights.push_back(handle);
        lightCreated++;
    }

    // Restore preserved lights from refresh (they weren't destroyed)
    if (!preservedLights.empty()) {
        cellData.lights = std::move(preservedLights);
        _MESSAGE("FO4RemixPlugin: [Cell 0x%X] Lights - preserved %zu from refresh",
                 cellFormID, cellData.lights.size());
    } else {
        _MESSAGE("FO4RemixPlugin: [Cell 0x%X] Lights - created %u, failed %u",
                 cellFormID, lightCreated, lightFailed);
    }

    g_cellScenes[cellFormID] = std::move(cellData);
}

// ---------------------------------------------------------------------------
// Per-frame rendering
// ---------------------------------------------------------------------------
void RemixRenderer::OnFrame(const CameraState& cam, const OverlayData& overlay,
                            const std::vector<SkinnedMeshBones>& bones) {
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

    // Build bone transform lookup by mesh hash
    std::unordered_map<uint64_t, const std::vector<remixapi_Transform>*> boneLookup;
    for (const auto& entry : bones) {
        boneLookup[entry.meshHash] = &entry.bones;
    }

    // Draw scene meshes from all loaded cells
    bool hasAnyMeshes = false;
    uint32_t totalLightsDrawn = 0;
    uint32_t totalLightsFailed = 0;
    for (auto& [cellID, cellData] : g_cellScenes) {
        for (auto& inst : cellData.meshes) {
            remixapi_InstanceInfoBoneTransformsEXT boneExt = {};
            remixapi_InstanceInfo instance = {};
            instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
            instance.mesh = inst.handle;
            instance.transform = inst.transform;
            instance.doubleSided = 1;
            instance.categoryFlags = 0;

            if (inst.isSkinned) {
                auto it = boneLookup.find(inst.meshHash);
                if (it != boneLookup.end() && !it->second->empty()) {
                    boneExt.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
                    boneExt.pNext = nullptr;
                    boneExt.boneTransforms_values = it->second->data();
                    boneExt.boneTransforms_count = (uint32_t)it->second->size();
                    instance.pNext = &boneExt;
                }
            }

            api->DrawInstance(&instance);
            hasAnyMeshes = true;
        }
        for (auto& handle : cellData.lights) {
            remixapi_ErrorCode lightErr = api->DrawLightInstance(handle);
            if (lightErr == REMIXAPI_ERROR_CODE_SUCCESS) {
                totalLightsDrawn++;
            } else {
                totalLightsFailed++;
            }
        }
    }

    // Periodic light status log (every ~5 seconds)
    static uint32_t s_frameCounter = 0;
    s_frameCounter++;
    if (s_frameCounter % 300 == 0) {
        _MESSAGE("FO4RemixPlugin: OnFrame light status - %zu cells, %u lights drawn, %u lights failed",
                 g_cellScenes.size(), totalLightsDrawn, totalLightsFailed);
    }

    if (!hasAnyMeshes && g_fallbackMesh) {
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

    // Submit screen overlay (game UI/HUD captured from DX11 backbuffer)
    if (overlay.valid && !overlay.pixels.empty() && api->DrawScreenOverlay) {
        remixapi_Format fmt = DxgiToRemixFormat(static_cast<DXGI_FORMAT>(overlay.dxgiFormat));
        if (fmt != static_cast<remixapi_Format>(0)) {
            api->DrawScreenOverlay(overlay.pixels.data(), overlay.width, overlay.height, fmt, 1.0f);
        }
    }

    // Present
    remixapi_PresentInfo presentInfo = {};
    presentInfo.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
    presentInfo.hwndOverride = nullptr;
    api->Present(&presentInfo);
}

void RemixRenderer::Shutdown() {
    UnloadAllCells();

    remixapi_Interface* api = RemixAPI::GetInterface();
    if (api && g_fallbackMesh) {
        api->DestroyMesh(g_fallbackMesh);
        g_fallbackMesh = nullptr;
    }
}
