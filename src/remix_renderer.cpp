#include "remix_renderer.h"
#include "config.h"
#include "remix_api.h"
#include "fo4_diagnostics.h"
#include "semantic_capture.h"
#include "resolvers/lighting_static.h"  // Trace::SetStep + Trace::Step constants

#include "remix/remix_c.h"

#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <d3d11.h>
#include <excpt.h>  // EXCEPTION_EXECUTE_HANDLER for SEH wrappers below

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
// Mesh-handle cache key + content hashing
//
// Two drawables can share a Remix mesh handle iff their geometry bytes AND
// their resolved material match. Material participates because dxvk-remix
// bakes the material reference into the mesh at CreateMesh time -- two
// drawables with same content but different materials need separate handles.
// ---------------------------------------------------------------------------
struct MeshCacheKey {
    uint64_t contentHash;
    uint64_t materialHash;
    bool operator==(const MeshCacheKey& o) const noexcept {
        return contentHash == o.contentHash && materialHash == o.materialHash;
    }
};
struct MeshCacheKeyHash {
    size_t operator()(const MeshCacheKey& k) const noexcept {
        return (size_t)(k.contentHash ^ (k.materialHash * 0x9E3779B97F4A7C15ULL));
    }
};

// FNV-1a over a byte range. Stable across drawables that share the same NIF,
// since vertex/index data is byte-identical for shared geometry.
static uint64_t HashBytes(const void* data, size_t size, uint64_t seed = 0xCBF29CE484222325ULL) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) {
        seed ^= p[i];
        seed *= 0x100000001B3ULL;
    }
    return seed;
}

static uint64_t ContentHashOf(const ExtractedMesh& m) {
    uint64_t h = 0xCBF29CE484222325ULL;
    if (!m.vertices.empty())
        h = HashBytes(m.vertices.data(), m.vertices.size() * sizeof(remixapi_HardcodedVertex), h);
    if (!m.indices.empty())
        h = HashBytes(m.indices.data(), m.indices.size() * sizeof(uint32_t), h);
    return h;
}

// ---------------------------------------------------------------------------
// First-N-catches-per-callsite logger for C++ exceptions out of dxvk-remix
// API calls. We saw 3277 caught C++ exceptions out of SubmitDrawable in the
// last session; logging every one would balloon the log to MB. First 16 per
// call site is enough to spot patterns and learn what's actually being thrown.
// ---------------------------------------------------------------------------
static std::atomic<int> g_cxxLogCount_SubmitDrawable{0};
static std::atomic<int> g_cxxLogCount_ReleaseDrawableMesh{0};
static std::atomic<int> g_cxxLogCount_ReleaseDrawableMaterial{0};
static std::atomic<int> g_cxxLogCount_ReleaseDrawableTexture{0};
static std::atomic<int> g_cxxLogCount_DrawInstance{0};
static constexpr int kCxxLogCap = 16;

// ---------------------------------------------------------------------------
// SEH wrapper for api->DrawInstance. The OnFrame draw loop iterates
// g_drawables under g_renderStateMutex (lock_guard's destructor would
// conflict with __try, C2712), so we isolate the API call here. Returns
// 0 on normal completion (with *outErr set to the API status); 1 on
// SEH catch with *outExceptionCode set.
// ---------------------------------------------------------------------------
// Inner C++ helper: catches C++ exceptions thrown out of DrawInstance and
// logs e.what() for the first kCxxLogCap occurrences. Returns 0 on clean
// success (with *outErr set to API status), 2 if a C++ exception was
// caught and swallowed (with *outErr forced to GENERAL_FAILURE).
// Factored out so the outer CallDrawInstanceGuarded can wrap it in __try
// without MSVC complaining about mixed EH forms in the same function (C2713).
static int CallDrawInstanceCxxGuarded(remixapi_Interface* api,
                                      const remixapi_InstanceInfo* instance,
                                      remixapi_ErrorCode* outErr) {
    try {
        *outErr = api->DrawInstance(instance);
        return 0;
    } catch (const std::exception& e) {
        int n = g_cxxLogCount_DrawInstance.fetch_add(1, std::memory_order_relaxed);
        if (n < kCxxLogCap) {
            _MESSAGE("FO4RemixPlugin: [OnFrame] DrawInstance C++ exception #%d what=%s",
                     n, e.what());
        }
        *outErr = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        return 2;
    } catch (...) {
        int n = g_cxxLogCount_DrawInstance.fetch_add(1, std::memory_order_relaxed);
        if (n < kCxxLogCap) {
            _MESSAGE("FO4RemixPlugin: [OnFrame] DrawInstance unknown C++ exception #%d", n);
        }
        *outErr = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        return 2;
    }
}

static int CallDrawInstanceGuarded(remixapi_Interface* api,
                                   const remixapi_InstanceInfo* instance,
                                   remixapi_ErrorCode* outErr,
                                   unsigned long* outExceptionCode) {
    __try {
        return CallDrawInstanceCxxGuarded(api, instance, outErr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outExceptionCode = GetExceptionCode();
        return 1;
    }
}

// ---------------------------------------------------------------------------
// Scene tracking state
// ---------------------------------------------------------------------------
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

// Per-hash Remix InstanceInfoBlendEXT, populated at SubmitDrawable time for
// meshes that have any alpha state (test or blend). OnFrame's DrawInstance
// loop chains the stored struct onto instance.pNext so Remix honors per-
// instance alpha state. Keyed by the mesh's `hash` field (NOT material hash)
// -- per-instance, not per-material.
static std::unordered_map<uint64_t, remixapi_InstanceInfoBlendEXT> g_geometryAlphaState;

// ---------------------------------------------------------------------------
// Phase 1B: flat per-drawable map, keyed by PassKey/drawable hash.
//
// Flat map keyed by drawable hash (== PassKey from semantic_capture).
// Mutated by SubmitDrawable/ReleaseDrawable on the game thread (via
// SemanticCapture::Tick from hkPresent). Read by OnFrame draw loop on
// the Remix thread. ALL access requires holding g_renderStateMutex.
// ---------------------------------------------------------------------------
namespace {
    struct DrawableInstance {
        remixapi_MeshHandle          meshHandle    = nullptr;  // alias into g_meshCache, owned there
        MeshCacheKey                 meshCacheKey  = {};       // identity for ReleaseDrawable refcount drop
        uint64_t                     materialHash  = 0;        // index into g_materialCache
        std::unordered_set<uint64_t> textureHashes;            // for refcount cascade
        uint64_t                     lastDrawnFrame = 0;       // stamped by OnFrame
        float                        worldTransform[3][4] = {};  // row-major 3x4 matrix; populated from mesh.worldTransform at submit time

        // Worldspace LOD chunk metadata (2026-04-28). Copied from
        // ExtractedMesh at submit time. OnFrame applies a spatial filter:
        // when isLODChunk is true and the player's world position is inside
        // [chunkOriginXY, chunkOriginXY + chunkExtent] in raw Beth coords,
        // skip drawing this instance (the in-cell static refs are already
        // rendering that region with full detail). Avoids the close-up
        // multi-LOD-level overlap symptom while keeping distant LOD chunks
        // (player outside coverage) rendering normally.
        bool                         isLODChunk    = false;
        float                        chunkOriginX  = 0.0f;
        float                        chunkOriginY  = 0.0f;
        float                        chunkExtent   = 0.0f;

        // Water tag (2026-04-29). Copied from ExtractedMesh::isWater at
        // submit time. OnFrame reads this to OR the
        // REMIXAPI_INSTANCE_CATEGORY_BIT_ANIMATED_WATER bit into the
        // remixapi_InstanceInfo::categoryFlags, which makes dxvk-remix's
        // translucent shader run its built-in dual-layer scrolling-normal
        // animation path (cf. translucent_surface_material_interaction.slangh
        // line 56 onward). Scroll velocities live on the dxvk-remix side as
        // RtxOptions::translucentMaterial::animatedWaterPrimary/Secondary-
        // NormalMotion -- we don't pass Bethesda's kNormalsScrollN through;
        // dxvk-remix's defaults are reasonable for any water surface.
        bool                         isWater       = false;

        // Alpha-blend state (2026-05-01). Copied from ExtractedMesh at submit
        // time. OnFrame chains a remixapi_InstanceInfoBlendEXT onto pNext when
        // alphaBlendEnabled is true; the material was built with
        // useDrawCallAlphaState=1 in that case so the per-instance blend state
        // is the source of truth. All bucket members share the same blend
        // state because the material cache key folds blend factors in
        // (different factors produce different materials -> different mesh
        // handles -> different buckets).
        bool                         alphaBlendEnabled    = false;
        uint32_t                     srcColorBlendFactor  = 1;  // VK_BLEND_FACTOR_ONE
        uint32_t                     dstColorBlendFactor  = 0;  // VK_BLEND_FACTOR_ZERO
        bool                         alphaTestEnabled     = false;
        uint32_t                     alphaTestType        = 7;  // VK_COMPARE_OP_ALWAYS
        uint8_t                      alphaTestRef         = 128;

        // Decal tag (2026-05-01). Copied from ExtractedMesh at submit time.
        // OnFrame ORs REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC into the
        // categoryFlags so dxvk-remix applies decal depth-offset Z-fight
        // prevention against the underlying surface.
        bool                         isDecal              = false;
    };

    std::unordered_map<uint64_t, DrawableInstance> g_drawables;

    // Mesh-handle cache keyed by (contentHash, materialHash). Refcounted; a
    // SubmitDrawable that finds a matching key reuses the existing handle and
    // bumps refCount, ReleaseDrawable drops it, on zero we DestroyMesh + erase.
    // When g_config.gpuInstancingEnabled is false the cache key has the
    // drawable hash in `contentHash`, so each drawable lands in its own bucket
    // and no sharing happens -- preserves pre-instancing behavior for rollback.
    struct MeshRef {
        remixapi_MeshHandle handle;
        uint32_t            refCount;
    };
    std::unordered_map<MeshCacheKey, MeshRef, MeshCacheKeyHash> g_meshCache;

    // Serializes SetConfigVariable writes (game thread) against OnFrame draw
    // submissions (Remix thread). Other Remix API mutations elsewhere in this
    // file rely on g_renderStateMutex and Remix's internal sync, NOT on this
    // mutex. Recursive so a path that already holds it can re-acquire safely.
    // NOTE: distinct from g_renderStateMutex below — that one guards
    // g_drawables/g_meshCache/g_materialCache/g_textureHandles. They protect
    // different invariants and are taken in order: g_remixApiMutex first.
    std::recursive_mutex g_remixApiMutex;

    // Protects g_drawables, g_meshCache, g_materialCache, and g_textureHandles.
    // Submission (SubmitDrawable / ReleaseDrawable) runs on the game thread
    // via SemanticCapture::Tick from hkPresent; drawing (OnFrame) and sweeps
    // (SweepStaleMaterials / SweepStaleTextures) run on the Remix thread.
    // Without this mutex, unordered_map insertions can rehash mid-iteration
    // and crash the iterating thread.
    std::mutex g_renderStateMutex;
}

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
// VRAM telemetry. Returns false if the Remix runtime doesn't expose
// GetVramStats (older headers / missing entry point); callers that gate
// on a VRAM budget should degrade to TTL-only mode in that case.
// ---------------------------------------------------------------------------
bool RemixRenderer::GetVramStats(VramStats* out) {
    if (!out) return false;
    *out = {};
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api || !api->GetVramStats) return false;

    remixapi_VramStats s = {};
    if (api->GetVramStats(&s) != REMIXAPI_ERROR_CODE_SUCCESS) return false;
    out->totalAllocatedBytes               = s.totalAllocatedBytes;
    out->totalUsedBytes                    = s.totalUsedBytes;
    out->poolRetainedBytes                 = s.poolRetainedBytes;
    out->usedReplacementGeometryBytes      = s.usedReplacementGeometryBytes;
    out->usedBufferBytes                   = s.usedBufferBytes;
    out->usedAccelerationStructureBytes    = s.usedAccelerationStructureBytes;
    out->usedOpacityMicromapBytes          = s.usedOpacityMicromapBytes;
    out->usedMaterialTextureBytes          = s.usedMaterialTextureBytes;
    out->usedRenderTargetBytes             = s.usedRenderTargetBytes;
    out->driverAllocatedBytes              = s.driverAllocatedBytes;
    out->driverBudgetBytes                 = s.driverBudgetBytes;
    out->forkTextureCacheCount             = s.forkTextureCacheCount;
    return true;
}

// ---------------------------------------------------------------------------
// SweepStaleMaterials -- the LEVER for VRAM reclamation.
//
// LRU sweep for materials. Refcount-driven: materials with refCount == 0 and
// age > ttlFrames are destroyed. ReleaseDrawable normally drops refcounts to
// zero when the last referencing drawable is released; this sweep is the
// backstop for any orphan entries left behind. The cell-granular cascade was
// deleted with the cell pipeline; the refCount + lastDrawnFrame combination
// is the new eviction unit. cellsEvicted in the result is always 0 in Phase
// 1B onward (kept for ABI compatibility with the periodic stats logger).
// ---------------------------------------------------------------------------
RemixRenderer::StaleMaterialSweepResult RemixRenderer::SweepStaleMaterials(
        uint64_t currentFrameIndex,
        uint64_t ttlFrames,
        uint64_t budgetBytes,
        uint64_t currentMaterialTexBytes) {
    std::lock_guard<std::mutex> lock(g_renderStateMutex);

    StaleMaterialSweepResult result{};
    result.materialCacheCount = static_cast<uint32_t>(g_materialCache.size());

    auto* api = RemixAPI::GetInterface();
    if (!api) return result;

    // TTL pass: destroy materials whose refCount is 0 and lastDrawn is stale.
    // Refcount-driven destruction: ReleaseDrawable normally drops the count to
    // zero on TTL eviction, and this sweep is the backstop for any orphan
    // entries left behind. The cell-granular cascade was deleted with the
    // cell pipeline; the refcount + lastDrawnFrame combination is the new
    // eviction unit.
    for (auto it = g_materialCache.begin(); it != g_materialCache.end();) {
        const uint64_t age = (currentFrameIndex > it->second.lastDrawnFrame)
            ? (currentFrameIndex - it->second.lastDrawnFrame) : 0;
        const bool stale = (it->second.refCount == 0) && (age > ttlFrames);
        if (stale) {
            if (it->second.handle) {
                api->DestroyMaterial(it->second.handle);
            }
            it = g_materialCache.erase(it);
            ++result.staleMaterialCount;
        } else {
            ++it;
        }
    }

    // Cells are gone; cellsEvicted is always 0 in 1B onward (kept for ABI
    // compatibility with the periodic stats logger; Task 14 commit will note
    // this is now meaningless and can be retired in a future cleanup).
    result.cellsEvicted = 0;

    // Budget pressure on materials is handled implicitly via the texture
    // sweep below (textures are the major VRAM consumer). Material handles
    // themselves are tiny by comparison.
    (void)budgetBytes;
    (void)currentMaterialTexBytes;

    return result;
}

// ---------------------------------------------------------------------------
// SweepStaleTextures -- the BACKSTOP for the material sweep.
//
// LRU sweep for textures. Refcount-driven: textures with refCount == 0 and
// age > ttlFrames are destroyed. ReleaseDrawable normally drops refcounts
// to zero when the last referencing material is destroyed; this sweep is
// the backstop for any orphans. cellsEvicted in the result struct is
// always 0 in Phase 1B onward (cells retired with the cell pipeline).
//
// Two-pass eviction logic:
//   (1) TTL pass -- collect textures un-drawn in ttlFrames.
//   (2) Budget pass -- if currentMaterialTex is over budgetBytes, add the
//       oldest non-stale textures (with min-age guardrail and per-sweep
//       cap) to the eviction set.
// Then defensive orphan-handle destruction (refCount == 0 guard).
// ---------------------------------------------------------------------------
RemixRenderer::StaleTextureSweepResult RemixRenderer::SweepStaleTextures(
        uint64_t currentFrameIndex,
        uint64_t ttlFrames,
        uint64_t budgetBytes,
        uint64_t currentMaterialTexBytes) {
    std::lock_guard<std::mutex> lock(g_renderStateMutex);

    StaleTextureSweepResult result{};
    result.textureHandleCount = static_cast<uint32_t>(g_textureHandles.size());

    // Pass 1: TTL.
    std::unordered_set<uint64_t> staleTextures;
    staleTextures.reserve(g_textureHandles.size() / 4 + 8);
    for (const auto& [hash, tex] : g_textureHandles) {
        const uint64_t age = (currentFrameIndex > tex.lastDrawnFrame)
            ? (currentFrameIndex - tex.lastDrawnFrame) : 0;
        if (age > ttlFrames) {
            staleTextures.insert(hash);
        }
    }
    result.staleTextureCount = static_cast<uint32_t>(staleTextures.size());

    // Pass 2: budget overlay. Skyrim's guardrails: min-age 120 frames (~2s
    // @ 60fps so we don't evict things drawn this second), per-sweep cap 32
    // so a single sweep can't catastrophically drop everything.
    if (budgetBytes > 0 && currentMaterialTexBytes > budgetBytes) {
        constexpr uint64_t kBudgetMinAgeFrames        = 120;
        constexpr uint32_t kBudgetMaxEvictionsPerSweep = 32;

        std::vector<std::pair<uint64_t, uint64_t>> byAge;  // (age, hash)
        byAge.reserve(g_textureHandles.size());
        for (const auto& [hash, tex] : g_textureHandles) {
            if (staleTextures.count(hash)) continue;
            const uint64_t age = (currentFrameIndex > tex.lastDrawnFrame)
                ? (currentFrameIndex - tex.lastDrawnFrame) : 0;
            if (age < kBudgetMinAgeFrames) continue;
            byAge.emplace_back(age, hash);
        }
        std::sort(byAge.begin(), byAge.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });

        const uint64_t toAdd = (std::min<uint64_t>)(kBudgetMaxEvictionsPerSweep, byAge.size());
        for (uint64_t i = 0; i < toAdd; ++i) {
            staleTextures.insert(byAge[i].second);
            ++result.budgetEvictions;
        }
    }

    if (staleTextures.empty()) {
        return result;
    }

    // Orphan cleanup: textures the cell-unload didn't already release. With
    // the global texture refcount, an entry only survives here if some cell
    // we did NOT unload still references it -- but defensive sweep doesn't
    // hurt, and budget-pass entries that never had a cell owner go through
    // here.
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (api) {
        for (uint64_t texHash : staleTextures) {
            auto texIt = g_textureHandles.find(texHash);
            if (texIt != g_textureHandles.end() && texIt->second.refCount == 0) {
                if (texIt->second.handle) api->DestroyTexture(texIt->second.handle);
                g_textureHandles.erase(texIt);
                ++result.orphanTexturesDestroyed;
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Phase 1B: SubmitDrawable / ReleaseDrawable
// ---------------------------------------------------------------------------

// Backout helpers for a failed SubmitDrawable. They decrement refcounts only --
// they do NOT call DestroyTexture / DestroyMaterial even when the count reaches
// zero. Inline destruction here is what was driving the
// CreateTexture/DestroyTexture churn cycle: the resolver retries the same
// failing drawable on subsequent frames, so each retry would create-then-destroy
// the same handles, eventually corrupting dxvk-remix internal state and making
// every later DestroyTexture / DestroyMaterial throw 0xE06D7363. Leave orphans
// for the LRU sweep to collect naturally; the cost is a few stale entries
// living for ttlFrames, which the existing sweep cadence handles fine.
// Callers must hold g_renderStateMutex.
static void DecrementTextureRefs(const std::unordered_set<uint64_t>& hashes) {
    for (uint64_t h : hashes) {
        auto it = g_textureHandles.find(h);
        if (it != g_textureHandles.end() && it->second.refCount > 0) {
            --it->second.refCount;
        }
    }
}

static void DecrementMaterialRef(uint64_t matHash) {
    if (matHash == 0) return;
    auto it = g_materialCache.find(matHash);
    if (it != g_materialCache.end() && it->second.refCount > 0) {
        --it->second.refCount;
    }
}

// Drop a refCount on a g_meshCache entry. On zero we DestroyMesh and erase.
// Same per-call try/catch idiom as the texture/material destroy paths so a
// throw out of dxvk-remix is logged once and does not corrupt our state.
// Caller must hold g_renderStateMutex.
static void DecrementMeshCacheRef(const MeshCacheKey& key) {
    auto it = g_meshCache.find(key);
    if (it == g_meshCache.end()) return;
    if (it->second.refCount > 0) --it->second.refCount;
    if (it->second.refCount != 0) return;

    remixapi_Interface* api = RemixAPI::GetInterface();
    if (api && it->second.handle) {
        try {
            api->DestroyMesh(it->second.handle);
        } catch (const std::exception& e) {
            int n = g_cxxLogCount_ReleaseDrawableMesh.fetch_add(1, std::memory_order_relaxed);
            if (n < kCxxLogCap) {
                _MESSAGE("FO4RemixPlugin: [ReleaseDrawable] DestroyMesh (cache) C++ exception #%d content=0x%llX mat=0x%llX what=%s",
                         n, (unsigned long long)key.contentHash,
                         (unsigned long long)key.materialHash, e.what());
            }
        } catch (...) {
            int n = g_cxxLogCount_ReleaseDrawableMesh.fetch_add(1, std::memory_order_relaxed);
            if (n < kCxxLogCap) {
                _MESSAGE("FO4RemixPlugin: [ReleaseDrawable] DestroyMesh (cache) unknown C++ exception #%d content=0x%llX mat=0x%llX",
                         n, (unsigned long long)key.contentHash,
                         (unsigned long long)key.materialHash);
            }
        }
    }
    g_meshCache.erase(it);
}

RemixRenderer::SubmitStatus RemixRenderer::SubmitDrawable(
        uint64_t hash,
        const ExtractedMesh& mesh,
        const std::vector<ExtractedTexture>& newTextures) {

    std::lock_guard<std::mutex> lock(g_renderStateMutex);

    // C++ exception fence: dxvk-remix API calls (CreateTexture/CreateMaterial/
    // CreateMesh) can throw under VRAM pressure or other failure modes. Letting
    // those bubble up corrupts dxvk-remix's internal Vulkan command queue and
    // freezes the render thread (observed: 3277 caught C++ exceptions /
    // 0xE06D7363 in last session). Catch here, log e.what() for the first
    // kCxxLogCap occurrences, and return kFailed cleanly. Any partial state
    // (texture refs bumped before the throw) leaks until LRU sweep collects;
    // accepted cost.
    try {
    // Idempotent: already submitted, nothing to do
    if (g_drawables.find(hash) != g_drawables.end()) {
        return SubmitStatus::kSubmitted;
    }

    // No diffuse means no lit material — caller (resolver) is supposed to gate
    // on this and retry next frame. Defensive guard.
    if (mesh.diffuseTextureHash == 0) {
        return SubmitStatus::kFailed;
    }

    // Defensive: dxvk-remix CreateMesh / CreateTexture can throw on degenerate
    // inputs (empty vertex buffer, empty pixel data). Bail early -- saves an
    // api->CreateX call that would just throw and corrupt dxvk-remix internal
    // state. The trace step here lets the SEH catch (if a later call still
    // throws on this drawable for an unrelated reason) report which gate fired.
    if (mesh.vertices.empty()) {
        Resolvers::Trace::SetStep(Resolvers::Trace::kSubmit_GateInputEmpty);
        return SubmitStatus::kFailed;
    }

    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api) return SubmitStatus::kFailed;

    DrawableInstance inst;

    // ---- Texture upload + cache ----
    // Per-texture upload + cache loop: refCount++ on hit, insert at refCount=1 on miss.
    for (const auto& tex : newTextures) {
        auto existing = g_textureHandles.find(tex.hash);
        if (existing != g_textureHandles.end()) {
            // Already cached by another cell or drawable; bump refcount.
            existing->second.refCount++;
            inst.textureHashes.insert(tex.hash);
            continue;
        }

        // Defensive: skip degenerate textures (empty pixel data / 0 dims). dxvk-remix
        // CreateTexture has been observed to throw on these. Skipping here is safer
        // than the throw-and-corrupt-internal-state path. Refcounts NOT bumped and
        // g_textureHandles NOT inserted -- material lookup will just hit the
        // hash-path-as-string fallback for this slot.
        if (tex.pixels.empty() || tex.width == 0 || tex.height == 0) {
            continue;
        }

        remixapi_Format remixFmt = DxgiToRemixFormat(tex.dxgiFormat);
        if (remixFmt == (remixapi_Format)0) {
            // Unsupported format; skip — material will reference hash path anyway.
            continue;
        }

        remixapi_TextureInfo texInfo = {};
        texInfo.sType     = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
        texInfo.pNext     = nullptr;
        texInfo.hash      = tex.hash;
        texInfo.width     = tex.width;
        texInfo.height    = tex.height;
        texInfo.depth     = 1;
        texInfo.mipLevels = tex.mipLevels > 0 ? tex.mipLevels : 1;
        texInfo.format    = remixFmt;
        texInfo.data      = tex.pixels.data();
        texInfo.dataSize  = tex.pixels.size();

        remixapi_TextureHandle texHandle = nullptr;
        Resolvers::Trace::SetStep(Resolvers::Trace::kSubmit_BeforeTextureCreate);
        remixapi_ErrorCode status = api->CreateTexture(&texInfo, &texHandle);
        Resolvers::Trace::SetStep(Resolvers::Trace::kSubmit_AfterTextureCreate);
        if (status != REMIXAPI_ERROR_CODE_SUCCESS || !texHandle) {
            _MESSAGE("FO4RemixPlugin: [SubmitDrawable] Failed to upload texture 0x%llX (error %d)",
                     (unsigned long long)tex.hash, (int)status);
            continue;
        }

        g_textureHandles[tex.hash] = { texHandle, 1, Diagnostics::CurrentFrameIndex() };
        inst.textureHashes.insert(tex.hash);
    }

    // ---- Validate texture upload completeness ----
    // The resolver computed mesh.{diffuse,normal,roughness,emissive}TextureHash
    // from raw texture data, but the per-texture upload loop above can skip
    // textures (empty pixels, zero dims, unsupported format, CreateTexture
    // failure). If we still build a material referencing those skipped hashes,
    // dxvk-remix carries a dangling reference that hangs its overlay refresh
    // (alt-tab to the Remix window walks all materials and validates refs --
    // hundreds of dangling refs freeze the runtime). Bail if diffuse failed,
    // and zero out optional refs so the material is built without them.
    auto loaded = [&](uint64_t h) {
        return h == 0 || g_textureHandles.find(h) != g_textureHandles.end();
    };

    if (!loaded(mesh.diffuseTextureHash)) {
        // Diffuse missing: this drawable can't render lit. Drop refcounts on
        // any textures we bumped during the upload loop, but DON'T destroy
        // them here -- see DecrementTextureRefs comment for why.
        DecrementTextureRefs(inst.textureHashes);
        return SubmitStatus::kFailed;
    }

    const uint64_t normalH    = loaded(mesh.normalTextureHash)    ? mesh.normalTextureHash    : 0;
    const uint64_t roughnessH = loaded(mesh.roughnessTextureHash) ? mesh.roughnessTextureHash : 0;
    const uint64_t emissiveH  = loaded(mesh.emissiveTextureHash)  ? mesh.emissiveTextureHash  : 0;

    // ---- Material cache ----
    // Hash the texture combination to get a stable per-material cache key.
    // On hit, bump refCount + lastDrawnFrame. On miss, create a new Remix
    // material handle and insert at refCount=1.
    uint64_t matHash = 0;
    remixapi_MaterialHandle matHandle = nullptr;

    if (mesh.diffuseTextureHash != 0) {
        // Hash diffuse + normal + roughness + emissive hashes plus emissive
        // colour components and alpha flags into a single 64-bit cache key.
        uint64_t h = mesh.diffuseTextureHash;
        h ^= normalH    * 0x517CC1B727220A95ULL;
        h ^= roughnessH * 0x6C62272E07BB0142ULL;
        h ^= emissiveH  * 0x9E3779B97F4A7C15ULL;
        uint32_t ri, gi, bi, ii;
        memcpy(&ri, &mesh.emissiveColorR, 4);
        memcpy(&gi, &mesh.emissiveColorG, 4);
        memcpy(&bi, &mesh.emissiveColorB, 4);
        memcpy(&ii, &mesh.emissiveIntensity, 4);
        h ^= (uint64_t)ri * 0x85EBCA6BC2B2AE35ULL;
        h ^= (uint64_t)gi * 0xC2B2AE3D27D4EB4FULL;
        h ^= (uint64_t)bi * 0x165667B19E3779F9ULL;
        h ^= (uint64_t)ii * 0x27D4EB2F165667C5ULL;
        bool useDrawCall = mesh.alphaTestEnabled || mesh.alphaBlendEnabled;
        h ^= (uint64_t)(useDrawCall ? 1 : 0) * 0x9FB21C651E98DF25ULL;
        // Fold blend factors so different blend modes get different material
        // handles. (Different opaqueExt.useDrawCallAlphaState values are
        // already covered by the useDrawCall fold above; this fold splits
        // distinct blend modes within the alpha-blend-enabled population
        // so all drawables in a bucket share the same blend state -- which
        // OnFrame relies on when populating InstanceInfoBlendEXT from
        // bucket.members[0].)
        h ^= (uint64_t)mesh.srcColorBlendFactor * 0xCC9E2D51ULL;
        h ^= (uint64_t)mesh.dstColorBlendFactor * 0x1B873593ULL;
        // alphaTest fields are a material-level concern (they go on the
        // opaqueExt itself when useDrawCallAlphaState=0, or on the per-
        // instance blend ext when =1); fold them so test-on vs test-off
        // and distinct test thresholds never share materials.
        h ^= (uint64_t)(mesh.alphaTestEnabled ? 1 : 0) * 0xE6546B64ULL;
        h ^= (uint64_t)mesh.alphaTestType * 0x85EBCA77ULL;
        h ^= (uint64_t)mesh.alphaTestRef  * 0xC2B2AE3DULL;
        // Fold water tag + transmittance into the cache key so opaque and
        // translucent variants of the same texture set never share a Remix
        // material handle (the underlying pNext extension struct differs).
        h ^= (uint64_t)(mesh.isWater ? 1 : 0) * 0xD6E8FEB86659FD93ULL;
        if (mesh.isWater) {
            uint32_t wri, wgi, wbi;
            memcpy(&wri, &mesh.waterTransmittanceR, 4);
            memcpy(&wgi, &mesh.waterTransmittanceG, 4);
            memcpy(&wbi, &mesh.waterTransmittanceB, 4);
            h ^= (uint64_t)wri * 0xBF58476D1CE4E5B9ULL;
            h ^= (uint64_t)wgi * 0x94D049BB133111EBULL;
            h ^= (uint64_t)wbi * 0xCBF29CE484222325ULL;
        }
        matHash = h;

        auto cacheIt = g_materialCache.find(matHash);
        if (cacheIt != g_materialCache.end()) {
            // Already in cache; bump refcount.
            cacheIt->second.refCount++;
            cacheIt->second.lastDrawnFrame = Diagnostics::CurrentFrameIndex();
            matHandle = cacheIt->second.handle;
        } else {
            // Cache miss — create a new Remix material handle and insert at refCount=1.
            std::wstring diffPath     = HashToPath(mesh.diffuseTextureHash);
            std::wstring normalPath   = normalH    ? HashToPath(normalH)    : L"";
            std::wstring roughPath    = roughnessH ? HashToPath(roughnessH) : L"";
            std::wstring emissivePath = emissiveH  ? HashToPath(emissiveH)  : L"";

            // Build the appropriate pNext extension chain: translucent for
            // water surfaces (Fresnel-reflective refractive medium), opaque
            // for everything else. Both feed the same MaterialInfo parent.
            remixapi_MaterialInfoOpaqueEXT      opaqueExt      = {};
            remixapi_MaterialInfoTranslucentEXT translucentExt = {};
            void* pNext = nullptr;

            if (mesh.isWater) {
                // Translucent BRDF parameters. transmittanceColor comes from
                // BSWaterShaderMaterial::kDeepColor, captured at extraction
                // time -- per-worldspace water tinting (Far Harbor swamp
                // green, Sanctuary blue, Glowing Sea sludge) flows through
                // automatically without us baking constants per region.
                //
                // refractiveIndex 1.33: water IOR (physical constant). The
                // path tracer uses it for Fresnel + refraction angle.
                //
                // transmittanceMeasurementDistance 500.0 (cm): "thickness
                // over which transmittanceColor saturates." Water bodies in
                // FO4 are deep enough that 5m gives a solid tint. Tunable.
                //
                // thinWallThickness_hasvalue 0: water is a volume, not a
                // pane. Setting hasvalue=1 would model a glass-like sheet.
                //
                // useDiffuseLayer 0: don't sample the synthetic blue albedo.
                // The synth diffuse exists only to satisfy SubmitDrawable's
                // diffuse-loaded gate; it would muddy the refractive look
                // if the BRDF mixed it in.
                translucentExt.sType                            = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
                translucentExt.pNext                            = nullptr;
                translucentExt.transmittanceTexture             = nullptr;
                translucentExt.refractiveIndex                  = 1.33f;
                translucentExt.transmittanceColor               = { mesh.waterTransmittanceR,
                                                                    mesh.waterTransmittanceG,
                                                                    mesh.waterTransmittanceB };
                translucentExt.transmittanceMeasurementDistance = 500.0f;
                translucentExt.thinWallThickness_hasvalue       = 0;
                translucentExt.thinWallThickness_value          = 0.0f;
                translucentExt.useDiffuseLayer                  = 0;
                pNext = &translucentExt;
            } else {
                opaqueExt.sType             = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
                opaqueExt.pNext             = nullptr;
                opaqueExt.roughnessTexture  = roughnessH ? roughPath.c_str() : nullptr;
                opaqueExt.metallicTexture   = nullptr;
                opaqueExt.heightTexture     = nullptr;
                opaqueExt.albedoConstant    = { 1.0f, 1.0f, 1.0f };
                opaqueExt.opacityConstant   = 1.0f;
                opaqueExt.roughnessConstant = roughnessH ? 0.5f : 0.8f;
                opaqueExt.metallicConstant  = 0.0f;
                // Per remix_c.h: useDrawCallAlphaState=1 means "use Instance-
                // InfoBlendEXT as alpha state source" -- Remix consumes the
                // per-instance blend ext (chained at DrawInstance time in
                // OnFrame) for BOTH alpha test and alpha blend; the material-
                // level alphaTestType/alphaReferenceValue fields are ignored.
                //
                // alphaBlendEnabled=true: switch to per-instance state. OnFrame
                //   builds the InstanceInfoBlendEXT from DrawableInstance's
                //   srcColorBlendFactor/dstColorBlendFactor/alphaTest* fields.
                //   The material-level alpha fields are zeroed because they're
                //   ignored in this mode.
                //
                // alphaBlendEnabled=false: keep the material-level alpha-test
                //   path. Remix uses opaqueExt.alphaTestType +
                //   alphaReferenceValue, no blend ext needed. This preserves
                //   the existing alpha-test cutout behavior on foliage and
                //   chain-link without churning the bucket layout.
                if (mesh.alphaBlendEnabled) {
                    opaqueExt.useDrawCallAlphaState = 1;
                    opaqueExt.alphaTestType         = 7;
                    opaqueExt.alphaReferenceValue   = 0;
                } else {
                    opaqueExt.useDrawCallAlphaState = 0;
                    opaqueExt.alphaTestType         = mesh.alphaTestEnabled ? mesh.alphaTestType : 7;
                    opaqueExt.alphaReferenceValue   = mesh.alphaTestEnabled ? mesh.alphaTestRef  : 0;
                }
                pNext = &opaqueExt;
            }

            remixapi_MaterialInfo matInfo = {};
            matInfo.sType              = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
            matInfo.pNext              = pNext;
            matInfo.hash               = matHash;
            matInfo.albedoTexture      = diffPath.c_str();
            matInfo.normalTexture      = normalH   ? normalPath.c_str()   : nullptr;
            matInfo.tangentTexture     = nullptr;
            matInfo.emissiveTexture       = emissiveH ? emissivePath.c_str() : nullptr;
            matInfo.emissiveIntensity     = mesh.emissiveIntensity * g_config.emissiveIntensity;
            matInfo.emissiveColorConstant = { mesh.emissiveColorR, mesh.emissiveColorG, mesh.emissiveColorB };
            matInfo.spriteSheetRow     = 1;
            matInfo.spriteSheetCol     = 1;
            matInfo.spriteSheetFps     = 0;
            matInfo.filterMode         = 1;
            matInfo.wrapModeU          = 1;
            matInfo.wrapModeV          = 1;

            remixapi_MaterialHandle newHandle = nullptr;
            Resolvers::Trace::SetStep(Resolvers::Trace::kSubmit_BeforeMaterialCreate);
            remixapi_ErrorCode matStatus = api->CreateMaterial(&matInfo, &newHandle);
            Resolvers::Trace::SetStep(Resolvers::Trace::kSubmit_AfterMaterialCreate);
            if (matStatus != REMIXAPI_ERROR_CODE_SUCCESS || !newHandle) {
                _MESSAGE("FO4RemixPlugin: [SubmitDrawable] Failed to create material 0x%llX (error %d)",
                         (unsigned long long)matHash, (int)matStatus);
                DecrementTextureRefs(inst.textureHashes);
                return SubmitStatus::kFailed;
            }

            g_materialCache[matHash] = { newHandle, 1, Diagnostics::CurrentFrameIndex() };
            matHandle = newHandle;
        }
    }
    inst.materialHash = matHash;

    // ---- Mesh handle (shared via g_meshCache) ----
    // Build cache key. When GpuInstancing is disabled, slot the drawable hash
    // into contentHash so each drawable gets its own cache entry (no sharing,
    // preserving pre-instancing behavior). When enabled, contentHash is a
    // byte-stable hash over (vertices, indices), so identical NIF instances
    // with identical materials share a single cached handle.
    MeshCacheKey meshKey{};
    meshKey.contentHash  = g_config.gpuInstancingEnabled ? ContentHashOf(mesh) : hash;
    meshKey.materialHash = matHash;

    remixapi_MeshHandle meshHandle = nullptr;
    auto cacheIt = g_meshCache.find(meshKey);
    if (cacheIt != g_meshCache.end()) {
        cacheIt->second.refCount++;
        meshHandle = cacheIt->second.handle;
    } else {
        // Cache miss -- create a new Remix mesh handle. Use the content hash
        // as the Remix-side mesh hash so USD replacement matching is stable
        // across game runs (instead of the per-drawable PassKey, which isn't).
        remixapi_MeshInfoSurfaceTriangles surface = {};
        surface.vertices_values = mesh.vertices.data();
        surface.vertices_count  = (uint32_t)mesh.vertices.size();
        surface.indices_values  = mesh.indices.empty() ? nullptr : mesh.indices.data();
        surface.indices_count   = (uint32_t)mesh.indices.size();
        surface.skinning_hasvalue = 0;
        surface.material = matHandle;

        remixapi_MeshInfo meshInfo = {};
        meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
        meshInfo.hash = meshKey.contentHash;
        meshInfo.surfaces_values = &surface;
        meshInfo.surfaces_count  = 1;

        Resolvers::Trace::SetStep(Resolvers::Trace::kSubmit_BeforeMeshCreate);
        remixapi_ErrorCode meshStatus = api->CreateMesh(&meshInfo, &meshHandle);
        Resolvers::Trace::SetStep(Resolvers::Trace::kSubmit_AfterMeshCreate);
        if (meshStatus != REMIXAPI_ERROR_CODE_SUCCESS || !meshHandle) {
            _MESSAGE("FO4RemixPlugin: [SubmitDrawable] Failed to create mesh content=0x%llX mat=0x%llX (error %d)",
                     (unsigned long long)meshKey.contentHash,
                     (unsigned long long)meshKey.materialHash, (int)meshStatus);
            DecrementTextureRefs(inst.textureHashes);
            DecrementMaterialRef(matHash);
            return SubmitStatus::kFailed;
        }

        g_meshCache[meshKey] = { meshHandle, 1 };
    }

    inst.meshHandle     = meshHandle;
    inst.meshCacheKey   = meshKey;
    inst.lastDrawnFrame = Diagnostics::CurrentFrameIndex();

    // Save the world transform; the OnFrame draw loop reads it back per-frame.
    // Resolver-provided in row-major 3x4 layout matching remixapi_Transform.matrix.
    memcpy(inst.worldTransform, mesh.worldTransform, sizeof(inst.worldTransform));

    // Worldspace LOD chunk metadata for the OnFrame spatial filter.
    inst.isLODChunk   = mesh.isLODChunk;
    inst.chunkOriginX = mesh.chunkOriginX;
    inst.chunkOriginY = mesh.chunkOriginY;
    inst.chunkExtent  = mesh.chunkExtent;
    inst.isWater      = mesh.isWater;

    // Alpha-blend + alpha-test state for the OnFrame InstanceInfoBlendEXT
    // chain (only consumed when alphaBlendEnabled=true; the material was
    // built with useDrawCallAlphaState=1 in that case so the per-instance
    // state is the source of truth).
    inst.alphaBlendEnabled    = mesh.alphaBlendEnabled;
    inst.srcColorBlendFactor  = mesh.srcColorBlendFactor;
    inst.dstColorBlendFactor  = mesh.dstColorBlendFactor;
    inst.alphaTestEnabled     = mesh.alphaTestEnabled;
    inst.alphaTestType        = mesh.alphaTestType;
    inst.alphaTestRef         = mesh.alphaTestRef;

    // Decal tag for the OnFrame DECAL_STATIC categoryFlag.
    inst.isDecal              = mesh.isDecal;

    g_drawables[hash] = std::move(inst);
    return SubmitStatus::kSubmitted;
    } catch (const std::exception& e) {
        int n = g_cxxLogCount_SubmitDrawable.fetch_add(1, std::memory_order_relaxed);
        if (n < kCxxLogCap) {
            _MESSAGE("FO4RemixPlugin: [SubmitDrawable] C++ exception #%d hash=0x%llX what=%s",
                     n, (unsigned long long)hash, e.what());
        }
        return SubmitStatus::kFailed;
    } catch (...) {
        int n = g_cxxLogCount_SubmitDrawable.fetch_add(1, std::memory_order_relaxed);
        if (n < kCxxLogCap) {
            _MESSAGE("FO4RemixPlugin: [SubmitDrawable] unknown C++ exception #%d hash=0x%llX",
                     n, (unsigned long long)hash);
        }
        return SubmitStatus::kFailed;
    }
}

void RemixRenderer::ReleaseDrawable(uint64_t hash) {
    std::lock_guard<std::mutex> lock(g_renderStateMutex);

    auto it = g_drawables.find(hash);
    if (it == g_drawables.end()) return;

    DrawableInstance& inst = it->second;

    remixapi_Interface* api = RemixAPI::GetInterface();

    // Drop our refCount on the cached mesh handle. DestroyMesh only fires when
    // the last drawable using this (content, material) bucket releases. The
    // alias in inst.meshHandle is cleared to avoid any double-touch later.
    if (inst.meshHandle) {
        DecrementMeshCacheRef(inst.meshCacheKey);
        inst.meshHandle = nullptr;
    }

    // Decrement material refcount; on zero, destroy the material handle.
    // This is INDEPENDENT of the texture decrement below — material and texture
    // lifecycles are tracked separately so shared resources are freed correctly.
    // Per-call C++ catch around DestroyMaterial: refcount mutation + erase
    // run regardless so we don't leak the cache entry when the API throws.
    if (inst.materialHash != 0) {
        auto matIt = g_materialCache.find(inst.materialHash);
        if (matIt != g_materialCache.end()) {
            if (matIt->second.refCount > 0) matIt->second.refCount--;
            if (matIt->second.refCount == 0) {
                if (matIt->second.handle && api) {
                    try {
                        api->DestroyMaterial(matIt->second.handle);
                    } catch (const std::exception& e) {
                        int n = g_cxxLogCount_ReleaseDrawableMaterial.fetch_add(1, std::memory_order_relaxed);
                        if (n < kCxxLogCap) {
                            _MESSAGE("FO4RemixPlugin: [ReleaseDrawable] DestroyMaterial C++ exception #%d matHash=0x%llX what=%s",
                                     n, (unsigned long long)inst.materialHash, e.what());
                        }
                    } catch (...) {
                        int n = g_cxxLogCount_ReleaseDrawableMaterial.fetch_add(1, std::memory_order_relaxed);
                        if (n < kCxxLogCap) {
                            _MESSAGE("FO4RemixPlugin: [ReleaseDrawable] DestroyMaterial unknown C++ exception #%d matHash=0x%llX",
                                     n, (unsigned long long)inst.materialHash);
                        }
                    }
                }
                g_materialCache.erase(matIt);
            }
        }
    }

    // ALWAYS decrement this drawable's texture refcounts, independent of
    // material lifecycle. Each drawable holds its own reference to each
    // texture it uses (bumped in SubmitDrawable's texture-upload loop),
    // so we must release those refs here even if the material is still
    // alive (shared by another drawable). Nesting this inside the
    // refCount==0 block would leak one refcount per release that doesn't
    // destroy the material.
    // Per-call C++ catch around DestroyTexture: refcount mutation + erase
    // run regardless so we don't leak cache entries when the API throws.
    if (api) {
        for (uint64_t texHash : inst.textureHashes) {
            auto texIt = g_textureHandles.find(texHash);
            if (texIt != g_textureHandles.end()) {
                if (texIt->second.refCount > 0) texIt->second.refCount--;
                if (texIt->second.refCount == 0) {
                    if (texIt->second.handle) {
                        try {
                            api->DestroyTexture(texIt->second.handle);
                        } catch (const std::exception& e) {
                            int n = g_cxxLogCount_ReleaseDrawableTexture.fetch_add(1, std::memory_order_relaxed);
                            if (n < kCxxLogCap) {
                                _MESSAGE("FO4RemixPlugin: [ReleaseDrawable] DestroyTexture C++ exception #%d texHash=0x%llX what=%s",
                                         n, (unsigned long long)texHash, e.what());
                            }
                        } catch (...) {
                            int n = g_cxxLogCount_ReleaseDrawableTexture.fetch_add(1, std::memory_order_relaxed);
                            if (n < kCxxLogCap) {
                                _MESSAGE("FO4RemixPlugin: [ReleaseDrawable] DestroyTexture unknown C++ exception #%d texHash=0x%llX",
                                         n, (unsigned long long)texHash);
                            }
                        }
                    }
                    g_textureHandles.erase(texIt);
                }
            }
        }
    }

    g_drawables.erase(it);
}

// ---------------------------------------------------------------------------
// Per-frame rendering
// ---------------------------------------------------------------------------
void RemixRenderer::OnFrame(const CameraState& cam,
                            const OverlayData& overlay) {
    // Serializes Remix API option-registry writes (game thread, via
    // SetConfigVariable) against draw submissions on this thread. Held for
    // the function's full duration. Lock order: g_remixApiMutex ->
    // g_drawableMutex (SnapshotActiveDrawables) -> g_renderStateMutex
    // (bucket build). Never acquire in reverse.
    std::lock_guard<std::recursive_mutex> lock(g_remixApiMutex);
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

    bool hasAnyMeshes = false;

    // Snapshot the set of "engine-active" drawables -- those whose
    // GetRenderPasses hook has fired within the last kActiveAgeFrames frames.
    // 2026-04-29: window widened from 60 to TTL. Engine fires GetRenderPasses
    // roughly once per cell-load for HQ static refs, not per-frame; a tight
    // 60-frame window starved them after ~1 second of silence and produced
    // the "LOD vanishes but no HQ replacement" symptom (data: active=328 of
    // 5331 submitted, 94% silenced). The chunk-spatial filter handles the
    // worldspace-LOD-overlap case the active-set filter was originally added
    // for. With the window matched to TTL, this snapshot is now equivalent
    // to "every cached drawable" and could be elided in a follow-up; kept
    // for now to retain the per-flag activeStats diagnostic.
    // Done before acquiring g_renderStateMutex so we don't violate the
    // existing g_drawableMutex -> g_renderStateMutex lock order.
    constexpr uint64_t kActiveAgeFrames = 18000;  // == kTTLFrames; effectively unbounded for play
    std::unordered_set<uint64_t> activeSet;
    SemanticCapture::ActiveFlagStats activeStats;
    // Per-frame live poses (animated statics: doors, gates, machinery).
    // Populated from DrawableState::liveWorldTransform (refreshed on every
    // hook fire). Applied to DrawableInstance::worldTransform below before
    // bucket-build so animated drawables render at their current pose.
    std::unordered_map<uint64_t, std::array<float, 12>> livePoses;
    SemanticCapture::SnapshotActiveDrawables(Diagnostics::CurrentFrameIndex(),
                                             kActiveAgeFrames, activeSet,
                                             &activeStats, &livePoses);

    // Phase 1B: draw event-driven drawables. Bucket by Remix mesh handle so
    // drawables sharing a cached handle (identical content + material) collapse
    // into a single DrawInstance via remixapi_InstanceInfoGpuInstancingEXT.
    // Buckets of size 1 fall through to the simple path. Stamp lastDrawnFrame
    // on each successful draw's material + textures so the LRU sweep sees them
    // as live.
    size_t drawableCount = 0;
    size_t bucketCount = 0;
    size_t batchedBucketCount = 0;
    size_t skippedInactive = 0;
    size_t skippedChunkPlayerInside = 0;
    {
        std::lock_guard<std::mutex> lock(g_renderStateMutex);
        const uint64_t currentFrame = Diagnostics::CurrentFrameIndex();
        drawableCount = g_drawables.size();

        // Worldspace LOD chunk spatial filter: skip drawing chunks whose
        // coverage area contains the player. The in-cell static refs render
        // the player's immediate area at full detail; rendering the chunk
        // on top causes the visible up-close low-quality overlay symptom.
        // playerWorldPos is in raw Beth coords (camera.cpp populates it
        // directly from the player ref's pos at +0xD0).
        const float playerX = cam.playerWorldPos[0];
        const float playerY = cam.playerWorldPos[1];

        struct DrawBucket {
            std::vector<DrawableInstance*> members;
            std::vector<remixapi_Transform> transforms;  // built only for batched path
        };
        std::unordered_map<remixapi_MeshHandle, DrawBucket> buckets;
        buckets.reserve(g_drawables.size());

        for (auto& [drawHash, inst] : g_drawables) {
            if (!inst.meshHandle) continue;
            // Engine-active filter: skip drawables the engine stopped firing
            // for. Their mesh handles stay cached in g_meshCache so we resume
            // drawing instantly when the engine starts firing again.
            if (activeSet.find(drawHash) == activeSet.end()) {
                ++skippedInactive;
                continue;
            }
            // Apply live world transform if the hook captured one this frame
            // (or recently). Animated statics evaluate their controllers
            // before GetRenderPasses fires, so the live transform reflects
            // the current pose. Drawables without a live transform fall back
            // to the baked transform from SubmitDrawable.
            auto poseIt = livePoses.find(drawHash);
            if (poseIt != livePoses.end()) {
                const auto& pose = poseIt->second;
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        inst.worldTransform[r][c] = pose[r * 4 + c];
                    }
                }
            }
            // Spatial filter for worldspace LOD chunks: skip if player is
            // inside the chunk's coverage box. (Beth coords; chunk pivot is
            // the chunk's origin corner, extent is its side length.)
            if (inst.isLODChunk && inst.chunkExtent > 0.0f) {
                const float chunkMaxX = inst.chunkOriginX + inst.chunkExtent;
                const float chunkMaxY = inst.chunkOriginY + inst.chunkExtent;
                if (playerX >= inst.chunkOriginX && playerX <= chunkMaxX &&
                    playerY >= inst.chunkOriginY && playerY <= chunkMaxY) {
                    ++skippedChunkPlayerInside;
                    continue;
                }
            }
            buckets[inst.meshHandle].members.push_back(&inst);
        }
        bucketCount = buckets.size();

        for (auto& [meshHandle, bucket] : buckets) {
            if (bucket.members.empty()) continue;

            remixapi_InstanceInfo instance = {};
            instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
            instance.pNext = nullptr;
            instance.mesh = meshHandle;
            instance.doubleSided = 1;
            instance.categoryFlags = 0;

            // Animated water tag. Bucket members all share materialHash, and
            // matHash folds isWater (see SubmitDrawable cache-key code), so a
            // bucket is either all-water or all-not -- testing the first
            // member is sufficient. Setting the bit makes dxvk-remix's
            // translucent shader run its dual-layer scrolling-normal path
            // (cf. translucent_surface_material_interaction.slangh L56-81).
            // Scroll velocities + enable flag are dxvk-remix runtime config;
            // defaults (0.05/0.05 primary, -0.03/-0.06 secondary, enabled)
            // produce convincing water motion without per-game tuning.
            if (!bucket.members.empty() && bucket.members[0]->isWater) {
                instance.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_ANIMATED_WATER;
            }

            // Decal tag (2026-05-01). All bucket members share isDecal because
            // the static decal bit is per-mesh and the bucket key (mesh handle)
            // implies identical mesh + material. Setting DECAL_STATIC makes
            // dxvk-remix apply its decal depth-offset Z-fight-prevention pass
            // against the underlying surface so the decal blends cleanly
            // instead of fighting for the same depth.
            if (!bucket.members.empty() && bucket.members[0]->isDecal) {
                instance.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC;
            }

            remixapi_InstanceInfoGpuInstancingEXT gpuExt   = {};
            remixapi_InstanceInfoBlendEXT         blendExt = {};

            // Per-instance blend ext (2026-05-01). Built when the bucket's
            // drawables need per-instance alpha state. The material was built
            // in SubmitDrawable with useDrawCallAlphaState=1 in this case, so
            // Remix consumes this struct as the source of BOTH alpha test and
            // alpha blend (material-level alpha fields are ignored). All
            // bucket members share blend state because the material cache key
            // folds in src/dstColorBlendFactor + alpha-test fields, so reading
            // from members[0] is correct for the whole bucket.
            const bool needBlendExt = !bucket.members.empty() &&
                                       bucket.members[0]->alphaBlendEnabled;
            if (needBlendExt) {
                const DrawableInstance* m = bucket.members[0];
                blendExt.sType                      = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
                blendExt.pNext                      = nullptr;
                blendExt.alphaTestEnabled           = m->alphaTestEnabled ? 1 : 0;
                blendExt.alphaTestReferenceValue    = m->alphaTestRef;
                blendExt.alphaTestCompareOp         = m->alphaTestType;
                blendExt.alphaBlendEnabled          = 1;
                blendExt.srcColorBlendFactor        = m->srcColorBlendFactor;
                blendExt.dstColorBlendFactor        = m->dstColorBlendFactor;
                blendExt.colorBlendOp               = 0;   // VK_BLEND_OP_ADD
                // DX9-fixed-function texture stage state. dxvk-remix maps these
                // to its internal RtTextureArgSource / DxvkRtTextureOperation
                // enums in rtx_remix_api.cpp:914-921. Zero-initializing them
                // produces (None / Disable) which silences the texture sampler
                // entirely (renders untextured/black). Use the same defaults
                // dxvk-remix bakes into RtSurface (rtx_materials.h:341-347):
                // "modulate texture by current, alpha = SelectArg1(texture)" --
                // i.e. plain textured passthrough that respects the diffuse
                // texture's alpha channel for the alpha-blend equation.
                blendExt.textureColorArg1Source     = 1;   // RtTextureArgSource::Texture
                blendExt.textureColorArg2Source     = 0;   // RtTextureArgSource::None
                blendExt.textureColorOperation      = 3;   // DxvkRtTextureOperation::Modulate
                blendExt.textureAlphaArg1Source     = 1;   // RtTextureArgSource::Texture
                blendExt.textureAlphaArg2Source     = 0;   // RtTextureArgSource::None
                blendExt.textureAlphaOperation      = 1;   // DxvkRtTextureOperation::SelectArg1
                blendExt.tFactor                    = 0xFFFFFFFFu;
                blendExt.isTextureFactorBlend       = 0;
                blendExt.srcAlphaBlendFactor        = m->srcColorBlendFactor;
                blendExt.dstAlphaBlendFactor        = m->dstColorBlendFactor;
                blendExt.alphaBlendOp               = 0;   // VK_BLEND_OP_ADD
                blendExt.writeMask                  = 0xF; // RGBA
                blendExt.isVertexColorBakedLighting = 0;
            }

            if (bucket.members.size() == 1) {
                // Simple path: base transform carries the world placement.
                const DrawableInstance* member = bucket.members[0];
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        instance.transform.matrix[r][c] = member->worldTransform[r][c];
                    }
                }
                // pNext: blendExt directly, or nullptr if not needed.
                instance.pNext = needBlendExt ? (void*)&blendExt : nullptr;
            } else {
                // Batched path: identity base, per-instance transforms in pNext.
                ++batchedBucketCount;
                bucket.transforms.reserve(bucket.members.size());
                for (const DrawableInstance* member : bucket.members) {
                    remixapi_Transform xform = {};
                    for (int r = 0; r < 3; ++r) {
                        for (int c = 0; c < 4; ++c) {
                            xform.matrix[r][c] = member->worldTransform[r][c];
                        }
                    }
                    bucket.transforms.push_back(xform);
                }
                instance.transform.matrix[0][0] = 1.0f;
                instance.transform.matrix[1][1] = 1.0f;
                instance.transform.matrix[2][2] = 1.0f;

                gpuExt.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT;
                // pNext chain on gpuExt: blendExt -> nullptr if needed.
                gpuExt.pNext = needBlendExt ? (void*)&blendExt : nullptr;
                gpuExt.instanceTransforms_values = bucket.transforms.data();
                gpuExt.instanceTransforms_count  = (uint32_t)bucket.transforms.size();
                instance.pNext = &gpuExt;
            }

            // Gate hasAnyMeshes and lastDrawnFrame stamps on DrawInstance success.
            // SEH-guarded: a stale Remix mesh handle can fault inside DrawInstance.
            // C++-guarded: a thrown exception (e.g. std::bad_alloc on VRAM
            // pressure, or a dxvk-remix-internal type) is logged via e.what()
            // by the inner CallDrawInstanceCxxGuarded helper.
            // Pre-set drawErr to GENERAL_FAILURE so the success-gated stamping
            // below falls through cleanly if either wrapper catches.
            remixapi_ErrorCode drawErr = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
            unsigned long drawExcCode = 0;
            int guardRc = CallDrawInstanceGuarded(api, &instance, &drawErr, &drawExcCode);
            if (guardRc == 1) {
                _MESSAGE("FO4RemixPlugin: [OnFrame] SEH CRASH CAUGHT in DrawInstance "
                         "meshHandle=%p batchSize=%zu exception=0x%08lX -- nulling member meshHandles to skip permanently",
                         (void*)meshHandle, bucket.members.size(), drawExcCode);
                for (DrawableInstance* member : bucket.members) member->meshHandle = nullptr;
                continue;
            }
            if (guardRc == 2) {
                // C++ exception already logged by the inner helper with what().
                // Null member meshHandles to skip permanently; LRU sweep will clean up.
                for (DrawableInstance* member : bucket.members) member->meshHandle = nullptr;
                continue;
            }
            if (drawErr == REMIXAPI_ERROR_CODE_SUCCESS) {
                hasAnyMeshes = true;
                for (DrawableInstance* member : bucket.members) {
                    member->lastDrawnFrame = currentFrame;
                    if (member->materialHash != 0) {
                        auto matIt = g_materialCache.find(member->materialHash);
                        if (matIt != g_materialCache.end()) {
                            matIt->second.lastDrawnFrame = currentFrame;
                        }
                    }
                    for (uint64_t texHash : member->textureHashes) {
                        auto texIt = g_textureHandles.find(texHash);
                        if (texIt != g_textureHandles.end()) {
                            texIt->second.lastDrawnFrame = currentFrame;
                        }
                    }
                }
            }
        }
    }

    // Periodic status log (every ~5 seconds)
    static uint32_t s_frameCounter = 0;
    s_frameCounter++;
    if (s_frameCounter % 300 == 0) {
        _MESSAGE("FO4RemixPlugin: OnFrame status - drawables=%zu buckets=%zu batched=%zu skippedInactive=%zu "
                 "skippedChunkPlayerInside=%zu "
                 "active=%u isLod=%u fadedIn=%u notVisible=%u lodFadingOut=%u forcedFadeOut=%u "
                 "player=(%.0f,%.0f,%.0f)",
                 drawableCount, bucketCount, batchedBucketCount, skippedInactive,
                 skippedChunkPlayerInside,
                 activeStats.total, activeStats.isLod, activeStats.fadedIn,
                 activeStats.notVisible, activeStats.lodFadingOut, activeStats.forcedFadeOut,
                 cam.playerWorldPos[0], cam.playerWorldPos[1], cam.playerWorldPos[2]);
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

    // ------------------------------------------------------------------
    // LRU sweeps. Run every cullingTextureLRUSweepPeriod frames.
    //
    //   (1) Material sweep -- the LEVER: materials hold Rc<DxvkImageView>
    //       refs to textures, so cascade-evicting stale materials is what
    //       actually drops texture VRAM for shared sets. Gated by budget;
    //       below budget, no eviction (TTL-only mode skips this gate).
    //
    //   (2) Texture sweep -- the BACKSTOP: catches textures whose cache
    //       entry survives their material. Cheap; runs on the same cadence.
    // ------------------------------------------------------------------
    static uint32_t s_texLRUSweepCounter = 0;
    if (g_config.cullingTextureLRUSweepPeriod > 0) {
        ++s_texLRUSweepCounter;
        if (s_texLRUSweepCounter >= g_config.cullingTextureLRUSweepPeriod) {
            s_texLRUSweepCounter = 0;
            const uint64_t currentFrameIndex = Diagnostics::CurrentFrameIndex();

            uint64_t currentMaterialTexBytes = 0;
            VramStats vramStats{};
            if (RemixRenderer::GetVramStats(&vramStats)) {
                currentMaterialTexBytes = vramStats.usedMaterialTextureBytes;
            }
            const uint64_t budgetBytes = static_cast<uint64_t>(g_config.cullingTextureBudgetMiB) << 20;

            if (g_config.cullingMaterialLRUGraceFrames > 0) {
                auto matResult = RemixRenderer::SweepStaleMaterials(
                    currentFrameIndex,
                    g_config.cullingMaterialLRUGraceFrames,
                    budgetBytes,
                    currentMaterialTexBytes);
                _MESSAGE("FO4RemixPlugin: [LRU] Material sweep: %u/%u stale, %u cells evicted",
                         matResult.staleMaterialCount,
                         matResult.materialCacheCount,
                         matResult.cellsEvicted);
            }

            // Re-query VRAM after the material sweep so the texture sweep's
            // budget gate sees the freed bytes.
            if (g_config.cullingTextureBudgetMiB > 0) {
                VramStats vramStatsAfter{};
                if (RemixRenderer::GetVramStats(&vramStatsAfter)) {
                    currentMaterialTexBytes = vramStatsAfter.usedMaterialTextureBytes;
                }
            }

            if (g_config.cullingTextureLRUGraceFrames > 0) {
                auto texResult = RemixRenderer::SweepStaleTextures(
                    currentFrameIndex,
                    g_config.cullingTextureLRUGraceFrames,
                    budgetBytes,
                    currentMaterialTexBytes);
                _MESSAGE("FO4RemixPlugin: [LRU] Texture sweep: %u/%u stale, %u cells evicted, %u budget, %u orphans",
                         texResult.staleTextureCount,
                         texResult.textureHandleCount,
                         texResult.cellsEvicted,
                         texResult.budgetEvictions,
                         texResult.orphanTexturesDestroyed);
            }
        }
    }

    // Present
    remixapi_PresentInfo presentInfo = {};
    presentInfo.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
    presentInfo.hwndOverride = nullptr;
    api->Present(&presentInfo);
}

void RemixRenderer::Shutdown() {
    std::lock_guard<std::mutex> lock(g_renderStateMutex);

    remixapi_Interface* api = RemixAPI::GetInterface();

    // Drop drawable entries first; their meshHandle members alias g_meshCache,
    // so we don't DestroyMesh here -- the cache loop below does that once per
    // unique handle.
    g_drawables.clear();

    // Destroy all cached meshes, materials, and textures.
    if (api) {
        for (auto& [key, mref] : g_meshCache) {
            if (mref.handle) api->DestroyMesh(mref.handle);
        }
        g_meshCache.clear();

        for (auto& [hash, matRef] : g_materialCache) {
            if (matRef.handle) api->DestroyMaterial(matRef.handle);
        }
        g_materialCache.clear();

        for (auto& [hash, texRef] : g_textureHandles) {
            if (texRef.handle) api->DestroyTexture(texRef.handle);
        }
        g_textureHandles.clear();
    }

    if (api && g_fallbackMesh) {
        api->DestroyMesh(g_fallbackMesh);
        g_fallbackMesh = nullptr;
    }
}

bool RemixRenderer::SetConfigVariable(const char* key, const char* value) {
    if (!key || !value) return false;
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api || !api->SetConfigVariable) return false;

    std::lock_guard<std::recursive_mutex> lock(g_remixApiMutex);
    return api->SetConfigVariable(key, value) == REMIXAPI_ERROR_CODE_SUCCESS;
}
