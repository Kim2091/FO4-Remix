#include "scene_extractor.h"

#include "f4se_common/f4se_version.h"
#include "f4se_common/Relocation.h"
#include "f4se/PluginAPI.h"
#include "f4se/NiNodes.h"
#include "f4se/NiObjects.h"
#include "f4se/NiTypes.h"
#include "f4se/BSGeometry.h"
#include "f4se/GameTypes.h"
#include "f4se/NiProperties.h"
#include "f4se/NiMaterials.h"
#include "f4se/NiTextures.h"

#include <d3d11.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <wrl/client.h>

// ---------------------------------------------------------------------------
// g_player RelocPtr — same address as GameReferences.cpp but avoids pulling
// in the full GameReferences / GameForms dependency chain.
// ---------------------------------------------------------------------------
static RelocPtr<uintptr_t> s_g_player(0x032D2260);

// Known offsets (verified by STATIC_ASSERTs in F4SE SDK headers):
//   TESObjectREFR::parentCell  = 0xB8  (GameReferences.h)
//   TESObjectREFR::unkF0       = 0xF0  (LoadedData*)
//   LoadedData::rootNode       = 0x08  (NiNode*)
//   TESObjectCELL::objectList  = 0x70  (tArray<TESObjectREFR*>)

static constexpr uintptr_t OFF_REFR_PARENT_CELL = 0xB8;
static constexpr uintptr_t OFF_REFR_LOADED_DATA = 0xF0;
static constexpr uintptr_t OFF_LOADED_ROOT_NODE = 0x08;
static constexpr uintptr_t OFF_CELL_OBJECT_LIST = 0x70;

// ---------------------------------------------------------------------------
// Half-float → float conversion
// ---------------------------------------------------------------------------
static float HalfToFloat(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;

    uint32_t result;
    if (exp == 0) {
        if (mant == 0) {
            result = sign;
        } else {
            // Denormalized → renormalize
            exp = 1;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            mant &= 0x3FF;
            result = sign | ((exp + 112) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        result = sign | 0x7F800000 | (mant << 13); // Inf / NaN
    } else {
        result = sign | ((exp + 112) << 23) | (mant << 13);
    }

    float f;
    memcpy(&f, &result, 4);
    return f;
}

// Packed unsigned byte → [-1, 1]
static float UnpackByte(uint8_t b) {
    return (b / 255.0f) * 2.0f - 1.0f;
}

// ---------------------------------------------------------------------------
// Texture cache — keyed by hash derived from ID3D11Resource pointer
// ---------------------------------------------------------------------------
static std::unordered_map<uint64_t, ExtractedTexture> g_textureCache;

// ---------------------------------------------------------------------------
// Compute mip-0 byte size for a given DXGI_FORMAT, width, height.
// Returns 0 for unsupported formats (caller should skip texture).
// ---------------------------------------------------------------------------
static uint32_t ComputeMip0Size(uint32_t width, uint32_t height, DXGI_FORMAT fmt)
{
    uint32_t bw, bh; // block dimensions (in blocks for BC, in pixels for uncompressed)
    switch (fmt) {
        // BC1 / DXT1
        case DXGI_FORMAT_BC1_TYPELESS:      // 70 — not in task list but close
        case DXGI_FORMAT_BC1_UNORM:         // 71
        case DXGI_FORMAT_BC1_UNORM_SRGB:    // 72
            bw = (width + 3) / 4;  if (bw < 1) bw = 1;
            bh = (height + 3) / 4; if (bh < 1) bh = 1;
            return bw * bh * 8;

        // BC3 / DXT5
        case DXGI_FORMAT_BC3_TYPELESS:      // 76
        case DXGI_FORMAT_BC3_UNORM:         // 77
        case DXGI_FORMAT_BC3_UNORM_SRGB:    // 78
            bw = (width + 3) / 4;  if (bw < 1) bw = 1;
            bh = (height + 3) / 4; if (bh < 1) bh = 1;
            return bw * bh * 16;

        // BC5
        case DXGI_FORMAT_BC5_TYPELESS:      // 82
        case DXGI_FORMAT_BC5_UNORM:         // 83
        case DXGI_FORMAT_BC5_SNORM:         // 84
            bw = (width + 3) / 4;  if (bw < 1) bw = 1;
            bh = (height + 3) / 4; if (bh < 1) bh = 1;
            return bw * bh * 16;

        // BC7
        case DXGI_FORMAT_BC7_TYPELESS:      // 97
        case DXGI_FORMAT_BC7_UNORM:         // 98
        case DXGI_FORMAT_BC7_UNORM_SRGB:    // 99
            bw = (width + 3) / 4;  if (bw < 1) bw = 1;
            bh = (height + 3) / 4; if (bh < 1) bh = 1;
            return bw * bh * 16;

        // R8G8B8A8
        case DXGI_FORMAT_R8G8B8A8_UNORM:        // 28
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   // 29
            return width * height * 4;

        // B8G8R8A8
        case DXGI_FORMAT_B8G8R8A8_UNORM:         // 87
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:    // 91
            return width * height * 4;

        default:
            return 0; // unsupported
    }
}

// ---------------------------------------------------------------------------
// Determine whether a DXGI_FORMAT is block-compressed and its block byte size.
// Returns false for uncompressed formats.
// ---------------------------------------------------------------------------
static bool IsBlockCompressed(DXGI_FORMAT fmt, uint32_t& blockSize)
{
    switch (fmt) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            blockSize = 8;
            return true;

        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            blockSize = 16;
            return true;

        default:
            blockSize = 0;
            return false;
    }
}

// ---------------------------------------------------------------------------
// GPU readback: copy mip 0 of a texture into CPU memory
// ---------------------------------------------------------------------------
static bool ReadbackTexture(ID3D11Device* device, ID3D11Texture2D* tex2D,
                            uint64_t hash, ExtractedTexture& out)
{
    using Microsoft::WRL::ComPtr;

    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    if (!ctx) return false;

    D3D11_TEXTURE2D_DESC desc;
    tex2D->GetDesc(&desc);

    uint32_t dataSize = ComputeMip0Size(desc.Width, desc.Height, desc.Format);
    if (dataSize == 0) {
        _MESSAGE("FO4RemixPlugin: ReadbackTexture - unsupported DXGI format %u, skipping", (unsigned)desc.Format);
        return false;
    }

    // Create staging texture matching mip 0
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width              = desc.Width;
    stagingDesc.Height             = desc.Height;
    stagingDesc.MipLevels          = 1;
    stagingDesc.ArraySize          = 1;
    stagingDesc.Format             = desc.Format;
    stagingDesc.SampleDesc.Count   = 1;
    stagingDesc.SampleDesc.Quality = 0;
    stagingDesc.Usage              = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags          = 0;
    stagingDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr)) {
        _MESSAGE("FO4RemixPlugin: ReadbackTexture - CreateTexture2D staging failed hr=0x%08X", (unsigned)hr);
        return false;
    }

    // Copy mip 0 from the source texture
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex2D, 0, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        _MESSAGE("FO4RemixPlugin: ReadbackTexture - Map failed hr=0x%08X", (unsigned)hr);
        return false;
    }

    // Determine row layout
    uint32_t blockSize = 0;
    bool bc = IsBlockCompressed(desc.Format, blockSize);

    uint32_t numRows;       // number of scanline-rows (or block-rows for BC)
    uint32_t expectedPitch; // tight row pitch

    if (bc) {
        uint32_t bw = (desc.Width  + 3) / 4; if (bw < 1) bw = 1;
        uint32_t bh = (desc.Height + 3) / 4; if (bh < 1) bh = 1;
        numRows       = bh;
        expectedPitch = bw * blockSize;
    } else {
        numRows       = desc.Height;
        expectedPitch = desc.Width * 4; // all uncompressed formats we support are 4 bpp
    }

    out.pixels.resize(dataSize);
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst = out.pixels.data();

    for (uint32_t row = 0; row < numRows; row++) {
        memcpy(dst, src, expectedPitch);
        src += mapped.RowPitch;
        dst += expectedPitch;
    }

    ctx->Unmap(staging.Get(), 0);

    // Fill metadata
    out.hash       = hash;
    out.width      = desc.Width;
    out.height     = desc.Height;
    out.dxgiFormat = desc.Format;

    return true;
}

// ---------------------------------------------------------------------------
// Walk the shader property chain on a BSTriShape to extract the diffuse texture
// ---------------------------------------------------------------------------
static uint64_t ExtractDiffuseTexture(BSTriShape* shape, ID3D11Device* device,
                                      std::vector<ExtractedTexture>& newTextures)
{
    if (!shape) return 0;

    // BSGeometry::shaderProperty is NiPointer<NiProperty>
    NiProperty* prop = shape->shaderProperty;
    if (!prop) return 0;

    BSShaderProperty* shaderProp = static_cast<BSShaderProperty*>(prop);
    BSShaderMaterial* material = shaderProp->shaderMaterial;
    if (!material) return 0;

    // GetFeature() == 2 means lighting shader material
    if (material->GetFeature() != 2) return 0;

    BSLightingShaderMaterialBase* lightingMat =
        static_cast<BSLightingShaderMaterialBase*>(material);

    NiTexture* diffuseTex = lightingMat->spDiffuseTexture;
    if (!diffuseTex) return 0;

    BSRenderData* renderData = diffuseTex->rendererData;
    if (!renderData) return 0;

    ID3D11Resource* resource = renderData->resource;
    if (!resource) return 0;

    // Compute hash from resource pointer
    uint64_t hash = (uintptr_t)resource * 0x9E3779B97F4A7C15ULL;

    // Check cache first
    auto it = g_textureCache.find(hash);
    if (it != g_textureCache.end()) {
        return hash;
    }

    // QueryInterface to ID3D11Texture2D
    ID3D11Texture2D* tex2D = nullptr;
    HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                          reinterpret_cast<void**>(&tex2D));
    if (FAILED(hr) || !tex2D) return 0;

    ExtractedTexture extracted;
    bool ok = ReadbackTexture(device, tex2D, hash, extracted);
    tex2D->Release();

    if (!ok) return 0;

    // Log the texture name for debugging
    const char* texName = diffuseTex->name.c_str();
    _MESSAGE("FO4RemixPlugin: Extracted diffuse texture \"%s\" %ux%u fmt=%u hash=0x%016llX",
             texName ? texName : "<null>",
             extracted.width, extracted.height,
             (unsigned)extracted.dxgiFormat, hash);

    // Store in cache and output list
    g_textureCache[hash] = extracted; // copy into cache
    newTextures.push_back(std::move(extracted));

    return hash;
}

// ---------------------------------------------------------------------------
// Extract vertex/index data from a single BSTriShape
// ---------------------------------------------------------------------------
static bool ExtractTriShape(BSTriShape* shape, uint64_t baseHash,
                            std::vector<ExtractedMesh>& out,
                            ID3D11Device* device,
                            std::vector<ExtractedTexture>& newTextures)
{
    if (!shape || shape->numVertices == 0 || shape->numTriangles == 0)
        return false;

    // Renderer data → vertex/index buffers
    auto* gfxData = static_cast<BSGraphics::TriShape*>(shape->pRendererData);
    if (!gfxData || !gfxData->pVB || !gfxData->pIB)
        return false;

    uint8_t* vbData = static_cast<uint8_t*>(gfxData->pVB->pData);
    uint8_t* ibData = static_cast<uint8_t*>(gfxData->pIB->pData);
    if (!vbData || !ibData)
        return false;

    uint64_t desc = shape->vertexDesc;
    uint16_t vertexSize = shape->GetVertexSize();
    if (vertexSize == 0) return false;

    bool hasUVs        = (desc & BSGeometry::kFlag_UVs) != 0;
    bool hasNormals    = (desc & BSGeometry::kFlag_Normals) != 0;
    bool hasColors     = (desc & BSGeometry::kFlag_VertexColors) != 0;

    // Offsets are stored as nibbles, each representing offset/4
    uint32_t oUV     = ((desc >>  8) & 0xF) * 4;
    uint32_t oNormal = ((desc >> 16) & 0xF) * 4;
    uint32_t oColor  = ((desc >> 24) & 0xF) * 4;

    // Determine position format from UV offset:
    //   oUV == 8  → positions are 4 half-floats (8 bytes)
    //   oUV >= 12 → positions are 3 full floats (12 bytes)
    bool posHalfFloat = (oUV > 0 && oUV <= 8);

    // Log first shape's vertex format for debugging
    static int s_loggedShapes = 0;
    if (s_loggedShapes < 3) {
        _MESSAGE("FO4RemixPlugin: Shape vertexSize=%u desc=0x%016llX oUV=%u oNormal=%u oColor=%u posHalf=%d "
                 "flags: UV=%d Norm=%d Color=%d FullPrec=%d verts=%u tris=%u",
                 vertexSize, desc, oUV, oNormal, oColor, posHalfFloat,
                 hasUVs, hasNormals, hasColors,
                 (desc & BSGeometry::kFlag_FullPrecision) ? 1 : 0,
                 shape->numVertices, shape->numTriangles);
    }

    ExtractedMesh mesh;
    mesh.hash = baseHash ^ (uintptr_t)shape;
    mesh.vertices.resize(shape->numVertices);

    for (uint16_t i = 0; i < shape->numVertices; i++) {
        uint8_t* v = vbData + (uint32_t)i * vertexSize;
        remixapi_HardcodedVertex& out_v = mesh.vertices[i];
        memset(&out_v, 0, sizeof(out_v));

        // Position (always at offset 0)
        if (posHalfFloat) {
            uint16_t* pos = reinterpret_cast<uint16_t*>(v);
            out_v.position[0] = HalfToFloat(pos[0]);
            out_v.position[1] = HalfToFloat(pos[1]);
            out_v.position[2] = HalfToFloat(pos[2]);
        } else {
            float* pos = reinterpret_cast<float*>(v);
            out_v.position[0] = pos[0];
            out_v.position[1] = pos[1];
            out_v.position[2] = pos[2];
        }

        // UVs
        if (hasUVs && oUV + 4 <= vertexSize) {
            uint16_t* uv = reinterpret_cast<uint16_t*>(v + oUV);
            out_v.texcoord[0] = HalfToFloat(uv[0]);
            out_v.texcoord[1] = HalfToFloat(uv[1]);
        }

        // Normals (packed as 3 unsigned bytes + 1 byte bitangent sign)
        if (hasNormals && oNormal + 4 <= vertexSize) {
            uint8_t* n = v + oNormal;
            out_v.normal[0] = UnpackByte(n[0]);
            out_v.normal[1] = UnpackByte(n[1]);
            out_v.normal[2] = UnpackByte(n[2]);
        } else {
            out_v.normal[0] = 0.0f;
            out_v.normal[1] = 0.0f;
            out_v.normal[2] = 1.0f;
        }

        // Color (BGRA byte order → pack as B8G8R8A8)
        if (hasColors && oColor + 4 <= vertexSize) {
            memcpy(&out_v.color, v + oColor, 4);
        } else {
            out_v.color = 0xFFFFFFFF; // white
        }
    }

    // Validate vertex positions — reject mesh if any are NaN/Inf
    for (uint16_t i = 0; i < shape->numVertices; i++) {
        const auto& pos = mesh.vertices[i].position;
        for (int j = 0; j < 3; j++) {
            if (std::isnan(pos[j]) || std::isinf(pos[j])) {
                _MESSAGE("FO4RemixPlugin: Rejecting mesh - vertex %u has NaN/Inf position", i);
                return false;
            }
        }
    }

    // Indices (uint16 → uint32)
    uint32_t indexCount = shape->numTriangles * 3;
    uint16_t* indices16 = reinterpret_cast<uint16_t*>(ibData);
    mesh.indices.resize(indexCount);
    for (uint32_t i = 0; i < indexCount; i++) {
        uint32_t idx = indices16[i];
        if (idx >= shape->numVertices) {
            _MESSAGE("FO4RemixPlugin: Rejecting mesh - index[%u]=%u >= numVertices=%u",
                     i, idx, shape->numVertices);
            return false;
        }
        mesh.indices[i] = idx;
    }

    // World transform → row-major 3x4
    // Negate X and Z axes to mirror the world into Remix's LH coordinate system
    const NiTransform& xf = shape->m_worldTransform;
    float scale = xf.scale;
    // Swap X and Y columns in rotation to match camera coordinate swap
    for (int r = 0; r < 3; r++) {
        mesh.worldTransform[r][0] = xf.rot.data[r][1] * scale;
        mesh.worldTransform[r][1] = xf.rot.data[r][0] * scale;
        mesh.worldTransform[r][2] = xf.rot.data[r][2] * scale;
    }
    // Swap X and Y in translation too
    mesh.worldTransform[0][3] = xf.pos.y;
    mesh.worldTransform[1][3] = xf.pos.x;
    mesh.worldTransform[2][3] = xf.pos.z;

    // Compute local-space bounding extent for diagnostics
    float minPos[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float maxPos[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (uint16_t i = 0; i < shape->numVertices; i++) {
        for (int j = 0; j < 3; j++) {
            if (mesh.vertices[i].position[j] < minPos[j]) minPos[j] = mesh.vertices[i].position[j];
            if (mesh.vertices[i].position[j] > maxPos[j]) maxPos[j] = mesh.vertices[i].position[j];
        }
    }
    float extentX = (maxPos[0] - minPos[0]) * scale;
    float extentY = (maxPos[1] - minPos[1]) * scale;
    float extentZ = (maxPos[2] - minPos[2]) * scale;
    float maxExtent = extentX;
    if (extentY > maxExtent) maxExtent = extentY;
    if (extentZ > maxExtent) maxExtent = extentZ;

    const char* shapeName = shape->m_name.c_str();

    // Log shapes with large world extent to help identify unwanted geometry
    if (maxExtent > 500.0f) {
        _MESSAGE("FO4RemixPlugin: LARGE shape \"%s\" extent=(%.0f, %.0f, %.0f) maxExt=%.0f "
                 "worldPos=(%.1f, %.1f, %.1f) verts=%u tris=%u",
                 shapeName ? shapeName : "<null>",
                 extentX, extentY, extentZ, maxExtent,
                 xf.pos.x, xf.pos.y, xf.pos.z,
                 shape->numVertices, shape->numTriangles);
    }

    // Extract diffuse texture
    mesh.diffuseTextureHash = ExtractDiffuseTexture(shape, device, newTextures);
    mesh.normalTextureHash  = 0; // future

    out.push_back(std::move(mesh));
    return true;
}

// ---------------------------------------------------------------------------
// Recursively walk an NiNode tree and extract all BSTriShape children
// ---------------------------------------------------------------------------
static void WalkNode(NiAVObject* obj, uint64_t baseHash,
                     std::vector<ExtractedMesh>& out,
                     ID3D11Device* device,
                     std::vector<ExtractedTexture>& newTextures,
                     int depth = 0)
{
    if (!obj || depth > 32) return;

    // Skip invisible nodes
    if (obj->flags & NiAVObject::kFlagNotVisible)
        return;

    // Skip non-renderable geometry by node name
    const char* nodeName = obj->m_name.c_str();
    if (nodeName && nodeName[0]) {
        if (strstr(nodeName, "RoomMarker") ||
            strstr(nodeName, "Portal") ||
            strstr(nodeName, "EditorMarker") ||
            strstr(nodeName, "Trigger") ||
            strstr(nodeName, "MultiBound") ||
            strstr(nodeName, "Collision") ||
            strstr(nodeName, "bhk")) {
            return;
        }
    }

    // Check if this is a BSTriShape
    BSTriShape* tri = obj->GetAsBSTriShape();
    if (tri) {
        ExtractTriShape(tri, baseHash, out, device, newTextures);
        return; // BSTriShape is a leaf — no children
    }

    // If it's an NiNode, recurse into children
    NiNode* node = obj->GetAsNiNode();
    if (node) {
        for (uint16_t i = 0; i < node->m_children.m_emptyRunStart; i++) {
            NiAVObject* child = node->m_children.m_data[i];
            if (child) {
                WalkNode(child, baseHash, out, device, newTextures, depth + 1);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Lightweight readiness check — is the player in a cell with loaded 3D?
// ---------------------------------------------------------------------------
bool SceneExtractor::IsPlayerCellReady()
{
    uintptr_t* ppPlayer = reinterpret_cast<uintptr_t*>(s_g_player.GetPtr());
    if (!ppPlayer || !*ppPlayer)
        return false;
    uintptr_t player = *ppPlayer;

    // parentCell must exist
    uintptr_t cellPtr = *reinterpret_cast<uintptr_t*>(player + OFF_REFR_PARENT_CELL);
    if (!cellPtr)
        return false;

    // Cell must have objects
    struct SimpleArray {
        uintptr_t* entries;
        uint32_t capacity;
        uint32_t pad0C;
        uint32_t count;
    };
    auto& objectList = *reinterpret_cast<SimpleArray*>(cellPtr + OFF_CELL_OBJECT_LIST);
    if (!objectList.entries || objectList.count == 0)
        return false;

    // Player's own 3D must be loaded (strong signal that cell 3D is populated)
    uintptr_t loadedData = *reinterpret_cast<uintptr_t*>(player + OFF_REFR_LOADED_DATA);
    if (!loadedData)
        return false;
    NiNode* rootNode = *reinterpret_cast<NiNode**>(loadedData + OFF_LOADED_ROOT_NODE);
    if (!rootNode)
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// Extract all geometry from loaded references in the player's current cell
// ---------------------------------------------------------------------------
ExtractionResult SceneExtractor::ExtractPlayerCell(ID3D11Device* device)
{
    std::vector<ExtractedMesh> result;
    std::vector<ExtractedTexture> newTextures;

    // Get player pointer
    uintptr_t* ppPlayer = reinterpret_cast<uintptr_t*>(s_g_player.GetPtr());
    if (!ppPlayer || !*ppPlayer) {
        _MESSAGE("FO4RemixPlugin: ExtractPlayerCell - no player");
        return { std::move(result), std::move(newTextures) };
    }
    uintptr_t player = *ppPlayer;

    // Get parentCell
    uintptr_t cellPtr = *reinterpret_cast<uintptr_t*>(player + OFF_REFR_PARENT_CELL);
    if (!cellPtr) {
        _MESSAGE("FO4RemixPlugin: ExtractPlayerCell - no parentCell");
        return { std::move(result), std::move(newTextures) };
    }

    // Access cell objectList (tArray<TESObjectREFR*> at offset 0x70)
    struct SimpleArray {
        uintptr_t* entries;
        uint32_t capacity;
        uint32_t pad0C;
        uint32_t count;
    };
    auto& objectList = *reinterpret_cast<SimpleArray*>(cellPtr + OFF_CELL_OBJECT_LIST);

    _MESSAGE("FO4RemixPlugin: ExtractPlayerCell - cell has %u objects", objectList.count);

    uint32_t meshCount = 0;

    for (uint32_t i = 0; i < objectList.count; i++) {
        uintptr_t refrPtr = objectList.entries[i];
        if (!refrPtr) continue;

        // Get LoadedData
        uintptr_t loadedData = *reinterpret_cast<uintptr_t*>(refrPtr + OFF_REFR_LOADED_DATA);
        if (!loadedData) continue;

        // Get root NiNode
        NiNode* rootNode = *reinterpret_cast<NiNode**>(loadedData + OFF_LOADED_ROOT_NODE);
        if (!rootNode) continue;

        // Use object pointer as base hash for uniqueness
        uint64_t baseHash = refrPtr * 0x9E3779B97F4A7C15ULL; // hash mix

        size_t before = result.size();
        WalkNode(rootNode, baseHash, result, device, newTextures);
        meshCount += (uint32_t)(result.size() - before);
    }

    _MESSAGE("FO4RemixPlugin: ExtractPlayerCell - extracted %u meshes, %zu new textures (%zu cached) from %u objects",
             meshCount, newTextures.size(), g_textureCache.size(), objectList.count);

    return { std::move(result), std::move(newTextures) };
}

// ---------------------------------------------------------------------------
// Clear the texture readback cache
// ---------------------------------------------------------------------------
void SceneExtractor::ClearTextureCache()
{
    _MESSAGE("FO4RemixPlugin: ClearTextureCache - clearing %zu entries", g_textureCache.size());
    g_textureCache.clear();
}
