#include "remix_renderer.h"
#include "config.h"
#include "crash_diag.h"
#include "remix_api.h"
#include "fo4_diagnostics.h"
#include "semantic_capture.h"
#include "skinned_meshes.h"
#include "resolvers/lighting_static.h"  // Trace::SetStep + Trace::Step constants

#include "remix/remix_c.h"

#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <d3d11.h>
#include <excpt.h>  // EXCEPTION_EXECUTE_HANDLER for SEH wrappers below
#include <DbgHelp.h>  // MiniDumpWriteDump (guard-filter faulting-context dumps)
#pragma comment(lib, "dbghelp.lib")

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

// Remix API handles in this fork are the caller-provided 64-bit hash value,
// and CreateMesh registrations are immutable while that handle is live.  The
// material therefore has to participate in the API-visible hash just as it
// participates in MeshCacheKey: otherwise two material variants of identical
// geometry alias one runtime mesh, which remains bound to whichever material
// registered first.  FNV the material hash into the geometry hash to keep the
// result stable across runs while giving every cache key its own handle.
static uint64_t RuntimeMeshHashOf(const MeshCacheKey& key) {
    uint64_t h = HashBytes(&key.materialHash, sizeof(key.materialHash), key.contentHash);
    // A null hash is rejected as an invalid Remix handle.  This branch is
    // astronomically unlikely, but keep the API contract deterministic.
    return h != 0 ? h : 0xD6E8FEB86659FD93ULL;
}

// ---------------------------------------------------------------------------
// First-N-catches-per-callsite logger for C++ exceptions out of dxvk-remix
// API calls. We saw 3277 caught C++ exceptions out of SubmitDrawable in the
// last session; logging every one would balloon the log to MB. First 16 per
// call site is enough to spot patterns and learn what's actually being thrown.
// ---------------------------------------------------------------------------
static std::atomic<int> g_cxxLogCount_SubmitDrawable{0};
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

static LONG WriteGuardDumpFilter(EXCEPTION_POINTERS* xp);  // defined below

static int CallDrawInstanceGuarded(remixapi_Interface* api,
                                   const remixapi_InstanceInfo* instance,
                                   remixapi_ErrorCode* outErr,
                                   unsigned long* outExceptionCode) {
    __try {
        return CallDrawInstanceCxxGuarded(api, instance, outErr);
    } __except (WriteGuardDumpFilter(GetExceptionInformation())) {
        *outExceptionCode = GetExceptionCode();
        return 1;
    }
}

// ---------------------------------------------------------------------------
// Generic guard for every remixapi call on the frame path (2026-07-09
// Boston-Airport terminate fix). dxvk-remix (/MD) throws std::out_of_range
// from internal resource-map .at() lookups when a stale/destroyed handle
// reaches the API; the July-8 crash dumps (three identical) show that
// exception propagating out of an UNGUARDED OnFrame call site, crossing into
// this /MT module uncaught, and killing the process via terminate ->
// __fastfail(7). With the guard, a bad handle costs one skipped call plus a
// capped log line. Same inner/outer split as CallDrawInstanceGuarded (SEH
// cannot share a frame with unwindable objects, C2712); the callable is
// passed by reference so the __try frame owns nothing unwindable.
// Returns 0 = clean, 1 = SEH caught, 2 = C++ exception caught.
static std::atomic<int> g_remixGuardLogCount{0};
static constexpr int kRemixGuardLogCap = 32;

// ---------------------------------------------------------------------------
// Minidump at the SEH filter (2026-07-12). The CreateMesh AV that kills
// sessions happens INSIDE dxvk-remix (d3d9.dll) and leaves no trace: dxvk's
// log is clean, and WER records nothing because the process dies moments
// later on a DIFFERENT thread (the runtime's internals are left half-mutated
// by the unwound-through fault). The only place the true faulting stack
// exists is right here, in the filter, with the original context -- so write
// the dump now. Capped at 2 per session; C++ exceptions (0xE06D7363) are
// excluded (they carry no faulting context worth a dump and would burn the
// budget on out_of_range noise).
// ---------------------------------------------------------------------------
static std::atomic<int> g_guardDumpCount{0};

static LONG WriteGuardDumpFilter(EXCEPTION_POINTERS* xp) {
    if (!xp || !xp->ExceptionRecord ||
        xp->ExceptionRecord->ExceptionCode == 0xE06D7363UL) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    const int n = g_guardDumpCount.fetch_add(1, std::memory_order_relaxed);
    if (n >= 2) return EXCEPTION_EXECUTE_HANDLER;

    // Log the faulting site FIRST -- module-relative, so the crash location
    // is recoverable from the log alone. Field lesson (2026-07-12 second
    // crash): the first dump attempt died mid-MiniDumpWriteDump (0-byte
    // file, no log line) because the heavyweight dump type gave the other
    // poisoned runtime threads ~seconds to kill the process. The log write
    // and a stacks-only dump fit inside the window.
    void* addr = xp->ExceptionRecord->ExceptionAddress;
    char modName[MAX_PATH] = "?";
    uintptr_t modOff = 0;
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) && mod) {
        GetModuleFileNameA(mod, modName, sizeof(modName));
        modOff = (uintptr_t)addr - (uintptr_t)mod;
    }
    _MESSAGE("FO4RemixPlugin: [RemixGuard] SEH fault code=0x%08lX addr=%p "
             "module=%s +0x%llX -- writing dump",
             xp->ExceptionRecord->ExceptionCode, addr, modName,
             (unsigned long long)modOff);

    char dir[MAX_PATH] = {};
    if (!GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir))) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    char path[MAX_PATH];
    sprintf_s(path, "%s\\CrashDumps\\FO4RemixGuard_%lu_%d.dmp",
              dir, GetCurrentProcessId(), n);

    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei = {};
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = xp;
        mei.ClientPointers    = FALSE;
        // Stacks + modules only: a few MB written in tens of ms. The fat
        // IndirectlyReferencedMemory dump never completed (above).
        const BOOL ok = MiniDumpWriteDump(
            GetCurrentProcess(), GetCurrentProcessId(), file,
            (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo |
                            MiniDumpWithUnloadedModules),
            &mei, nullptr, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
        _MESSAGE("FO4RemixPlugin: [RemixGuard] dump %s: %s",
                 ok ? "written" : "FAILED", path);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

template <typename F>
static int RemixCallCxxGuarded(const char* site, F& fn) {
    try {
        fn();
        return 0;
    } catch (const std::exception& e) {
        int n = g_remixGuardLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < kRemixGuardLogCap) {
            _MESSAGE("FO4RemixPlugin: [RemixGuard] %s C++ exception #%d what=%s",
                     site, n, e.what());
        }
        CrashDiag::LogLastCxxThrow(site);
        return 2;
    } catch (...) {
        int n = g_remixGuardLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < kRemixGuardLogCap) {
            _MESSAGE("FO4RemixPlugin: [RemixGuard] %s unknown C++ exception #%d", site, n);
        }
        CrashDiag::LogLastCxxThrow(site);
        return 2;
    }
}

// Diagnostic dump with the CURRENT context (no exception pointers): all
// thread stacks as they are right now. Used by the std::terminate handler --
// abort() fast-fails through THIS module's static CRT, so our handler is the
// one that runs, and the dump names the thread whose uncaught exception is
// killing the process (the WER event only says "abort, this DLL").
void RemixRenderer::WriteDiagDump(const char* tag) {
    char dir[MAX_PATH] = {};
    if (!GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir))) return;
    char path[MAX_PATH];
    // Nothing else guarantees this folder exists (WER only creates it if
    // LocalDumps ran); without it every CreateFileA below fails silently
    // on a fresh install. Idempotent.
    sprintf_s(path, "%s\\CrashDumps", dir);
    CreateDirectoryA(path, nullptr);
    sprintf_s(path, "%s\\CrashDumps\\FO4Remix_%s_%lu.dmp",
              dir, tag, GetCurrentProcessId());
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const BOOL ok = MiniDumpWriteDump(
        GetCurrentProcess(), GetCurrentProcessId(), file,
        (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo |
                        MiniDumpWithUnloadedModules),
        nullptr, nullptr, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    _MESSAGE("FO4RemixPlugin: [%s] dump %s: %s (thread=%lu)",
             tag, ok ? "written" : "FAILED", path, GetCurrentThreadId());
}

template <typename F>
static int RemixCallGuarded(const char* site, F&& fn) {
    __try {
        return RemixCallCxxGuarded(site, fn);
    } __except (WriteGuardDumpFilter(GetExceptionInformation())) {
        int n = g_remixGuardLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < kRemixGuardLogCap) {
            _MESSAGE("FO4RemixPlugin: [RemixGuard] %s SEH exception #%d code=0x%08lX",
                     site, n, GetExceptionCode());
        }
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

        // Two-sided tag (2026-07-02). Copied from ExtractedMesh (shader-flag
        // bit 36) at submit time. OnFrame sets instance.doubleSided from this
        // (OR water/decal) instead of the old hardcoded doubleSided=1, so ray
        // traversal backface-culls ordinary opaque geometry. Folded into the
        // material cache key so bucket members agree.
        bool                         isTwoSided           = false;

        // Skinned drawable (2026-07-08). worldTransform holds the bare
        // Beth->Remix mirror P (bones produce Beth WORLD coordinates); the
        // livePoses per-frame pose update is SKIPPED for skinned instances
        // (bone updates carry all motion -- the shape's own transform must
        // not stomp P). boneTransforms is refreshed each frame from the
        // bone queue (SkinnedMeshes::UpdateAndQueue on the game thread);
        // OnFrame chains remixapi_InstanceInfoBoneTransformsEXT and SKIPS
        // drawing until the first bone set arrives (bind-pose verts are
        // model-space -- drawn boneless they'd T-pose at the world origin).
        bool                            isSkinned = false;
        std::vector<remixapi_Transform> boneTransforms;
    };

    std::unordered_map<uint64_t, DrawableInstance> g_drawables;

    // Mesh-handle cache keyed by (contentHash, materialHash). Refcounted; a
    // SubmitDrawable that finds a matching key reuses the existing handle and
    // bumps refCount, ReleaseDrawable drops it, on zero we erase + park the
    // handle in g_pendingDestroys for the Remix thread to destroy.
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

    // Pending config writes queued by game-thread callers (QueueConfigVariable)
    // and drained at the top of OnFrame on the Remix thread. Last write per key
    // wins. Guarded by its own lightweight mutex -- held only for the map
    // operation on either side, never across a Remix API call, so the game
    // thread can never block behind OnFrame's frame-long g_remixApiMutex hold.
    std::mutex g_configQueueMutex;
    std::unordered_map<std::string, std::string> g_pendingConfigVars;
    // Per-key failure dedup for the drain site: first failure logs, then silent.
    std::unordered_set<std::string> g_configFailedKeys;

    // Placed-light sync (revived 2026-07-07). The game thread queues a full
    // snapshot of the loaded cells' placed lights (QueueLights, same
    // never-touch-the-API-mutex contract as QueueConfigVariable); OnFrame
    // drains it on the Remix thread, diffs by hash against the live handle
    // map, and re-submits every live handle each frame (Remix lights are
    // per-frame draws like instances).
    std::mutex g_lightQueueMutex;
    bool g_lightSnapshotPending = false;
    std::vector<ExtractedLight> g_lightSnapshot;
    std::unordered_map<uint64_t, remixapi_LightHandle> g_lights;  // Remix thread only

    // Skinned bone sync (2026-07-08). The game thread queues composed
    // bind->world matrix sets per skinned drawable (QueueBoneTransforms from
    // SkinnedMeshes::UpdateAndQueue, once per Tick; never touches the API
    // mutex); OnFrame drains into DrawableInstance::boneTransforms on the
    // Remix thread and chains the bones ext on each skinned draw.
    std::mutex g_boneQueueMutex;
    std::unordered_map<uint64_t, std::vector<remixapi_Transform>> g_boneQueue;

    // Deferred handle destruction (2026-07-10). Game-thread release paths
    // (ReleaseDrawable / DecrementMeshCacheRef, driven by the Tick TTL sweep,
    // the PreLoadGame release wave, and merge upgrades) must never call
    // DestroyMesh/DestroyMaterial/DestroyTexture inline: the runtime
    // serializes each API call internally, but a destroy that lands between
    // OnFrame's DrawInstance records and the Present that consumes them
    // erases a handle the in-flight frame still references -- the runtime's
    // Present-side .at() then throws std::out_of_range on the Remix thread
    // (the 0xc0000409 CTD that 3e763bd's RemixCallGuarded only papers over).
    // Instead the handle is parked here (its cache entry is erased
    // immediately, so plugin bookkeeping is unchanged) and OnFrame destroys
    // it at the top of the NEXT frame -- on the Remix thread, after the
    // previous Present returned and before any new draw is recorded -- so a
    // destroy can never overlap a frame in flight.
    // Guarded by g_renderStateMutex (every producer already holds it).
    // g_hasPendingDestroys lets OnFrame skip the mutex in the common empty
    // steady state -- the lock is exactly the one a game-thread release
    // wave holds throughout a cell unload, so an unconditional per-frame
    // acquisition would block the Remix thread during teardown storms for
    // zero pending work.
    struct PendingDestroys {
        std::vector<remixapi_MeshHandle>     meshes;
        std::vector<remixapi_MaterialHandle> materials;
        std::vector<remixapi_TextureHandle>  textures;
    };
    PendingDestroys g_pendingDestroys;
    std::atomic<bool> g_hasPendingDestroys{false};
    // Parked-handle count mirror (updated under g_renderStateMutex at every
    // park/cancel/drain; atomic so the drain gate and the [VRAM] log can
    // read it without the lock).
    std::atomic<size_t> g_pendingDestroyCount{0};
    // Load-screen drain request ([Performance] DeferHandleDestroyToLoad):
    // set from the PreLoadGame message, consumed by the next OnFrame.
    std::atomic<bool> g_destroyDrainRequested{false};
}

void RemixRenderer::RequestDestroyDrain() {
    g_destroyDrainRequested.store(true, std::memory_order_release);
}

// Park a handle for deferred destruction. Caller holds g_renderStateMutex.
template <typename H>
static void ParkForDestroy(std::vector<H>& parked, H h) {
    parked.push_back(h);
    g_pendingDestroyCount.fetch_add(1, std::memory_order_relaxed);
    g_hasPendingDestroys.store(true, std::memory_order_release);
}

// Pull a handle back out of the park list. Handles are HASH-VALUED in this
// fork (rtx_remix_api.cpp reinterpret_casts info->hash into the handle, and
// the runtime IGNORES repeated registrations of a live handle), so re-
// creating content identical to something released-but-not-yet-drained
// yields the SAME handle value -- if the parked destroy then ran, it would
// unregister the live re-created resource (flicker/vanish on exactly the
// churn paths the deferral serves: TTL eviction, merge/texture upgrades).
// Called from SubmitDrawable's create sites under g_renderStateMutex.
template <typename H>
static void CancelParkedHandle(std::vector<H>& parked, H h) {
    const size_t before = parked.size();
    parked.erase(std::remove(parked.begin(), parked.end(), h), parked.end());
    const size_t removed = before - parked.size();
    if (removed) {
        g_pendingDestroyCount.fetch_sub(removed, std::memory_order_relaxed);
    }
}

// Destroy a batch of parked handles on the Remix thread. Shared by the
// OnFrame top-of-frame drain and Shutdown so both stay guarded -- a parked
// handle is precisely the class most likely to throw out of the runtime
// (see the 0xE06D7363 note above DecrementTextureRefs).
static void DestroyParkedHandles(remixapi_Interface* api, PendingDestroys& doomed) {
    for (remixapi_MeshHandle h : doomed.meshes) {
        if (h) RemixCallGuarded("DestroyMesh(deferred)", [&] { api->DestroyMesh(h); });
    }
    for (remixapi_MaterialHandle h : doomed.materials) {
        if (h) RemixCallGuarded("DestroyMaterial(deferred)", [&] { api->DestroyMaterial(h); });
    }
    for (remixapi_TextureHandle h : doomed.textures) {
        if (h) RemixCallGuarded("DestroyTexture(deferred)", [&] { api->DestroyTexture(h); });
    }
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
    remixapi_ErrorCode vramErr = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    RemixCallGuarded("GetVramStats", [&] { vramErr = api->GetVramStats(&s); });
    if (vramErr != REMIXAPI_ERROR_CODE_SUCCESS) return false;
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
                // Sweeps run after this frame's DrawInstance calls.  Destroying
                // inline here lets the runtime free a bindless slot before
                // Present consumes preserved instances, and its generation bump
                // is then swallowed by onFrameEnd.  Park it for the guarded
                // top-of-frame drain, matching ReleaseDrawable.
                ParkForDestroy(g_pendingDestroys.materials, it->second.handle);
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
// Then defensive orphan-handle parking (refCount == 0 guard); the next
// top-of-frame destroy drain performs the API release safely.
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
                if (texIt->second.handle) {
                    // Do not release texture-table slots after draws have been
                    // recorded.  The top-of-frame drain runs before any new
                    // DrawInstance, so the runtime generation mismatch forces a
                    // safe one-frame retranslation before preserved state can
                    // sample the freed/recycled slot.
                    ParkForDestroy(g_pendingDestroys.textures, texIt->second.handle);
                }
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

// Drop a refCount on a g_meshCache entry. On zero we erase the entry and park
// the handle for deferred destruction on the Remix thread (see
// g_pendingDestroys) -- this runs on the game thread, where an inline
// DestroyMesh can invalidate a handle the frame in flight still references.
// Caller must hold g_renderStateMutex.
static void DecrementMeshCacheRef(const MeshCacheKey& key) {
    auto it = g_meshCache.find(key);
    if (it == g_meshCache.end()) return;
    if (it->second.refCount > 0) --it->second.refCount;
    if (it->second.refCount != 0) return;

    if (it->second.handle) {
        // RuntimeMeshHashOf normally makes handles unique per full cache key.
        // Keep a defensive alias check for the vanishingly unlikely 64-bit hash
        // collision: destroying one collision peer must not unregister another.
        bool aliased = false;
        for (const auto& [k, m] : g_meshCache) {
            if (&m != &it->second && m.handle == it->second.handle) {
                aliased = true;
                break;
            }
        }
        if (!aliased) {
            ParkForDestroy(g_pendingDestroys.meshes, it->second.handle);
        }
    }
    g_meshCache.erase(it);
}

RemixRenderer::SubmitStatus RemixRenderer::SubmitDrawable(
        uint64_t hash,
        const ExtractedMesh& mesh,
        const TextureSupply& newTextures) {

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
    // Entries are shared_ptr views of the extraction cache's chains (see
    // TextureSupply); the only remaining pixel copy is CreateTexture's own.
    for (const auto& texPtr : newTextures) {
        const ExtractedTexture& tex = *texPtr;
        // Dupe guard: the extraction cache re-supplies pixels whenever the
        // Remix-side handle is missing, so a drawable that references the
        // same texture in two slots (diffuse reused as glow map) can list
        // the hash twice in one submit. Processing it twice would bump the
        // refcount twice against a single textureHashes entry -- a refcount
        // the drawable's release could never return.
        if (inst.textureHashes.count(tex.hash)) {
            continue;
        }
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

        // RemixCallGuarded is LOAD-BEARING on every create in this function,
        // not just log hygiene: SubmitDrawable holds g_renderStateMutex, and
        // an AV inside the runtime that escapes to the resolver's outer SEH
        // frame skips this scope's lock_guard destructor (/EHsc runs no
        // destructors on hardware-exception unwinds) -- the mutex is then
        // stranded owned-by-game-thread, every later lock throws
        // system_error("resource deadlock would occur"), and the Remix
        // thread blocks forever (the 2026-07-11 wedge; same signature as
        // 07-10's "9000 exception storm"). Catching AT the call means no
        // unwind ever crosses the lock scope; the create becomes a normal
        // failure. dxvk-remix logged nothing at the observed CreateMesh AV,
        // so [RemixGuard] site lines are also the only breadcrumb for
        // root-causing the underlying runtime fault.
        remixapi_TextureHandle texHandle = nullptr;
        Resolvers::Trace::SetStep(Resolvers::Trace::kSubmit_BeforeTextureCreate);
        remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        const int texGuard = RemixCallGuarded("CreateTexture(submit)", [&] {
            status = api->CreateTexture(&texInfo, &texHandle);
        });
        Resolvers::Trace::SetStep(Resolvers::Trace::kSubmit_AfterTextureCreate);
        if (texGuard != 0 || status != REMIXAPI_ERROR_CODE_SUCCESS || !texHandle) {
            _MESSAGE("FO4RemixPlugin: [SubmitDrawable] Failed to upload texture 0x%llX "
                     "(guard %d, error %d)",
                     (unsigned long long)tex.hash, texGuard, (int)status);
            continue;
        }

        // Hash-valued handle: if this same texture was released but its
        // deferred destroy hasn't drained yet, that parked destroy would
        // unregister the handle we just (re-)created -- pull it back out.
        CancelParkedHandle(g_pendingDestroys.textures, texHandle);
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
        //
        // Capped log: this was the ONLY silent kFailed path, and it hid the
        // 2026-07-02 post-load poisoning (extraction cache hit returned a
        // hash with no pixels after the PreLoadGame release wave destroyed
        // the Remix-side handle -- thousands of drawables failing here with
        // zero log evidence).
        static std::atomic<int> s_diffuseMissLogs{0};
        const int n = s_diffuseMissLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 500) == 0) {
            _MESSAGE("FO4RemixPlugin: [SubmitDrawable] #%d diffuse 0x%llX has no "
                     "Remix handle and no pixel data was supplied -- kFailed "
                     "(drawable hash=0x%llX)",
                     n, (unsigned long long)mesh.diffuseTextureHash,
                     (unsigned long long)hash);
        }
        DecrementTextureRefs(inst.textureHashes);
        return SubmitStatus::kFailed;
    }

    const uint64_t normalH    = loaded(mesh.normalTextureHash)    ? mesh.normalTextureHash    : 0;
    const uint64_t roughnessH = loaded(mesh.roughnessTextureHash) ? mesh.roughnessTextureHash : 0;
    const uint64_t emissiveH  = loaded(mesh.emissiveTextureHash)  ? mesh.emissiveTextureHash  : 0;

    // ---- True texture refcounts (2026-07-02) ----
    // The upload loop above only tracks textures that arrived in newTextures
    // (fresh extraction or handle-recreate). Textures referenced via an
    // extraction-cache hit whose Remix handle already existed were invisible
    // to this drawable's refcount set -- only the FIRST submitter of a shared
    // texture held a reference and stamped lastDrawnFrame in OnFrame. Any
    // path that zeroed that lone refcount while later submitters still used
    // the texture (failed-submit backout followed by a successful hash-only
    // retry; sole holder released at TTL / reload) let ReleaseDrawable or the
    // LRU sweep destroy a texture still bound to visible materials: objects
    // rendered correctly and then turned black ~TextureLRUGraceFrames (~10s)
    // later. Reference every texture this drawable's material will actually
    // use, so refcounts mean what they say.
    for (uint64_t refH : { mesh.diffuseTextureHash, normalH, roughnessH, emissiveH }) {
        if (refH == 0 || inst.textureHashes.count(refH)) {
            continue;
        }
        auto refIt = g_textureHandles.find(refH);
        if (refIt != g_textureHandles.end()) {
            refIt->second.refCount++;
            inst.textureHashes.insert(refH);
        }
    }

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
        // Fold the metal-conversion constants so metal and non-metal
        // variants sharing the same texture set never share a material
        // handle (metallic/roughnessConstant live on the handle).
        uint32_t mci, rci;
        memcpy(&mci, &mesh.metallicConstant, 4);
        memcpy(&rci, &mesh.roughnessConstantOverride, 4);
        h ^= (uint64_t)mci * 0xA24BAED4963EE407ULL;
        h ^= (uint64_t)rci * 0xE7037ED1A0B428DBULL;
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
        // Fold the two-sided tag so single- and double-sided variants of the
        // same texture set never share a material handle -- OnFrame reads
        // doubleSided from bucket.members[0] and relies on bucket homogeneity
        // (buckets share mesh handles, which bake in the material).
        h ^= (uint64_t)(mesh.isTwoSided ? 1 : 0) * 0x9E3779B97F4A7C15ULL;
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
                // Metal conversion: the resolver derives roughness from the
                // material's scalar fSmoothness and metallic from the same
                // scalar (see lighting_static.cpp). Non-metals keep the
                // legacy defaults (metallic 0; 0.8 rough, 0.5 if a roughness
                // texture is ever supplied again).
                opaqueExt.roughnessConstant = mesh.roughnessConstantOverride >= 0.0f
                    ? mesh.roughnessConstantOverride
                    : (roughnessH ? 0.5f : 0.8f);
                opaqueExt.metallicConstant  = mesh.metallicConstant;
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
            remixapi_ErrorCode matStatus = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
            const int matGuard = RemixCallGuarded("CreateMaterial(submit)", [&] {
                matStatus = api->CreateMaterial(&matInfo, &newHandle);
            });
            Resolvers::Trace::SetStep(Resolvers::Trace::kSubmit_AfterMaterialCreate);
            if (matGuard != 0 || matStatus != REMIXAPI_ERROR_CODE_SUCCESS || !newHandle) {
                _MESSAGE("FO4RemixPlugin: [SubmitDrawable] Failed to create material 0x%llX "
                         "(guard %d, error %d)",
                         (unsigned long long)matHash, matGuard, (int)matStatus);
                DecrementTextureRefs(inst.textureHashes);
                return SubmitStatus::kFailed;
            }

            // Hash-valued handle: cancel any not-yet-drained deferred
            // destroy of this same material (see CancelParkedHandle).
            CancelParkedHandle(g_pendingDestroys.materials, newHandle);
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
    // Skinned meshes never share a handle across drawables: the runtime
    // re-skins a mesh's geometry against its instance's bone set (boneHash-
    // keyed, rtx_scene_manager.cpp:351), so two actors on one handle would
    // fight over the same skinned BLAS. Key by drawable hash instead.
    meshKey.contentHash  = mesh.hasSkinning
        ? hash
        : (g_config.gpuInstancingEnabled ? ContentHashOf(mesh) : hash);
    meshKey.materialHash = matHash;

    remixapi_MeshHandle meshHandle = nullptr;
    auto cacheIt = g_meshCache.find(meshKey);
    if (cacheIt != g_meshCache.end()) {
        cacheIt->second.refCount++;
        meshHandle = cacheIt->second.handle;
    } else {
        // Cache miss -- create a new Remix mesh handle. The runtime stores the
        // surface material on this immutable, hash-valued handle, so hash the
        // complete (geometry, material) key rather than geometry alone. The
        // derived value is deterministic, so each full key keeps a stable
        // runtime identity instead of aliasing whichever variant arrived first.
        remixapi_MeshInfoSurfaceTriangles surface = {};
        surface.vertices_values = mesh.vertices.data();
        surface.vertices_count  = (uint32_t)mesh.vertices.size();
        surface.indices_values  = mesh.indices.empty() ? nullptr : mesh.indices.data();
        surface.indices_count   = (uint32_t)mesh.indices.size();
        surface.skinning_hasvalue = 0;
        if (mesh.hasSkinning &&
            mesh.blendWeights.size() == mesh.vertices.size() * 4 &&
            mesh.blendIndices.size() == mesh.vertices.size() * 4) {
            // 4-bone rigid layout (FO4 VB: 4x f16 weights + 4x u8 indices,
            // widened at parse). The runtime packs indices back to bytes and
            // GPU-skins in object space; per-frame bone sets arrive via the
            // InstanceInfoBoneTransformsEXT chain in OnFrame.
            surface.skinning_hasvalue = 1;
            surface.skinning_value.bonesPerVertex      = 4;
            surface.skinning_value.blendWeights_values = mesh.blendWeights.data();
            surface.skinning_value.blendWeights_count  = (uint32_t)mesh.blendWeights.size();
            surface.skinning_value.blendIndices_values = mesh.blendIndices.data();
            surface.skinning_value.blendIndices_count  = (uint32_t)mesh.blendIndices.size();
        }
        surface.material = matHandle;

        remixapi_MeshInfo meshInfo = {};
        meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
        meshInfo.hash = RuntimeMeshHashOf(meshKey);
        meshInfo.surfaces_values = &surface;
        meshInfo.surfaces_count  = 1;

        // CreateMeshBatched, NOT CreateMesh (2026-07-12, the four-crash
        // night's root fix). The synchronous CreateMesh EmitCs's onto the
        // device's CS chunk from THIS (game) thread under only the API
        // bridge's s_mutex, while the Remix thread's Present flushes that
        // same chunk under only the device lock -- two different locks, no
        // exclusion, and the chunk pointer is momentarily null mid-swap
        // inside the flush (dump-proven: AV in DxvkCsChunk::push,
        // dxvk_cs.h:171, this=null, caller rtx_remix_api.cpp:1142).
        // CreateMeshBatched copies the surfaces into runtime-owned storage
        // and queues them under s_mutex; the render thread materializes
        // pending creates at its next DrawInstance/Present, so the game
        // thread never touches the CS chunk at all. Handle semantics are
        // identical (caller hash cast). Fallback kept for older runtimes.
        Resolvers::Trace::SetStep(Resolvers::Trace::kSubmit_BeforeMeshCreate);
        remixapi_ErrorCode meshStatus = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        const bool haveBatched = api->CreateMeshBatched != nullptr;
        const int meshGuard = RemixCallGuarded(
            haveBatched ? "CreateMeshBatched(submit)" : "CreateMesh(submit)", [&] {
            meshStatus = haveBatched
                ? api->CreateMeshBatched(&meshInfo, &meshHandle)
                : api->CreateMesh(&meshInfo, &meshHandle);
        });
        Resolvers::Trace::SetStep(Resolvers::Trace::kSubmit_AfterMeshCreate);
        if (meshGuard != 0 || meshStatus != REMIXAPI_ERROR_CODE_SUCCESS || !meshHandle) {
            _MESSAGE("FO4RemixPlugin: [SubmitDrawable] Failed to create mesh content=0x%llX "
                     "mat=0x%llX (guard %d, error %d)",
                     (unsigned long long)meshKey.contentHash,
                     (unsigned long long)meshKey.materialHash,
                     meshGuard, (int)meshStatus);
            DecrementTextureRefs(inst.textureHashes);
            DecrementMaterialRef(matHash);
            return SubmitStatus::kFailed;
        }

        // Hash-valued handle: cancel any not-yet-drained deferred destroy
        // of this same content (see CancelParkedHandle).
        CancelParkedHandle(g_pendingDestroys.meshes, meshHandle);
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
    inst.isTwoSided           = mesh.isTwoSided;
    inst.isSkinned            = mesh.hasSkinning;

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
    // Drop bone tracking regardless of drawable presence (registration can
    // outlive a failed submit).
    SkinnedMeshes::OnDrawableReleased(hash);

    std::lock_guard<std::mutex> lock(g_renderStateMutex);

    auto it = g_drawables.find(hash);
    if (it == g_drawables.end()) return;

    DrawableInstance& inst = it->second;

    remixapi_Interface* api = RemixAPI::GetInterface();

    // Drop our refCount on the cached mesh handle. DestroyMesh only fires when
    // the last drawable using this (content, material) bucket releases.
    // Keyed on meshCacheKey, NOT the meshHandle alias: OnFrame's guard paths
    // null member meshHandles after a caught DrawInstance fault, and gating
    // this decrement on the alias meant the cache entry's refCount never hit
    // zero -- the runtime mesh (geometry + BLAS VRAM) leaked for the rest of
    // the session, one whole working set per exception storm. Every drawable
    // that reaches g_drawables was inserted with both fields set; a fault-
    // nulled alias still holds its cache ref. (A default key would simply
    // miss in g_meshCache.)
    DecrementMeshCacheRef(inst.meshCacheKey);
    inst.meshHandle = nullptr;

    // Decrement material refcount; on zero, erase the entry and park the
    // handle for deferred destruction (see g_pendingDestroys -- this is the
    // game thread; an inline DestroyMaterial can invalidate a handle the
    // frame in flight still references). This is INDEPENDENT of the texture
    // decrement below — material and texture lifecycles are tracked
    // separately so shared resources are freed correctly.
    if (inst.materialHash != 0) {
        auto matIt = g_materialCache.find(inst.materialHash);
        if (matIt != g_materialCache.end()) {
            if (matIt->second.refCount > 0) matIt->second.refCount--;
            if (matIt->second.refCount == 0) {
                if (matIt->second.handle) {
                    ParkForDestroy(g_pendingDestroys.materials, matIt->second.handle);
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
    for (uint64_t texHash : inst.textureHashes) {
        auto texIt = g_textureHandles.find(texHash);
        if (texIt != g_textureHandles.end()) {
            if (texIt->second.refCount > 0) texIt->second.refCount--;
            if (texIt->second.refCount == 0) {
                if (texIt->second.handle) {
                    ParkForDestroy(g_pendingDestroys.textures, texIt->second.handle);
                }
                g_textureHandles.erase(texIt);
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
    // Per-phase CPU timing, reported through the every-300-frame status log.
    // ~10 steady_clock reads per frame -- negligible. Static accumulators are
    // safe: OnFrame runs only on the Remix thread.
    using PerfClock = std::chrono::steady_clock;
    auto nsSince = [](PerfClock::time_point t0, PerfClock::time_point t1) {
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    };
    static uint64_t s_accLockWaitNs = 0, s_accSnapNs = 0, s_accBucketNs = 0,
                    s_accDrawNs = 0, s_accPresentNs = 0, s_accTotalNs = 0;
    static SemanticCapture::PerfCounters s_lastGameCounters = {};
    const PerfClock::time_point tEnter = PerfClock::now();

    // Serializes Remix API option-registry writes (game thread, via
    // SetConfigVariable) against draw submissions on this thread. Held for
    // the function's full duration. Lock order: g_remixApiMutex ->
    // g_drawableMutex (SnapshotActiveDrawables) -> g_renderStateMutex
    // (bucket build). Never acquire in reverse.
    std::lock_guard<std::recursive_mutex> lock(g_remixApiMutex);
    const PerfClock::time_point tLocked = PerfClock::now();
    remixapi_Interface* api = RemixAPI::GetInterface();
    if (!api) return;

    // Destroy handles parked by game-thread release paths. This is the one
    // point where a destroy provably cannot overlap a frame in flight: the
    // previous Present has returned (same thread), no draw of the new frame
    // has been recorded yet, and g_remixApiMutex is held.
    // g_renderStateMutex is held ACROSS the destroy calls, not just a swap:
    // handles are hash-valued, so a game-thread SubmitDrawable re-creating
    // identical content between a swap-out and the destroy would produce
    // the same handle value and the destroy would unregister the live
    // resource. Under the mutex, SubmitDrawable either runs first (its
    // CancelParkedHandle pulls the handle back out) or after the destroys
    // (its create re-registers cleanly). The runtime's Destroy* only queues
    // a CS command under its own lock -- no GPU wait -- so the hold is
    // short, and it matches the old inline-destroy locking exactly.
    //
    // Drained on a CADENCE, not every frame (2026-07-10): each DestroyTexture
    // bumps the runtime's texture-cache generation (the preserve-path fix for
    // the stale-albedo-slot corruption), which sends the NEXT frame's draws
    // down the dynamic path. A steady destroy trickle (TextureUpgradeOnApproach
    // while moving) would keep the ~93-95% preserve win suppressed every
    // frame; batching destroys to every kDestroyDrainPeriodFrames confines the
    // re-translation cost to one frame per period. Longer parking is free --
    // the handles are already erased from the plugin caches -- and it widens
    // the CancelParkedHandle rescue window (re-created content avoids its
    // destroy+re-upload entirely).
    // Drain policy (2026-07-12): with [Performance] DeferHandleDestroyToLoad
    // (default), parked handles are destroyed ONLY during load screens
    // (PreLoadGame requests a drain; the runtime is quiescent then) plus an
    // emergency valve, instead of the 30-frame cadence. Motivation: both
    // sessions that died on an AV inside api->CreateMesh (07-10, 07-11)
    // featured TexUpgrade churn with interleaved destroys -- a create-vs-
    // CS-side-destruction race inside the runtime is the live suspect, and
    // parking longer is free (handles are already erased from the plugin
    // caches; CancelParkedHandle rescues re-creates for as long as they stay
    // parked). VRAM held by parked handles is monitored via parked= on the
    // [VRAM] line. Set the key to 0 to restore the 30-frame cadence for A/B.
    constexpr uint64_t kDestroyDrainPeriodFrames = 30;
    constexpr size_t   kEmergencyDrainParked = 8192;
    static uint64_t s_lastDrainFrame = 0;
    const uint64_t drainNow = Diagnostics::CurrentFrameIndex();
    if (g_hasPendingDestroys.load(std::memory_order_acquire)) {
        bool wantDrain;
        if (g_config.deferHandleDestroyToLoad) {
            wantDrain = g_destroyDrainRequested.exchange(
                false, std::memory_order_acq_rel);
            if (!wantDrain &&
                g_pendingDestroyCount.load(std::memory_order_relaxed) >=
                    kEmergencyDrainParked) {
                _MESSAGE("FO4RemixPlugin: [DeferredDestroy] emergency drain: "
                         "%zu handles parked",
                         g_pendingDestroyCount.load(std::memory_order_relaxed));
                wantDrain = true;
            }
        } else {
            wantDrain = drainNow - s_lastDrainFrame >= kDestroyDrainPeriodFrames;
        }
        if (wantDrain) {
            g_hasPendingDestroys.store(false, std::memory_order_release);
            s_lastDrainFrame = drainNow;
            std::lock_guard<std::mutex> rsLock(g_renderStateMutex);
            DestroyParkedHandles(api, g_pendingDestroys);
            g_pendingDestroyCount.store(0, std::memory_order_relaxed);
            g_pendingDestroys.meshes.clear();
            g_pendingDestroys.materials.clear();
            g_pendingDestroys.textures.clear();
        }
    }

    // Apply config writes queued by game-thread callers (weather bridge et
    // al). Swap the map out under the queue mutex so the Remix API calls run
    // without it -- QueueConfigVariable on the game thread only ever waits
    // for the swap, never for the API.
    {
        std::unordered_map<std::string, std::string> pending;
        {
            std::lock_guard<std::mutex> qlock(g_configQueueMutex);
            pending.swap(g_pendingConfigVars);
        }
        for (const auto& kv : pending) {
            const std::string& key = kv.first;
            const std::string& value = kv.second;
            // Guarded like every other frame-path call into d3d9.dll: an
            // SEH fault in the runtime's option-registry write would skip
            // the thread-level C++ backstop entirely (/EHsc catch(...)
            // doesn't see hardware exceptions) and kill the process with no
            // [RemixGuard] breadcrumb.
            remixapi_ErrorCode cfgErr = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
            if (api->SetConfigVariable) {
                RemixCallGuarded("SetConfigVariable", [&] {
                    cfgErr = api->SetConfigVariable(key.c_str(), value.c_str());
                });
            }
            if (cfgErr != REMIXAPI_ERROR_CODE_SUCCESS) {
                std::lock_guard<std::mutex> qlock(g_configQueueMutex);
                if (g_configFailedKeys.insert(key).second) {
                    _MESSAGE("FO4RemixPlugin: [ConfigQueue] SetConfigVariable failed for "
                             "key '%s' = '%s' (key not registered in Remix fork?)",
                             key.c_str(), value.c_str());
                }
            }
        }
    }

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
    RemixCallGuarded("SetupCamera", [&] { api->SetupCamera(&camInfo); });

    bool hasAnyMeshes = false;

    // The full-map "engine-active" snapshot that used to live here is gone.
    // 2026-04-29 widened its window to TTL, making it equivalent to "every
    // cached drawable" (measured skippedInactive=0 in every session window),
    // yet it still walked the entire g_drawableMap under g_drawableMutex
    // every Remix frame -- 4-5ms/frame in dense scenes, and its long mutex
    // holds stalled the game thread's hook fires. Live poses for animated
    // statics (the one thing it actually delivered) now arrive through
    // DrainDirtyPoses: the fire hook queues a key only when the transform
    // actually changed, so this is O(animating) instead of O(map).
    // kActiveAgeFrames survives for the stats-only diagnostic snapshot taken
    // on log frames below.
    constexpr uint64_t kActiveAgeFrames = 18000;  // == kTTLFrames; effectively unbounded for play
    SemanticCapture::ActiveFlagStats activeStats;
    // Poses that changed since last frame (animated statics: doors, gates,
    // machinery). Applied to DrawableInstance::worldTransform below, which
    // persists them -- unchanged drawables keep their last-applied pose.
    // static + clear(): reuses node capacity instead of reallocating at 60Hz.
    // Safe: OnFrame runs only on the Remix thread.
    static std::unordered_map<uint64_t, std::array<float, 12>> livePoses;
    livePoses.clear();
    const PerfClock::time_point tSnap0 = PerfClock::now();
    SemanticCapture::DrainDirtyPoses(livePoses);

    // Drain queued skinned bone sets (game thread queues one composed set
    // per skinned drawable per Tick via SkinnedMeshes::UpdateAndQueue).
    // Applied to DrawableInstance::boneTransforms in the member loop below.
    static std::unordered_map<uint64_t, std::vector<remixapi_Transform>> freshBones;
    freshBones.clear();
    {
        std::lock_guard<std::mutex> boneLock(g_boneQueueMutex);
        freshBones.swap(g_boneQueue);
    }

    // Stale-chunk filter inputs. The engine fires GetRenderPasses every frame
    // for geometry that survives its culling and HIDES worldspace LOD chunks
    // when their cells attach at full detail -- so a chunk whose fire age
    // exceeds the threshold is one the engine stopped rendering, and drawing
    // it overlays the low-poly shell on the streamed-in buildings. Guard on
    // "fires advanced since last OnFrame": in pause/main menus the 3D scene
    // stops firing entirely and staleness is meaningless (without the guard,
    // unpausing would blink all distant LOD out of the Remix view).
    static std::unordered_map<uint64_t, uint64_t> lodChunkAges;
    lodChunkAges.clear();
    static uint64_t s_lastTotalFires = 0;
    const uint64_t totalFires =
        SemanticCapture::SnapshotLodChunkAges(Diagnostics::CurrentFrameIndex(), lodChunkAges);
    const bool sceneFiring = totalFires != s_lastTotalFires;
    s_lastTotalFires = totalFires;
    const bool staleChunkFilterActive = sceneFiring && g_config.cullingLodChunkStaleFrames > 0;

    // Engine-hidden skinned drawables (live app-culled bit mirrored by Tick):
    // hair suppressed by hats, hidden gore parts. Unlike the age-based chunk
    // filter this is a direct flag read, so no scene-firing guard is needed
    // (valid in pause menus too).
    static std::unordered_set<uint64_t> skinnedCulled;
    skinnedCulled.clear();
    SemanticCapture::SnapshotSkinnedCulled(skinnedCulled);

    const PerfClock::time_point tSnap1 = PerfClock::now();
    PerfClock::duration dBucket{}, dDraw{};

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
    size_t skippedChunkStale = 0;
    {
        std::lock_guard<std::mutex> lock(g_renderStateMutex);
        const PerfClock::time_point tBucket0 = PerfClock::now();
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

        // [HeadDiag] hold accounting (2026-07-08 missing-heads): a skinned
        // drawable that was submitted but whose bone set never queues sits
        // invisible in the empty-boneTransforms hold below forever
        // (hypothesis (c)). Counted per frame, logged rate-limited while
        // any draw is held. skinnedHidden counts engine-app-culled skips.
        uint32_t skinnedTotal = 0, skinnedHeldNoBones = 0, skinnedHidden = 0;

        for (auto& [drawHash, inst] : g_drawables) {
            if (!inst.meshHandle) continue;
            // (Engine-active filter removed 2026-07-02 -- with the window at
            // TTL it never skipped anything; skippedInactive stays in the
            // status log as a tombstone and always reads 0.)
            // Apply live world transform if the hook captured one this frame
            // (or recently). Animated statics evaluate their controllers
            // before GetRenderPasses fires, so the live transform reflects
            // the current pose. Drawables without a live transform fall back
            // to the baked transform from SubmitDrawable.
            auto poseIt = livePoses.find(drawHash);
            if (poseIt != livePoses.end() && !inst.isSkinned) {
                // Skinned instances keep their bare mirror-P base: bone
                // matrices carry all motion, and the shape's own transform
                // (what livePoses holds) must not stomp it.
                const auto& pose = poseIt->second;
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        inst.worldTransform[r][c] = pose[r * 4 + c];
                    }
                }
            }
            if (inst.isSkinned) {
                ++skinnedTotal;
                auto bIt = freshBones.find(drawHash);
                if (bIt != freshBones.end()) {
                    inst.boneTransforms = std::move(bIt->second);
                }
                // Engine hid this geometry (app-culled: hair under a hat,
                // gore parts) -- skip the draw but keep the handle; it
                // returns the moment the engine unhides it.
                if (skinnedCulled.count(drawHash)) { ++skinnedHidden; continue; }
                // No bone set yet: bind-pose verts are model-space and would
                // render T-posed at the world origin -- hold the draw until
                // the first game-thread bone update lands (next Tick).
                if (inst.boneTransforms.empty()) { ++skinnedHeldNoBones; continue; }
            }
            if (inst.isLODChunk) {
                // Stale-fire filter: the engine hid this chunk (its cells
                // attached at full detail) if it stopped firing -- see the
                // lodChunkAges block above. Re-appears within a frame or two
                // of the engine rendering it again.
                if (staleChunkFilterActive) {
                    auto ageIt = lodChunkAges.find(drawHash);
                    if (ageIt != lodChunkAges.end() &&
                        ageIt->second > g_config.cullingLodChunkStaleFrames) {
                        ++skippedChunkStale;
                        continue;
                    }
                }
                // Spatial filter: skip if player is inside the chunk's
                // coverage box. (Beth coords; chunk pivot is the chunk's
                // origin corner, extent is its side length.) Backstop for
                // the fire-age filter's first N frames after a swap.
                if (inst.chunkExtent > 0.0f) {
                    const float chunkMaxX = inst.chunkOriginX + inst.chunkExtent;
                    const float chunkMaxY = inst.chunkOriginY + inst.chunkExtent;
                    if (playerX >= inst.chunkOriginX && playerX <= chunkMaxX &&
                        playerY >= inst.chunkOriginY && playerY <= chunkMaxY) {
                        ++skippedChunkPlayerInside;
                        continue;
                    }
                }
            }
            buckets[inst.meshHandle].members.push_back(&inst);
        }
        bucketCount = buckets.size();
        if (skinnedHeldNoBones > 0 || skinnedHidden > 0) {
            // First occurrence logs immediately, then every ~300 frames,
            // capped for the session. OnFrame is single-threaded.
            static int      s_heldLogs = 0;
            static uint32_t s_sinceLog = 300;
            if (s_heldLogs < 40 && ++s_sinceLog > 300) {
                s_sinceLog = 0;
                ++s_heldLogs;
                _MESSAGE("FO4RemixPlugin: [HeadDiag] OnFrame skinned=%u "
                         "heldForEmptyBones=%u engineHidden=%u",
                         skinnedTotal, skinnedHeldNoBones, skinnedHidden);
            }
        }
        const PerfClock::time_point tBucket1 = PerfClock::now();
        dBucket = tBucket1 - tBucket0;

        for (auto& [meshHandle, bucket] : buckets) {
            if (bucket.members.empty()) continue;

            remixapi_InstanceInfo instance = {};
            instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
            instance.pNext = nullptr;
            instance.mesh = meshHandle;
            // Backface culling (2026-07-02). Previously hardcoded
            // doubleSided=1, which defeated backface culling in ray traversal
            // for the whole scene -- extra hit evaluations on every opaque
            // wall and rock. Honor the authored two-sided shader flag
            // (foliage, hair, fences), and keep water (visible from beneath)
            // and decals (planar; both faces kept so slight camera/parent
            // misalignment can't cull them) double-sided. Bucket members
            // share the flag because it's folded into the material cache key.
            {
                const DrawableInstance* rep = bucket.members[0];
                instance.doubleSided = (rep->isTwoSided || rep->isWater || rep->isDecal) ? 1 : 0;
            }
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

            remixapi_InstanceInfoGpuInstancingEXT  gpuExt   = {};
            remixapi_InstanceInfoBlendEXT          blendExt = {};
            remixapi_InstanceInfoBoneTransformsEXT bonesExt = {};

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
                // Skinned draw: chain the per-instance bone set. The base
                // transform is the bare mirror P (set at submit); bones are
                // Beth-world (det>0), so P both places the actor in Remix
                // space and drives the runtime's mirrored-facing flip.
                // Skinned meshes are keyed per-drawable in the mesh cache,
                // so they always land on this single-member path.
                if (member->isSkinned && !member->boneTransforms.empty()) {
                    bonesExt.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
                    bonesExt.pNext = instance.pNext;
                    bonesExt.boneTransforms_values = member->boneTransforms.data();
                    bonesExt.boneTransforms_count  = (uint32_t)member->boneTransforms.size();
                    instance.pNext = &bonesExt;
                }
            } else {
                // Batched path: identity base, per-instance transforms in pNext.
                ++batchedBucketCount;
                // Deterministic member order. The runtime folds the WHOLE
                // per-instance transform array (content AND order) into the
                // external draw identity hash (computeExternalDrawIdentityHash
                // -> gpuInstancingHash, rtx_scene_manager.cpp). Members arrive
                // in g_drawables iteration order, which permutes on rehash
                // (any insertion), churning the identity and breaking the
                // runtime's frame-over-frame RtInstance matching. Pointers are
                // node-stable in unordered_map, so sorting by pointer is
                // deterministic for a drawable's lifetime.
                std::sort(bucket.members.begin(), bucket.members.end());
                // Mirrored-facing fix (2026-07-08, [Performance]
                // BatchedMirrorBase). The runtime composes each per-instance
                // transform into the TLAS transform (rtx_accel_manager.cpp:
                // 1135), which per the Vulkan spec CANNOT change facing, and
                // XORs its facing-flip compensation from the BASE transform
                // only (isObjectToWorldMirrored, rtx_instance_manager.cpp:
                // 1300). Every FO4 placement is mirrored (det<0: the Beth->
                // Remix X/Y swap times a det>0 rotation and positive scale),
                // so an identity base left batched instances WITHOUT the flip
                // that single-member draws get -- repeated single-sided
                // statics (street lamps, PA stands, merge-expanded records)
                // rendered inside-out while unique placements were correct
                // (WindDiag 2026-07-08: byte-identical submissions, opposite
                // visual result). Hoist the X/Y-swap reflection P into the
                // base and pre-multiply each member by P^-1 (= P, a row 0/1
                // swap): composed placement P*(P^-1*M) == M is unchanged,
                // the base carries the mirror, and the runtime applies the
                // same flip as the single-member path.
                const bool mirrorBase = g_config.batchedMirrorBase;
                bucket.transforms.reserve(bucket.members.size());
                for (const DrawableInstance* member : bucket.members) {
                    remixapi_Transform xform = {};
                    for (int r = 0; r < 3; ++r) {
                        const int srcRow = mirrorBase ? (r == 0 ? 1 : (r == 1 ? 0 : 2)) : r;
                        for (int c = 0; c < 4; ++c) {
                            xform.matrix[r][c] = member->worldTransform[srcRow][c];
                        }
                    }
                    bucket.transforms.push_back(xform);
                }
                if (mirrorBase) {
                    instance.transform.matrix[0][1] = 1.0f;
                    instance.transform.matrix[1][0] = 1.0f;
                    instance.transform.matrix[2][2] = 1.0f;
                } else {
                    instance.transform.matrix[0][0] = 1.0f;
                    instance.transform.matrix[1][1] = 1.0f;
                    instance.transform.matrix[2][2] = 1.0f;
                }

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
        dDraw = PerfClock::now() - tBucket1;
    }

    s_accLockWaitNs += nsSince(tEnter, tLocked);
    s_accSnapNs     += nsSince(tSnap0, tSnap1);
    s_accBucketNs   += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(dBucket).count();
    s_accDrawNs     += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(dDraw).count();

    // Periodic status log (every ~5 seconds)
    static uint32_t s_frameCounter = 0;
    s_frameCounter++;
    if (s_frameCounter % 300 == 0) {
        // Stats-only full-map snapshot, once per window (diagnostic). The
        // per-frame snapshot this replaced ran 300x as often for the same
        // information.
        static std::unordered_set<uint64_t> statsScratch;
        statsScratch.clear();
        SemanticCapture::SnapshotActiveDrawables(Diagnostics::CurrentFrameIndex(),
                                                 kActiveAgeFrames, statsScratch,
                                                 &activeStats, nullptr);
        _MESSAGE("FO4RemixPlugin: OnFrame status - drawables=%zu buckets=%zu batched=%zu skippedInactive=%zu "
                 "skippedChunkPlayerInside=%zu skippedChunkStale=%zu chunks=%zu "
                 "active=%u isLod=%u fadedIn=%u notVisible=%u lodFadingOut=%u forcedFadeOut=%u "
                 "player=(%.0f,%.0f,%.0f)",
                 drawableCount, bucketCount, batchedBucketCount, skippedInactive,
                 skippedChunkPlayerInside, skippedChunkStale, lodChunkAges.size(),
                 activeStats.total, activeStats.isLod, activeStats.fadedIn,
                 activeStats.notVisible, activeStats.lodFadingOut, activeStats.forcedFadeOut,
                 cam.playerWorldPos[0], cam.playerWorldPos[1], cam.playerWorldPos[2]);

        // Perf: Remix-thread averages over the window (present/total lag one
        // frame -- they accumulate at the end of OnFrame). Game-thread costs
        // come from SemanticCapture's cumulative counters, diffed across the
        // window and normalized per game frame (Tick == one hkPresent).
        const auto game = SemanticCapture::GetPerfCounters();
        const uint64_t dTicks  = game.ticks  - s_lastGameCounters.ticks;
        const uint64_t dFires  = game.fires  - s_lastGameCounters.fires;
        const uint64_t dFireNs = game.fireNs - s_lastGameCounters.fireNs;
        const uint64_t dTickNs = game.tickNs - s_lastGameCounters.tickNs;
        s_lastGameCounters = game;
        _MESSAGE("FO4RemixPlugin: OnFrame perf (avg us/frame over 300) - "
                 "lockWait=%llu snap=%llu bucket=%llu draw=%llu present=%llu total=%llu | "
                 "game thread: fires/frame=%llu fireUs/frame=%llu tickUs/frame=%llu gameFrames=%llu",
                 (unsigned long long)(s_accLockWaitNs / 300 / 1000),
                 (unsigned long long)(s_accSnapNs     / 300 / 1000),
                 (unsigned long long)(s_accBucketNs   / 300 / 1000),
                 (unsigned long long)(s_accDrawNs     / 300 / 1000),
                 (unsigned long long)(s_accPresentNs  / 300 / 1000),
                 (unsigned long long)(s_accTotalNs    / 300 / 1000),
                 (unsigned long long)(dTicks ? dFires  / dTicks        : 0),
                 (unsigned long long)(dTicks ? dFireNs / dTicks / 1000 : 0),
                 (unsigned long long)(dTicks ? dTickNs / dTicks / 1000 : 0),
                 (unsigned long long)dTicks);
        s_accLockWaitNs = s_accSnapNs = s_accBucketNs = 0;
        s_accDrawNs = s_accPresentNs = s_accTotalNs = 0;

        // Remix-side VRAM on the same cadence: pairs with present_hook's
        // process-wide [VRAM] line to split "the path tracer's budget" from
        // "what the game's raster path pins" during the SuppressGameRaster
        // A/B runs.
        VramStats vram{};
        if (GetVramStats(&vram)) {
            _MESSAGE("FO4RemixPlugin: [VRAM] remix allocated=%llu MiB used=%llu MiB "
                     "materialTex=%llu MiB buffers=%llu MiB accel=%llu MiB parked=%zu",
                     (unsigned long long)(vram.totalAllocatedBytes >> 20),
                     (unsigned long long)(vram.totalUsedBytes >> 20),
                     (unsigned long long)(vram.usedMaterialTextureBytes >> 20),
                     (unsigned long long)(vram.usedBufferBytes >> 20),
                     (unsigned long long)(vram.usedAccelerationStructureBytes >> 20),
                     g_pendingDestroyCount.load(std::memory_order_relaxed));
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
        RemixCallGuarded("DrawInstance(fallback)",
                         [&] { api->DrawInstance(&instance); });
    }

    // ------------------------------------------------------------------
    // Placed lights (revived 2026-07-07). Drain the game thread's latest
    // snapshot, diff by hash (REFR formID -- static params per form, so an
    // existing hash keeps its handle), then draw every live handle: Remix
    // analytical lights are per-frame submissions like instances.
    // ------------------------------------------------------------------
    {
        bool haveSnapshot = false;
        std::vector<ExtractedLight> snapshot;
        {
            std::lock_guard<std::mutex> qlock(g_lightQueueMutex);
            if (g_lightSnapshotPending) {
                snapshot = std::move(g_lightSnapshot);
                g_lightSnapshot.clear();
                g_lightSnapshotPending = false;
                haveSnapshot = true;
            }
        }
        if (haveSnapshot && g_config.lightsEnabled) {
            std::unordered_map<uint64_t, const ExtractedLight*> desired;
            desired.reserve(snapshot.size());
            for (const auto& l : snapshot) desired.emplace(l.hash, &l);

            for (auto it = g_lights.begin(); it != g_lights.end();) {
                if (desired.find(it->first) == desired.end()) {
                    if (it->second) {
                        remixapi_LightHandle h = it->second;
                        RemixCallGuarded("DestroyLight",
                                         [&] { api->DestroyLight(h); });
                    }
                    it = g_lights.erase(it);
                } else {
                    ++it;
                }
            }
            static std::atomic<int> sLightLogs{0};
            uint32_t created = 0, failed = 0;
            for (const auto& [lhash, lp] : desired) {
                if (g_lights.find(lhash) != g_lights.end()) continue;
                const ExtractedLight& light = *lp;

                remixapi_LightInfoSphereEXT sphere = {};
                sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
                sphere.position = { light.position[0], light.position[1],
                                    light.position[2] };
                // Emitter size, not falloff: FO4's radius is the falloff
                // range; a small emitter sphere scaled from it keeps shadows
                // soft in proportion (0.025 = the retired pipeline's tuning).
                sphere.radius = (std::max)(light.radius * 0.025f *
                                           g_config.lightRadius, 0.5f);
                sphere.volumetricRadianceScale = 1.0f;
                if (light.isSpotLight && light.spotFOV > 0.0f) {
                    sphere.shaping_hasvalue = true;
                    sphere.shaping_value.direction = { light.spotDirection[0],
                                                       light.spotDirection[1],
                                                       light.spotDirection[2] };
                    // FO4 FOV is the full cone angle.
                    sphere.shaping_value.coneAngleDegrees = light.spotFOV * 0.5f;
                    sphere.shaping_value.coneSoftness = light.spotSoftness;
                    sphere.shaping_value.focusExponent = 0.0f;
                }

                float r = light.radiance[0], g = light.radiance[1],
                      b = light.radiance[2];
                const float cs = g_config.lightColorStrength;
                if (cs < 1.0f) {
                    const float avg = (r + g + b) / 3.0f;
                    r = avg + (r - avg) * cs;
                    g = avg + (g - avg) * cs;
                    b = avg + (b - avg) * cs;
                }

                remixapi_LightInfo info = {};
                info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
                info.pNext = &sphere;
                info.hash = lhash;
                info.radiance = { r * g_config.lightIntensity,
                                  g * g_config.lightIntensity,
                                  b * g_config.lightIntensity };
                info.isDynamic = false;
                info.ignoreViewModel = false;

                remixapi_LightHandle handle = nullptr;
                remixapi_ErrorCode lightErr = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
                RemixCallGuarded("CreateLight",
                                 [&] { lightErr = api->CreateLight(&info, &handle); });
                if (lightErr == REMIXAPI_ERROR_CODE_SUCCESS && handle) {
                    g_lights.emplace(lhash, handle);
                    ++created;
                    const int ln = sLightLogs.fetch_add(1,
                                       std::memory_order_relaxed);
                    if (ln < 12) {
                        _MESSAGE("FO4RemixPlugin: [Lights] #%d hash=0x%llX "
                                 "pos=(%.0f,%.0f,%.0f) radius=%.1f "
                                 "radiance=(%.1f,%.1f,%.1f) spot=%d",
                                 ln, (unsigned long long)lhash,
                                 light.position[0], light.position[1],
                                 light.position[2], sphere.radius,
                                 info.radiance.x, info.radiance.y,
                                 info.radiance.z,
                                 light.isSpotLight ? 1 : 0);
                    }
                } else {
                    ++failed;
                }
            }
            if (created || failed) {
                _MESSAGE("FO4RemixPlugin: [Lights] snapshot applied: %zu total, "
                         "%u created, %u failed, %zu live",
                         snapshot.size(), created, failed, g_lights.size());
            }
        }
        for (const auto& [lhash, handle] : g_lights) {
            if (handle) {
                remixapi_LightHandle h = handle;
                RemixCallGuarded("DrawLightInstance",
                                 [&] { api->DrawLightInstance(h); });
            }
        }
    }

    // Submit screen overlay (game UI/HUD captured from the DX11 UI render
    // target). Gated on g_config.hudOverlayEnabled; requires a runtime with
    // the rtx_fork_overlay.cpp layout fix (dxvk-remix 8990aed) -- older
    // runtimes asserted inside dxvk_barrier (dstLayout ==
    // VK_IMAGE_LAYOUT_UNDEFINED) the moment a HUD frame was staged. Enabled
    // via the shipped ini as of 2026-07-03.
    if (g_config.hudOverlayEnabled
        && overlay.valid && !overlay.pixels.empty()
        && api->DrawScreenOverlay) {
        remixapi_Format fmt = DxgiToRemixFormat(static_cast<DXGI_FORMAT>(overlay.dxgiFormat));
        if (fmt != static_cast<remixapi_Format>(0)) {
            RemixCallGuarded("DrawScreenOverlay", [&] {
                api->DrawScreenOverlay(overlay.pixels.data(), overlay.width,
                                       overlay.height, fmt, 1.0f);
            });
        }
    }

    // ------------------------------------------------------------------
    // LRU sweeps. Run every cullingTextureLRUSweepPeriod frames.
    //
    //   (1) Material sweep -- the LEVER: materials hold Rc<DxvkImageView>
    //       refs to textures, so parking stale materials for the next guarded
    //       destroy drain is what drops texture VRAM for shared sets. Gated by
    //       budget; below budget, no eviction (TTL-only mode skips this gate).
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

            // Material victims are only parked here; their runtime references
            // are released by a later top-of-frame drain. Keep the pre-sweep
            // VRAM reading for the texture budget pass instead of pretending
            // the queued releases have already reclaimed memory.

            if (g_config.cullingTextureLRUGraceFrames > 0) {
                auto texResult = RemixRenderer::SweepStaleTextures(
                    currentFrameIndex,
                    g_config.cullingTextureLRUGraceFrames,
                    budgetBytes,
                    currentMaterialTexBytes);
                _MESSAGE("FO4RemixPlugin: [LRU] Texture sweep: %u/%u stale, %u cells evicted, %u budget, %u orphans parked",
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
    const PerfClock::time_point tPresent0 = PerfClock::now();
    RemixCallGuarded("Present", [&] { api->Present(&presentInfo); });
    const PerfClock::time_point tEnd = PerfClock::now();
    s_accPresentNs += nsSince(tPresent0, tEnd);
    s_accTotalNs   += nsSince(tEnter, tEnd);
}

void RemixRenderer::Shutdown() {
    std::lock_guard<std::mutex> lock(g_renderStateMutex);

    remixapi_Interface* api = RemixAPI::GetInterface();

    // Drop drawable entries first; their meshHandle members alias g_meshCache,
    // so we don't DestroyMesh here -- the cache loop below does that once per
    // unique handle.
    g_drawables.clear();

    // Handles parked for deferred destruction were already erased from the
    // caches, so the loops below won't see them -- destroy them here (same
    // guarded helper as the OnFrame drain: a parked handle is the class most
    // likely to throw, and an unguarded throw here would crash on exit).
    if (api) {
        DestroyParkedHandles(api, g_pendingDestroys);
    }
    g_pendingDestroys = {};
    g_hasPendingDestroys.store(false, std::memory_order_release);

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

    if (api) {
        for (auto& [hash, handle] : g_lights) {
            if (handle) api->DestroyLight(handle);
        }
        g_lights.clear();
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

void RemixRenderer::QueueConfigVariable(const char* key, const char* value) {
    if (!key || !value) return;
    std::lock_guard<std::mutex> lock(g_configQueueMutex);
    g_pendingConfigVars[key] = value;
}

void RemixRenderer::QueueLights(std::vector<ExtractedLight>&& lights) {
    std::lock_guard<std::mutex> lock(g_lightQueueMutex);
    g_lightSnapshot = std::move(lights);
    g_lightSnapshotPending = true;
}

void RemixRenderer::QueueBoneTransforms(
    std::unordered_map<uint64_t, std::vector<remixapi_Transform>>&& bones) {
    std::lock_guard<std::mutex> lock(g_boneQueueMutex);
    if (g_boneQueue.empty()) {
        g_boneQueue = std::move(bones);
    } else {
        // OnFrame hasn't drained the previous Tick's sets yet (Remix thread
        // running slower than the game thread): newest set per drawable wins.
        for (auto& kv : bones) {
            g_boneQueue[kv.first] = std::move(kv.second);
        }
    }
}

bool RemixRenderer::HasTextureHandle(uint64_t hash) {
    std::lock_guard<std::mutex> lock(g_renderStateMutex);
    return g_textureHandles.find(hash) != g_textureHandles.end();
}
