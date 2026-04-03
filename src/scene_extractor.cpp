#include "scene_extractor.h"

#include "f4se_common/f4se_version.h"
#include "f4se_common/Relocation.h"
#include "f4se/PluginAPI.h"
#include "f4se/NiNodes.h"
#include "f4se/NiObjects.h"
#include "f4se/NiTypes.h"
#include "f4se/BSGeometry.h"
#include "f4se/GameTypes.h"

#include <cstring>
#include <unordered_set>

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
// Extract vertex/index data from a single BSTriShape
// ---------------------------------------------------------------------------
static bool ExtractTriShape(BSTriShape* shape, uint64_t baseHash,
                            std::vector<ExtractedMesh>& out)
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

    // Indices (uint16 → uint32)
    uint32_t indexCount = shape->numTriangles * 3;
    uint16_t* indices16 = reinterpret_cast<uint16_t*>(ibData);
    mesh.indices.resize(indexCount);
    for (uint32_t i = 0; i < indexCount; i++) {
        mesh.indices[i] = indices16[i];
    }

    // World transform → row-major 3x4
    const NiTransform& xf = shape->m_worldTransform;
    float scale = xf.scale;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            mesh.worldTransform[r][c] = xf.rot.data[r][c] * scale;
        }
    }
    mesh.worldTransform[0][3] = xf.pos.x;
    mesh.worldTransform[1][3] = xf.pos.y;
    mesh.worldTransform[2][3] = xf.pos.z;

    // Log first few shapes' vertex data for debugging
    if (s_loggedShapes < 3 && !mesh.vertices.empty()) {
        auto& v0 = mesh.vertices[0];
        _MESSAGE("FO4RemixPlugin:   vert[0] pos=(%.2f, %.2f, %.2f) normal=(%.2f, %.2f, %.2f)",
                 v0.position[0], v0.position[1], v0.position[2],
                 v0.normal[0], v0.normal[1], v0.normal[2]);
        _MESSAGE("FO4RemixPlugin:   worldTransform pos=(%.1f, %.1f, %.1f) scale=%.2f",
                 xf.pos.x, xf.pos.y, xf.pos.z, xf.scale);
        s_loggedShapes++;
    }

    out.push_back(std::move(mesh));
    return true;
}

// ---------------------------------------------------------------------------
// Recursively walk an NiNode tree and extract all BSTriShape children
// ---------------------------------------------------------------------------
static void WalkNode(NiAVObject* obj, uint64_t baseHash,
                     std::vector<ExtractedMesh>& out, int depth = 0)
{
    if (!obj || depth > 32) return;

    // Skip invisible nodes
    if (obj->flags & NiAVObject::kFlagNotVisible)
        return;

    // Check if this is a BSTriShape
    BSTriShape* tri = obj->GetAsBSTriShape();
    if (tri) {
        ExtractTriShape(tri, baseHash, out);
        return; // BSTriShape is a leaf — no children
    }

    // If it's an NiNode, recurse into children
    NiNode* node = obj->GetAsNiNode();
    if (node) {
        for (uint16_t i = 0; i < node->m_children.m_emptyRunStart; i++) {
            NiAVObject* child = node->m_children.m_data[i];
            if (child) {
                WalkNode(child, baseHash, out, depth + 1);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Extract all geometry from loaded references in the player's current cell
// ---------------------------------------------------------------------------
std::vector<ExtractedMesh> SceneExtractor::ExtractPlayerCell()
{
    std::vector<ExtractedMesh> result;

    // Get player pointer
    uintptr_t* ppPlayer = reinterpret_cast<uintptr_t*>(s_g_player.GetPtr());
    if (!ppPlayer || !*ppPlayer) {
        _MESSAGE("FO4RemixPlugin: ExtractPlayerCell - no player");
        return result;
    }
    uintptr_t player = *ppPlayer;

    // Get parentCell
    uintptr_t cellPtr = *reinterpret_cast<uintptr_t*>(player + OFF_REFR_PARENT_CELL);
    if (!cellPtr) {
        _MESSAGE("FO4RemixPlugin: ExtractPlayerCell - no parentCell");
        return result;
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
        WalkNode(rootNode, baseHash, result);
        meshCount += (uint32_t)(result.size() - before);
    }

    _MESSAGE("FO4RemixPlugin: ExtractPlayerCell - extracted %u meshes from %u objects",
             meshCount, objectList.count);

    return result;
}
