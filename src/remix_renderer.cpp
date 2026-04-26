#include "remix_renderer.h"
#include "config.h"
#include "remix_api.h"
#include "fo4_diagnostics.h"

#include "remix/remix_c.h"

#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"

#include <algorithm>
#include <cstring>
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
    uint64_t            meshHash = 0;       // Index into g_geometryAlphaState
    uint64_t            materialHash = 0;   // Index into g_materialCache (LRU)
    std::unordered_set<uint64_t> textureHashes;  // Textures used by this mesh's material
};

struct CellSceneData {
    std::vector<SceneMeshInstance> meshes;
    std::vector<SkinnedMeshInstance> skinnedMeshInstances;  // Skinned meshes
    std::unordered_set<uint64_t> materialHashes;   // Hashes into g_materialCache owned by this cell
    std::vector<remixapi_LightHandle> lights;
    std::unordered_set<uint64_t> textureHashes;
};

static std::unordered_map<uint32_t, CellSceneData> g_cellScenes;

// Textures are shared across cells (same texture may appear in multiple cells)
// so keep them global with a reference count
struct TextureRef {
    remixapi_TextureHandle handle;
    uint32_t refCount;
    uint64_t lastDrawnFrame = 0;  // Stamped in OnFrame DrawInstance loop;
                                  // read by SweepStaleTextures.
};
static std::unordered_map<uint64_t, TextureRef> g_textureHandles;

// Materials are shared across cells (same texture combo may appear in
// multiple cells) so keep them global with a reference count, similar to
// textures. lastDrawnFrame is stamped in OnFrame when any owner mesh's
// DrawInstance fires; it drives SweepStaleMaterials -- the LEVER that
// actually frees VRAM for shared texture sets, because materials hold
// Rc<DxvkImageView> refs and only their destruction drops those refs.
struct MaterialRef {
    remixapi_MaterialHandle handle;
    uint32_t refCount;
    uint64_t lastDrawnFrame = 0;
};
static std::unordered_map<uint64_t, MaterialRef> g_materialCache;

// Per-hash Remix InstanceInfoBlendEXT, populated at LoadCellScene time for
// meshes that have any alpha state (test or blend). OnFrame's DrawInstance
// loop chains the stored struct onto instance.pNext so Remix honors per-
// instance alpha state. Keyed by the mesh's `hash` field (NOT MaterialKey
// hash) -- per-instance, not per-material.
static std::unordered_map<uint64_t, remixapi_InstanceInfoBlendEXT> g_geometryAlphaState;

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

    for (auto& inst : cell.skinnedMeshInstances) {
        if (inst.meshHandle) api->DestroyMesh(inst.meshHandle);
    }

    // Decrement refcounts in the global material cache. Destroy the
    // material handle when the refcount hits zero; otherwise leave it
    // for other cells that still reference it.
    for (uint64_t matHash : cell.materialHashes) {
        auto matIt = g_materialCache.find(matHash);
        if (matIt != g_materialCache.end()) {
            if (matIt->second.refCount > 0) matIt->second.refCount--;
            if (matIt->second.refCount == 0) {
                if (matIt->second.handle) api->DestroyMaterial(matIt->second.handle);
                g_materialCache.erase(matIt);
            }
        }
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
        for (auto& inst : cell.skinnedMeshInstances) {
            if (inst.meshHandle) api->DestroyMesh(inst.meshHandle);
        }
        for (auto& handle : cell.lights) {
            if (handle) api->DestroyLight(handle);
        }
    }
    g_cellScenes.clear();

    // Destroy every material in the global cache. Refcounts are irrelevant
    // since every cell is being unloaded; nothing can still own these.
    for (auto& [hash, matRef] : g_materialCache) {
        if (matRef.handle) api->DestroyMaterial(matRef.handle);
    }
    g_materialCache.clear();

    for (auto& [hash, texRef] : g_textureHandles) {
        if (texRef.handle) api->DestroyTexture(texRef.handle);
    }
    g_textureHandles.clear();
}

// ---------------------------------------------------------------------------
// Material key: identifies a unique material by its texture combo + emissive params
// Defined at file scope so both CreateCellMaterials and LoadCellScene can use it.
// ---------------------------------------------------------------------------
struct MaterialKey {
    uint64_t diffuse;
    uint64_t normal;
    uint64_t roughness;
    uint64_t emissive;
    float emissiveColorR, emissiveColorG, emissiveColorB;
    float emissiveIntensity;
    bool alphaTestEnabled;
    int alphaTestType;       // Remix/VkCompareOp
    uint8_t alphaTestRef;
    bool useDrawCallAlphaState = false;  // true -> opaqueExt.useDrawCallAlphaState=1
    uint64_t combined() const {
        uint64_t h = diffuse;
        h ^= normal    * 0x517CC1B727220A95ULL;
        h ^= roughness * 0x6C62272E07BB0142ULL;
        h ^= emissive  * 0x9E3779B97F4A7C15ULL;
        // Include emissive color/intensity so different tints get separate materials
        uint32_t ri, gi, bi, ii;
        memcpy(&ri, &emissiveColorR, 4);
        memcpy(&gi, &emissiveColorG, 4);
        memcpy(&bi, &emissiveColorB, 4);
        memcpy(&ii, &emissiveIntensity, 4);
        h ^= (uint64_t)ri * 0x85EBCA6BC2B2AE35ULL;
        h ^= (uint64_t)gi * 0xC2B2AE3D27D4EB4FULL;
        h ^= (uint64_t)bi * 0x165667B19E3779F9ULL;
        h ^= (uint64_t)ii * 0x27D4EB2F165667C5ULL;
        // Distinguish materials that defer alpha to per-instance state.
        // Different value here means a different material in cache, which
        // is what we want -- otherwise a per-instance alpha-blend mesh
        // would share its material with an opaque variant and the Remix
        // fork would honor only one of the two states.
        h ^= (uint64_t)(useDrawCallAlphaState ? 1 : 0) * 0x9FB21C651E98DF25ULL;
        return h;
    }
};

// ---------------------------------------------------------------------------
// Create Remix materials from an extraction result.
// Collects unique MaterialKey entries from meshes and skinned meshes, then
// creates one Remix material per unique key.  Returns material handles keyed
// by the combined hash.
// ---------------------------------------------------------------------------
static std::unordered_map<uint64_t, remixapi_MaterialHandle> CreateCellMaterials(
    remixapi_Interface* api,
    uint32_t cellFormID,
    const ExtractionResult& result)
{
    std::unordered_map<uint64_t, remixapi_MaterialHandle> materials;

    std::unordered_map<uint64_t, MaterialKey> materialKeys;

    // Collect material keys from static meshes
    for (auto& mesh : result.meshes) {
        if (mesh.diffuseTextureHash == 0) continue;
        MaterialKey key { mesh.diffuseTextureHash, mesh.normalTextureHash, mesh.roughnessTextureHash,
                          mesh.emissiveTextureHash, mesh.emissiveColorR, mesh.emissiveColorG,
                          mesh.emissiveColorB, mesh.emissiveIntensity,
                          mesh.alphaTestEnabled, mesh.alphaTestType, mesh.alphaTestRef };
        key.useDrawCallAlphaState = mesh.alphaTestEnabled || mesh.alphaBlendEnabled;
        auto it = materialKeys.find(key.combined());
        if (it == materialKeys.end()) {
            materialKeys.emplace(key.combined(), key);
        } else if (mesh.alphaTestEnabled && !it->second.alphaTestEnabled) {
            it->second.alphaTestEnabled = true;
            it->second.alphaTestType = mesh.alphaTestType;
            it->second.alphaTestRef = mesh.alphaTestRef;
        }
    }

    // Collect material keys from skinned meshes
    for (auto& sm : result.skinnedMeshes) {
        if (sm.diffuseTextureHash == 0) continue;
        MaterialKey key { sm.diffuseTextureHash, sm.normalTextureHash, sm.roughnessTextureHash,
                          sm.emissiveTextureHash, sm.emissiveColorR, sm.emissiveColorG,
                          sm.emissiveColorB, sm.emissiveIntensity,
                          sm.alphaTestEnabled, sm.alphaTestType, sm.alphaTestRef };
        key.useDrawCallAlphaState = sm.alphaTestEnabled || sm.alphaBlendEnabled;
        auto it = materialKeys.find(key.combined());
        if (it == materialKeys.end()) {
            materialKeys.emplace(key.combined(), key);
        } else if (sm.alphaTestEnabled && !it->second.alphaTestEnabled) {
            it->second.alphaTestEnabled = true;
            it->second.alphaTestType = sm.alphaTestType;
            it->second.alphaTestRef = sm.alphaTestRef;
        }
    }

    uint32_t matCreated = 0, matReused = 0, matFailed = 0;

    for (auto& [combinedHash, key] : materialKeys) {
        // Check the global cache first; if a previous cell already created
        // this material, bump its refcount and skip CreateMaterial.
        auto cacheIt = g_materialCache.find(combinedHash);
        if (cacheIt != g_materialCache.end()) {
            cacheIt->second.refCount++;
            cacheIt->second.lastDrawnFrame = Diagnostics::CurrentFrameIndex();
            materials[combinedHash] = cacheIt->second.handle;
            matReused++;
            continue;
        }

        std::wstring diffPath = HashToPath(key.diffuse);
        std::wstring normalPath = key.normal ? HashToPath(key.normal) : L"";
        std::wstring roughPath = key.roughness ? HashToPath(key.roughness) : L"";
        std::wstring emissivePath = key.emissive ? HashToPath(key.emissive) : L"";

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
        // Tell Remix to honor per-instance alpha state via InstanceInfoBlendEXT
        // chained at DrawInstance time. Set when the mesh has either alpha-test
        // or alpha-blend enabled; otherwise the material's own alpha defaults
        // (above) win.
        opaqueExt.useDrawCallAlphaState = key.useDrawCallAlphaState ? 1 : 0;

        remixapi_MaterialInfo matInfo = {};
        matInfo.sType              = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
        matInfo.pNext              = &opaqueExt;
        matInfo.hash               = combinedHash;
        matInfo.albedoTexture      = diffPath.c_str();
        matInfo.normalTexture      = key.normal ? normalPath.c_str() : nullptr;
        matInfo.tangentTexture     = nullptr;
        matInfo.emissiveTexture       = key.emissive ? emissivePath.c_str() : nullptr;
        matInfo.emissiveIntensity     = key.emissiveIntensity * g_config.emissiveIntensity;
        matInfo.emissiveColorConstant = { key.emissiveColorR, key.emissiveColorG, key.emissiveColorB };
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

        g_materialCache[combinedHash] = { handle, 1, Diagnostics::CurrentFrameIndex() };
        materials[combinedHash] = handle;
        matCreated++;
    }

    _MESSAGE("FO4RemixPlugin: [Cell 0x%X] Materials - created %u, reused %u, failed %u",
             cellFormID, matCreated, matReused, matFailed);

    return materials;
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
    // The returned map is a local lookup of combinedHash -> handle for the
    // mesh-creation loops below; the actual handles are owned by g_materialCache
    // (refcounted, swept by SweepStaleMaterials).
    auto materialsLookup = CreateCellMaterials(api, cellFormID, result);
    for (auto& [combinedHash, _] : materialsLookup) {
        cellData.materialHashes.insert(combinedHash);
    }

    // ----- Create meshes -----
    uint32_t meshCreated = 0, meshFailed = 0;

    for (auto& mesh : result.meshes) {
        if (mesh.vertices.empty()) continue;

        // Look up material handle by combined texture hash
        remixapi_MaterialHandle matHandle = nullptr;
        uint64_t combinedHash = 0;
        if (mesh.diffuseTextureHash != 0) {
            MaterialKey lookupKey { mesh.diffuseTextureHash, mesh.normalTextureHash, mesh.roughnessTextureHash,
                                    mesh.emissiveTextureHash, mesh.emissiveColorR, mesh.emissiveColorG,
                                    mesh.emissiveColorB, mesh.emissiveIntensity,
                                    mesh.alphaTestEnabled, mesh.alphaTestType, mesh.alphaTestRef };
            lookupKey.useDrawCallAlphaState = mesh.alphaTestEnabled || mesh.alphaBlendEnabled;
            combinedHash = lookupKey.combined();
            auto it = materialsLookup.find(combinedHash);
            if (it != materialsLookup.end())
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

        // Per-instance alpha state for this mesh. If the mesh has neither
        // test nor blend, erase any prior entry (could happen if a mesh
        // re-uploaded with different state). Otherwise write the EXT.
        if (mesh.alphaTestEnabled || mesh.alphaBlendEnabled) {
            remixapi_InstanceInfoBlendEXT blend = {};
            blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
            blend.pNext = nullptr;
            blend.alphaTestEnabled         = mesh.alphaTestEnabled ? 1 : 0;
            blend.alphaTestReferenceValue  = mesh.alphaTestRef;
            blend.alphaTestCompareOp       = (uint32_t)mesh.alphaTestType;
            blend.alphaBlendEnabled        = mesh.alphaBlendEnabled ? 1 : 0;
            blend.srcColorBlendFactor      = mesh.srcColorBlendFactor;
            blend.dstColorBlendFactor      = mesh.dstColorBlendFactor;
            blend.colorBlendOp             = 0;  // VK_BLEND_OP_ADD
            blend.srcAlphaBlendFactor      = mesh.srcColorBlendFactor;
            blend.dstAlphaBlendFactor      = mesh.dstColorBlendFactor;
            blend.alphaBlendOp             = 0;  // VK_BLEND_OP_ADD
            blend.writeMask                = 0xF;  // RGBA
            blend.isVertexColorBakedLighting = 0;
            // Remaining textureColorArg* and tFactor* fields stay zero --
            // those are fixed-function emulation paths the Remix fork
            // ignores when alphaBlend/alphaTest are the active inputs.
            g_geometryAlphaState[mesh.hash] = blend;
        } else {
            g_geometryAlphaState.erase(mesh.hash);
        }

        SceneMeshInstance inst;
        inst.handle = handle;
        memcpy(&inst.transform.matrix, mesh.worldTransform, sizeof(mesh.worldTransform));
        inst.meshHash = mesh.hash;
        inst.materialHash = combinedHash;
        if (mesh.diffuseTextureHash)   inst.textureHashes.insert(mesh.diffuseTextureHash);
        if (mesh.normalTextureHash)    inst.textureHashes.insert(mesh.normalTextureHash);
        if (mesh.roughnessTextureHash) inst.textureHashes.insert(mesh.roughnessTextureHash);
        if (mesh.emissiveTextureHash)  inst.textureHashes.insert(mesh.emissiveTextureHash);

        cellData.meshes.push_back(inst);
        meshCreated++;
    }

    _MESSAGE("FO4RemixPlugin: [Cell 0x%X] Meshes - created %u, failed %u",
             cellFormID, meshCreated, meshFailed);

    // ----- Create skinned meshes -----
    uint32_t skinnedCreated = 0, skinnedFailed = 0;

    for (auto& sm : result.skinnedMeshes) {
        if (sm.vertices.empty()) continue;

        // Look up material handle by combined texture hash
        remixapi_MaterialHandle matHandle = nullptr;
        uint64_t combinedHash = 0;
        if (sm.diffuseTextureHash != 0) {
            MaterialKey lookupKey { sm.diffuseTextureHash, sm.normalTextureHash, sm.roughnessTextureHash,
                                    sm.emissiveTextureHash, sm.emissiveColorR, sm.emissiveColorG,
                                    sm.emissiveColorB, sm.emissiveIntensity,
                                    sm.alphaTestEnabled, sm.alphaTestType, sm.alphaTestRef };
            lookupKey.useDrawCallAlphaState = sm.alphaTestEnabled || sm.alphaBlendEnabled;
            combinedHash = lookupKey.combined();
            auto it = materialsLookup.find(combinedHash);
            if (it != materialsLookup.end())
                matHandle = it->second;
        }

        remixapi_MeshInfoSurfaceTriangles surface = {};
        surface.vertices_values = sm.vertices.data();
        surface.vertices_count = (uint32_t)sm.vertices.size();
        surface.indices_values = sm.indices.empty() ? nullptr : sm.indices.data();
        surface.indices_count = (uint32_t)sm.indices.size();

        // Set skinning data
        surface.skinning_hasvalue = 1;
        surface.skinning_value.bonesPerVertex = kBonesPerVertex;
        surface.skinning_value.blendWeights_values = sm.blendWeights.data();
        surface.skinning_value.blendWeights_count  = static_cast<uint32_t>(sm.blendWeights.size());
        surface.skinning_value.blendIndices_values = sm.blendIndices.data();
        surface.skinning_value.blendIndices_count  = static_cast<uint32_t>(sm.blendIndices.size());

        surface.material = matHandle;

        remixapi_MeshInfo meshInfo = {};
        meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
        meshInfo.hash = sm.hash;
        meshInfo.surfaces_values = &surface;
        meshInfo.surfaces_count = 1;

        remixapi_MeshHandle handle = nullptr;
        remixapi_ErrorCode status = api->CreateMesh(&meshInfo, &handle);
        if (status != REMIXAPI_ERROR_CODE_SUCCESS || !handle) {
            _MESSAGE("FO4RemixPlugin: [Cell 0x%X] Failed to create skinned mesh 0x%llX (error %d)",
                     cellFormID, (unsigned long long)sm.hash, (int)status);
            skinnedFailed++;
            continue;
        }

        // Per-instance alpha state for this skinned mesh.
        if (sm.alphaTestEnabled || sm.alphaBlendEnabled) {
            remixapi_InstanceInfoBlendEXT blend = {};
            blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
            blend.pNext = nullptr;
            blend.alphaTestEnabled         = sm.alphaTestEnabled ? 1 : 0;
            blend.alphaTestReferenceValue  = sm.alphaTestRef;
            blend.alphaTestCompareOp       = (uint32_t)sm.alphaTestType;
            blend.alphaBlendEnabled        = sm.alphaBlendEnabled ? 1 : 0;
            blend.srcColorBlendFactor      = sm.srcColorBlendFactor;
            blend.dstColorBlendFactor      = sm.dstColorBlendFactor;
            blend.colorBlendOp             = 0;  // VK_BLEND_OP_ADD
            blend.srcAlphaBlendFactor      = sm.srcColorBlendFactor;
            blend.dstAlphaBlendFactor      = sm.dstColorBlendFactor;
            blend.alphaBlendOp             = 0;  // VK_BLEND_OP_ADD
            blend.writeMask                = 0xF;  // RGBA
            blend.isVertexColorBakedLighting = 0;
            g_geometryAlphaState[sm.hash] = blend;
        } else {
            g_geometryAlphaState.erase(sm.hash);
        }

        SkinnedMeshInstance inst;
        inst.meshHandle = handle;
        inst.materialHandle = matHandle;
        inst.meshHash = sm.hash;
        inst.boneCount = sm.boneCount;
        inst.ownerFormID = sm.ownerFormID;
        inst.isValid = true;
        inst.materialHash = combinedHash;
        if (sm.diffuseTextureHash)   inst.textureHashes.insert(sm.diffuseTextureHash);
        if (sm.normalTextureHash)    inst.textureHashes.insert(sm.normalTextureHash);
        if (sm.roughnessTextureHash) inst.textureHashes.insert(sm.roughnessTextureHash);
        if (sm.emissiveTextureHash)  inst.textureHashes.insert(sm.emissiveTextureHash);
        cellData.skinnedMeshInstances.push_back(inst);
        skinnedCreated++;
    }

    if (skinnedCreated > 0 || skinnedFailed > 0) {
        _MESSAGE("FO4RemixPlugin: [Cell 0x%X] Skinned meshes - created %u, failed %u",
                 cellFormID, skinnedCreated, skinnedFailed);
    }

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
void RemixRenderer::OnFrame(const CameraState& cam,
                            const std::vector<ExtractedSkinnedMesh>& skinnedMeshBoneData,
                            const OverlayData& overlay) {
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

    // Draw scene meshes from all loaded cells
    bool hasAnyMeshes = false;
    uint32_t totalLightsDrawn = 0;
    uint32_t totalLightsFailed = 0;
    for (auto& [cellID, cellData] : g_cellScenes) {
        for (auto& inst : cellData.meshes) {
            remixapi_InstanceInfo instance = {};
            instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
            instance.mesh = inst.handle;
            instance.transform = inst.transform;
            instance.doubleSided = 1;
            instance.categoryFlags = 0;

            // Chain per-instance alpha state if this mesh has any. Local
            // copy of the EXT so we can clear pNext for safety (the cached
            // entry's pNext is undefined) and so the address remains valid
            // through DrawInstance.
            remixapi_InstanceInfoBlendEXT blendLocal = {};
            auto alphaIt = g_geometryAlphaState.find(inst.meshHash);
            if (alphaIt != g_geometryAlphaState.end()) {
                blendLocal = alphaIt->second;
                blendLocal.pNext = nullptr;
                instance.pNext = &blendLocal;
            }

            api->DrawInstance(&instance);
            hasAnyMeshes = true;
        }

        // Draw skinned mesh instances with real per-frame bone transforms.
        // Bone transforms are computed by the game thread and passed in via skinnedMeshBoneData.
        for (auto& inst : cellData.skinnedMeshInstances) {
            if (!inst.isValid) continue;

            // Find the corresponding ExtractedSkinnedMesh with current bone transforms
            const ExtractedSkinnedMesh* matchedSM = nullptr;
            for (const auto& sm : skinnedMeshBoneData) {
                if (sm.hash == inst.meshHash) {
                    matchedSM = &sm;
                    break;
                }
            }

            // Build bone transforms array for Remix
            std::vector<remixapi_Transform> boneTransforms(inst.boneCount);

            bool usedRealBones = false;
            if (matchedSM && matchedSM->boneCount == inst.boneCount &&
                matchedSM->currentBoneTransforms.size() == inst.boneCount) {
                // Use real bone transforms from game thread
                static_assert(sizeof(remixapi_Transform) == 12 * sizeof(float),
                              "remixapi_Transform must be 48 bytes");
                for (uint32_t b = 0; b < inst.boneCount; b++) {
                    memcpy(&boneTransforms[b], matchedSM->currentBoneTransforms[b].data(),
                           sizeof(remixapi_Transform));
                }
                usedRealBones = true;
            } else {
                // Fallback: identity bone transforms (bind pose) if no bone data available
                for (uint32_t b = 0; b < inst.boneCount; b++) {
                    memset(&boneTransforms[b], 0, sizeof(remixapi_Transform));
                    boneTransforms[b].matrix[0][0] = 1.0f;
                    boneTransforms[b].matrix[1][1] = 1.0f;
                    boneTransforms[b].matrix[2][2] = 1.0f;
                }
            }

            // Enhanced diagnostics: log once per skinned mesh on first frame
            static std::unordered_set<uint64_t> s_loggedSkinnedHashes;
            if (s_loggedSkinnedHashes.find(inst.meshHash) == s_loggedSkinnedHashes.end()) {
                s_loggedSkinnedHashes.insert(inst.meshHash);
                const float* b0 = boneTransforms[0].matrix[0];
                float tx = boneTransforms[0].matrix[0][3];
                float ty = boneTransforms[0].matrix[1][3];
                float tz = boneTransforms[0].matrix[2][3];
                _MESSAGE("FO4RemixPlugin: [SKINNED DRAW] hash=0x%llX bones=%u %s bone0_trans=(%.1f,%.1f,%.1f) bone0_diag=(%.4f,%.4f,%.4f)",
                         inst.meshHash, inst.boneCount,
                         usedRealBones ? "REAL" : "FALLBACK_IDENTITY",
                         tx, ty, tz,
                         boneTransforms[0].matrix[0][0],
                         boneTransforms[0].matrix[1][1],
                         boneTransforms[0].matrix[2][2]);
                // Flag suspiciously large translations (>100k units = likely world-space issue)
                for (uint32_t b = 0; b < inst.boneCount; b++) {
                    float btx = boneTransforms[b].matrix[0][3];
                    float bty = boneTransforms[b].matrix[1][3];
                    float btz = boneTransforms[b].matrix[2][3];
                    if (fabsf(btx) > 100000.f || fabsf(bty) > 100000.f || fabsf(btz) > 100000.f) {
                        _MESSAGE("FO4RemixPlugin: [SKINNED DRAW] WARNING hash=0x%llX bone[%u] extreme translation=(%.1f,%.1f,%.1f)",
                                 inst.meshHash, b, btx, bty, btz);
                        break;
                    }
                    // Check for NaN/Inf in bone transforms
                    bool hasBad = false;
                    for (int rr = 0; rr < 3 && !hasBad; rr++)
                        for (int cc = 0; cc < 4 && !hasBad; cc++)
                            if (std::isnan(boneTransforms[b].matrix[rr][cc]) || std::isinf(boneTransforms[b].matrix[rr][cc]))
                                hasBad = true;
                    if (hasBad) {
                        _MESSAGE("FO4RemixPlugin: [SKINNED DRAW] WARNING hash=0x%llX bone[%u] has NaN/Inf transform!", inst.meshHash, b);
                        break;
                    }
                }
            }

            // Safety check: bone count within Remix limit
            uint32_t boneCountToSubmit = inst.boneCount;
            if (boneCountToSubmit > REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT) {
                boneCountToSubmit = REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT;
            }

            // Build bone transforms extension struct
            remixapi_InstanceInfoBoneTransformsEXT boneExt = {};
            boneExt.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
            boneExt.pNext = nullptr;
            boneExt.boneTransforms_values = boneTransforms.data();
            boneExt.boneTransforms_count  = boneCountToSubmit;

            // Identity instance transform (bones handle world positioning)
            remixapi_Transform identityXform = {};
            identityXform.matrix[0][0] = 1.0f;
            identityXform.matrix[1][1] = 1.0f;
            identityXform.matrix[2][2] = 1.0f;

            remixapi_InstanceInfo instance = {};
            instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
            instance.pNext = &boneExt;
            instance.mesh = inst.meshHandle;
            instance.transform = identityXform;
            instance.doubleSided = 1;
            instance.categoryFlags = 0;

            // Extend pNext chain with per-instance alpha state if any.
            remixapi_InstanceInfoBlendEXT skinnedBlendLocal = {};
            auto skinnedAlphaIt = g_geometryAlphaState.find(inst.meshHash);
            if (skinnedAlphaIt != g_geometryAlphaState.end()) {
                skinnedBlendLocal = skinnedAlphaIt->second;
                skinnedBlendLocal.pNext = nullptr;
                // Append after boneExt by setting its pNext.
                boneExt.pNext = &skinnedBlendLocal;
            }

            remixapi_ErrorCode err = api->DrawInstance(&instance);
            if (err == REMIXAPI_ERROR_CODE_SUCCESS) {
                hasAnyMeshes = true;
            }
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

    // Periodic status log (every ~5 seconds)
    static uint32_t s_frameCounter = 0;
    static uint32_t s_skinnedWithBones = 0;
    static uint32_t s_skinnedFallback = 0;
    static uint32_t s_skinnedDrawFailed = 0;
    s_frameCounter++;
    if (s_frameCounter % 300 == 0) {
        _MESSAGE("FO4RemixPlugin: OnFrame status - %zu cells, %u lights drawn, %u lights failed",
                 g_cellScenes.size(), totalLightsDrawn, totalLightsFailed);

        // Count skinned mesh stats for this snapshot
        uint32_t totalSkinned = 0;
        uint32_t withRealBones = 0;
        for (auto& [cid, cd] : g_cellScenes) {
            for (auto& inst : cd.skinnedMeshInstances) {
                if (!inst.isValid) continue;
                totalSkinned++;
                // Check if bone data was available
                for (const auto& sm : skinnedMeshBoneData) {
                    if (sm.hash == inst.meshHash && sm.boneCount == inst.boneCount) {
                        withRealBones++;
                        break;
                    }
                }
            }
        }
        if (totalSkinned > 0) {
            _MESSAGE("FO4RemixPlugin: [SKINNING] Draw status: %u skinned instances, %u with real bones, %u identity fallback, boneData entries=%zu",
                     totalSkinned, withRealBones, totalSkinned - withRealBones, skinnedMeshBoneData.size());
        }
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

    // Submit screen overlay (game UI/HUD captured from DX11 backbuffer).
    // Gated on g_config.hudOverlayEnabled (default false) because the in-source
    // dxvk-remix's dispatchScreenOverlay currently asserts inside dxvk_barrier
    // (dstLayout == VK_IMAGE_LAYOUT_UNDEFINED) the moment a HUD frame is staged.
    // Flip the [Overlay] HudOverlayEnabled INI key to true once the runtime
    // barrier path is fixed.
    if (g_config.hudOverlayEnabled
        && overlay.valid && !overlay.pixels.empty()
        && api->DrawScreenOverlay) {
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
