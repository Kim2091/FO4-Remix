#include "bs_extraction.h"
#include "bcdec_bc7.h"  // vendored BC7 block decoder (bcdec, MIT/Unlicense)
#include "config.h"
#include "fo4_diagnostics.h"   // Diagnostics::CurrentFrameIndex for readback aging
#include "remix_renderer.h"    // HasTextureHandle for cache-hit handle recreation
#include "semantic_capture.h"  // GetLeafClassName (RTTI gate on the glowmap path)

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
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <wrl/client.h>

// ---------------------------------------------------------------------------
// Debug: dump an RGBA8 texture to a TGA file for visual inspection
// ---------------------------------------------------------------------------
static void DebugDumpTGA(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return;

    uint8_t header[18] = {};
    header[2]  = 2; // uncompressed true-color
    header[12] = (uint8_t)(w & 0xFF);
    header[13] = (uint8_t)(w >> 8);
    header[14] = (uint8_t)(h & 0xFF);
    header[15] = (uint8_t)(h >> 8);
    header[16] = 32; // bits per pixel
    header[17] = 0x20; // top-left origin
    f.write(reinterpret_cast<const char*>(header), 18);

    // TGA is BGRA, our data is RGBA
    for (uint32_t i = 0; i < w * h; i++) {
        uint8_t pixel[4] = { rgba[i*4+2], rgba[i*4+1], rgba[i*4+0], rgba[i*4+3] };
        f.write(reinterpret_cast<const char*>(pixel), 4);
    }
}

// ---------------------------------------------------------------------------
// g_player RelocPtr — same address as GameReferences.cpp but avoids pulling
// in the full GameReferences / GameForms dependency chain.
// ---------------------------------------------------------------------------
static RelocPtr<uintptr_t> s_g_player(0x032D2260);
static RelocPtr<uintptr_t> s_g_dataHandler(0x030DC000);

// Known offsets (verified by STATIC_ASSERTs in F4SE SDK headers):
//   TESObjectREFR::parentCell  = 0xB8  (GameReferences.h)
//   TESObjectREFR::unkF0       = 0xF0  (LoadedData*)
//   LoadedData::rootNode       = 0x08  (NiNode*)
//   TESObjectCELL::objectList  = 0x70  (tArray<TESObjectREFR*>)

static constexpr uintptr_t OFF_FORM_ID          = 0x14;
static constexpr uintptr_t OFF_REFR_PARENT_CELL = 0xB8;
static constexpr uintptr_t OFF_REFR_POS         = 0xD0;  // NiPoint3 (3 floats)
static constexpr uintptr_t OFF_REFR_LOADED_DATA = 0xF0;
static constexpr uintptr_t OFF_LOADED_ROOT_NODE = 0x08;
static constexpr uintptr_t OFF_CELL_OBJECT_LIST = 0x70;
static constexpr uintptr_t OFF_CELL_FLAGS       = 0x40;
static constexpr uintptr_t OFF_CELL_LAND        = 0x58;  // TESObjectLAND*
static constexpr uintptr_t OFF_CELL_WORLD_SPACE = 0xC8;
static constexpr uint16_t  CELL_FLAG_IS_INTERIOR = 0x0001;
static constexpr uintptr_t OFF_LAND_QUADRANTS   = 0x40;  // BSMultiBoundNode*[4]
static constexpr int       LAND_QUADRANT_COUNT  = 4;

// TES singleton and GridCellArray offsets (from static analysis of Fallout4.exe)
static RelocPtr<uintptr_t> s_g_tes(0x032D2048);
static constexpr uintptr_t OFF_TES_GRID_CELLS   = 0x18;  // GridCellArray*
static constexpr uintptr_t OFF_GRID_DIMENSION    = 0x10;  // int32 (= uGridsToLoad)
static constexpr uintptr_t OFF_GRID_CELL_ARRAY   = 0x18;  // TESObjectCELL** (flat dim*dim)



// Packed unsigned byte -> [-1, 1]
// Upload-resolution cap ([Materials] MaxTextureDimension). Applied at every
// point that reads a resident texture dimension -- the resolution hash fold,
// the readback mip selection, and GetMaterialDiffuseResidentWidth (which
// feeds both the upgrade poll and the submitted-width record) -- so all of
// them agree on the capped value and upgrade polling converges instead of
// endlessly re-extracting identical capped pixels from a larger resident.
static uint32_t CapDim(uint32_t v) {
    const uint32_t cap = g_config.maxTextureDimension;
    return (cap > 0 && v > cap) ? cap : v;
}

static float UnpackByte(uint8_t b) {
    return (b / 255.0f) * 2.0f - 1.0f;
}

// ---------------------------------------------------------------------------
// Texture cache — keyed by hash derived from ID3D11Resource pointer.
// `touch` is a monotonic use-stamp (NOT a frame index: a cell-load burst
// decodes dozens of textures in one frame, and frame-granular stamps tie,
// degrading eviction to arbitrary-victim under exactly the burst the budget
// exists to bound).
// ---------------------------------------------------------------------------
struct CachedTexture {
    // Shared with in-flight TextureSupply lists (see bs_extraction.h): the
    // supply pass hands SubmitDrawable a refcounted view of this chain
    // instead of a ~22MB copy. Never null for a live entry.
    std::shared_ptr<const ExtractedTexture> tex;
    uint64_t         touch = 0;       // monotonic use-stamp (LRU order)
    uint64_t         touchFrame = 0;  // frame of last use (cold gating)
};
static std::unordered_map<uint64_t, CachedTexture> g_textureCache;
static uint64_t g_textureCacheTouchCounter = 0;
// Resolution-variant index (TextureUpgradeOnApproach): pre-resolution-fold
// hash -> (full cache key, mip0 width) of the LARGEST variant extracted so
// far. When a strictly larger variant lands, the superseded entry's pixels
// are evicted from g_textureCache -- without this, an upgrade wave (arriving
// at a texture-dense cell) accumulates every (texture x resolution) variant's
// decompressed RGBA mip chain (~22MB per 2048^2) until allocation fails
// (2026-07-08/09 Boston-Airport fail-fast crashes). Smaller late variants
// (another drawable first-resolving while less is streamed in) are left
// alone; their own upgrade supersedes them soon after. Remix-side handles
// need nothing here: they are refcounted per drawable and destroyed on
// ReleaseDrawable.
static std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> g_texResVariantIndex;

// CPU-side cache budget (2026-07-10). Decoded RGBA chains are ~22 MiB per
// 2048^2 texture and the cache previously grew unbounded for the whole
// session (the variant eviction above only bounds the upgrade path). A
// running byte total is kept; past the configured budget ([Performance]
// CpuTextureCacheMiB) the least-recently-touched entries are evicted down
// to a low-water mark -- a later need re-runs the readback+decode, which
// the async pipeline tolerates. Game-thread-only, like the cache itself.
static size_t g_textureCacheBytes = 0;

// Negative cache: hashes whose decode DROPPED (mip-format mismatch -- a
// deterministic property of the content; transient readback failures never
// reach the drop site). Without it every resolver retry re-ran the full GPU
// readback + worker decode for a texture that can never succeed. Cleared
// with the cache.
static std::unordered_set<uint64_t> g_texKnownBad;

static void TextureCacheErase(uint64_t key)
{
    auto it = g_textureCache.find(key);
    if (it == g_textureCache.end()) return;
    if (it->second.tex) g_textureCacheBytes -= it->second.tex->pixels.size();
    g_textureCache.erase(it);
}

// Returns the stored shared chain so a supplying caller can alias it into
// its TextureSupply without another copy. Reference stays valid across other
// entries' erasure (unordered_map node stability).
static const std::shared_ptr<const ExtractedTexture>&
TextureCacheInsert(uint64_t key, ExtractedTexture&& t)
{
    CachedTexture& slot = g_textureCache[key];
    if (slot.tex) g_textureCacheBytes -= slot.tex->pixels.size();
    g_textureCacheBytes += t.pixels.size();
    slot.tex        = std::make_shared<const ExtractedTexture>(std::move(t));
    slot.touch      = ++g_textureCacheTouchCounter;
    slot.touchFrame = Diagnostics::CurrentFrameIndex();
    return slot.tex;
}

// Entries younger than this many frames are HOT and never evicted, no
// matter the budget. This cache is the working set that re-supplies Remix
// handles: the 2026-07-10 deployment proved that budget-only eviction of
// hot entries melts down -- every evicted texture immediately re-readbacks
// and re-decodes (its drawable is still resolving), which re-inserts and
// evicts others, a feedback loop that saturated the decode pool and the
// allocator until the process died. 600 frames ~= 10s, matching the
// Remix-side TTL grace.
constexpr uint64_t kTexCacheColdFrames = 600;

static void TextureCacheEnforceBudget()
{
    const uint64_t budgetMiB = g_config.cpuTextureCacheMiB;
    if (budgetMiB == 0) return;  // unbounded (legacy)
    const size_t budget = (size_t)budgetMiB * 1024u * 1024u;
    if (g_textureCacheBytes <= budget) return;

    // SOFT cap: evict COLD entries (untouched for kTexCacheColdFrames)
    // oldest-first down to a low-water mark. If the whole working set is
    // hot, the cache is allowed to exceed the budget -- unbounded growth
    // was the pre-budget status quo and is strictly safer than thrashing
    // the live set.
    const uint64_t now = Diagnostics::CurrentFrameIndex();
    std::vector<std::pair<uint64_t, uint64_t>> coldByAge;  // (touch, key)
    coldByAge.reserve(g_textureCache.size());
    for (const auto& [k, c] : g_textureCache) {
        const uint64_t age = now > c.touchFrame ? now - c.touchFrame : 0;
        if (age > kTexCacheColdFrames) {
            coldByAge.emplace_back(c.touch, k);
        }
    }

    static std::atomic<int> sEvictLogs{0};
    if (coldByAge.empty()) {
        const int n = sEvictLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 24) {
            _MESSAGE("FO4RemixPlugin: [TexCache] over budget (%zu MiB > %llu MiB) "
                     "but all %zu entries are hot -- not evicting",
                     g_textureCacheBytes / (1024u * 1024u),
                     (unsigned long long)budgetMiB, g_textureCache.size());
        }
        return;
    }
    std::sort(coldByAge.begin(), coldByAge.end());

    const size_t lowWater = budget - budget / 10;
    uint32_t evicted = 0;
    for (const auto& [touch, key] : coldByAge) {
        if (g_textureCacheBytes <= lowWater) break;
        TextureCacheErase(key);
        ++evicted;
    }
    if (evicted) {
        const int n = sEvictLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 24) {
            _MESSAGE("FO4RemixPlugin: [TexCache] budget %llu MiB exceeded: "
                     "evicted %u cold entries, now %zu entries / %zu MiB",
                     (unsigned long long)budgetMiB, evicted,
                     g_textureCache.size(),
                     g_textureCacheBytes / (1024u * 1024u));
        }
    }
}

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

        // BC2 / DXT3
        case DXGI_FORMAT_BC2_TYPELESS:      // 73
        case DXGI_FORMAT_BC2_UNORM:         // 74
        case DXGI_FORMAT_BC2_UNORM_SRGB:    // 75
            bw = (width + 3) / 4;  if (bw < 1) bw = 1;
            bh = (height + 3) / 4; if (bh < 1) bh = 1;
            return bw * bh * 16;

        // BC3 / DXT5
        case DXGI_FORMAT_BC3_TYPELESS:      // 76
        case DXGI_FORMAT_BC3_UNORM:         // 77
        case DXGI_FORMAT_BC3_UNORM_SRGB:    // 78
            bw = (width + 3) / 4;  if (bw < 1) bw = 1;
            bh = (height + 3) / 4; if (bh < 1) bh = 1;
            return bw * bh * 16;

        // BC4 (single-channel masks; decoded to RGBA8 grayscale in
        // ConvertReadbackMips -- remixapi has no BC4 format)
        case DXGI_FORMAT_BC4_TYPELESS:      // 79
        case DXGI_FORMAT_BC4_UNORM:         // 80
            bw = (width + 3) / 4;  if (bw < 1) bw = 1;
            bh = (height + 3) / 4; if (bh < 1) bh = 1;
            return bw * bh * 8;

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
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
            blockSize = 8;
            return true;

        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
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
// Async GPU readback (2026-07-02): read every mip level of a texture into CPU
// memory across ticks, without stalling the game thread.
//
// The old synchronous path did CopySubresourceRegion + an immediate
// Map(READ, 0) once PER MIP per texture; Map with no flags blocks the CPU
// until the GPU drains the copy -- a full pipeline stall. Cell streaming
// resolves dozens of textures, so a load burst meant hundreds of stalls
// (the measured multi-ms SemanticCapture::Tick spikes and hitching).
//
// Two-phase protocol, keyed by the extraction cache hash:
//   phase 1 (first attempt): allocate ONE staging texture holding the whole
//     usable mip chain, queue one CopySubresourceRegion per mip, park it in
//     g_pendingReadbacks. Returns Pending -- no waiting.
//   phase 2 (later ticks):  Map mip 0 with D3D11_MAP_FLAG_DO_NOT_WAIT.
//     WAS_STILL_DRAWING -> still Pending, try next tick. Success means every
//     queued copy has completed (one in-order command stream), so the
//     remaining mips map without blocking.
// The resolver already retries unresolved textures every tick, so Pending
// costs one gate check per tick and nothing else.
//
// Why all mips: the path tracer samples normal/diffuse maps at varying LOD
// based on screen-space pixel footprint. Without a mip chain, a distant
// surface fetches the full-res mip at sub-pixel rate, average-out flattens
// the normal, and the BRDF degenerates -- most visibly on water, where a
// flat normal + high IOR + Fresnel collapses to a perfect mirror.
//
// BC chains truncate at the 4x4 block boundary: D3D11 rejects standalone BC
// resources below one block, and a BC mip below 4x4 is at most 4 texels of
// pre-filtered detail -- noise to the path tracer's sampler. Without the
// gate, every BC texture was rejected and re-attempted every frame (1 FPS
// regression with most terrain missing, observed 2026-04-29).
// ---------------------------------------------------------------------------
enum class ReadbackStatus { Ready, Pending, Failed };

struct PendingReadback {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    DXGI_FORMAT format        = DXGI_FORMAT_UNKNOWN;
    uint32_t    width         = 0;   // mip 0 dimensions
    uint32_t    height        = 0;
    uint32_t    mipCount      = 0;   // usable (BC-truncated) chain length
    uint32_t    ticksWaited   = 0;   // phase-2 attempts; TTL'd so a wedged
                                     // copy queue can't pin the entry forever
    uint64_t    lastTouchFrame = 0;  // last frame any caller polled this entry;
                                     // abandoned entries (drawable evicted mid-
                                     // readback) are swept when capacity is hit
};

static std::unordered_map<uint64_t, PendingReadback> g_pendingReadbacks;

// Bound VRAM parked in staging during load bursts. Excess textures stay
// unresolved and start their copies on a later tick via the resolver retry.
// Sizing: a fresh area resolves hundreds of unique texture variants, and a
// pipeline slot turns over in ~2-3 ticks, so the cap directly sets scene
// pop-in time (32 slots measured ~20s of visible pop-in; the staging VRAM is
// transient -- a 1K BC7 chain is ~1.4MB, so even 256 in flight is a few
// hundred MB that drains within a second or two). Ini-tunable via
// [Performance] MaxPendingTextureReadbacks.
static constexpr size_t   kDefaultMaxPendingReadbacks = 256;
static size_t MaxPendingReadbacks() {
    return g_config.maxPendingTextureReadbacks > 0
        ? (size_t)g_config.maxPendingTextureReadbacks
        : kDefaultMaxPendingReadbacks;
}
// Phase-2 attempts before an entry is abandoned (a copy that hasn't landed
// after ~10s of ticks means the caller stopped retrying or the queue died).
static constexpr uint32_t kPendingReadbackTTL  = 600;

static ReadbackStatus ReadbackAllMipsAsync(ID3D11Device* device,
                                           ID3D11Texture2D* tex2D,
                                           uint64_t cacheKey,
                                           std::vector<ExtractedTexture>& outMips)
{
    using Microsoft::WRL::ComPtr;

    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    if (!ctx) return ReadbackStatus::Failed;

    const uint64_t nowFrame = Diagnostics::CurrentFrameIndex();

    auto pendingIt = g_pendingReadbacks.find(cacheKey);
    if (pendingIt == g_pendingReadbacks.end()) {
        // ---- Phase 1: queue the copies, park the staging. ----
        if (g_pendingReadbacks.size() >= MaxPendingReadbacks()) {
            // Sweep abandoned entries (drawable evicted mid-readback, so no
            // caller polls them again). Without this, a full map of abandoned
            // entries would block every future readback permanently.
            for (auto it2 = g_pendingReadbacks.begin(); it2 != g_pendingReadbacks.end();) {
                if (nowFrame - it2->second.lastTouchFrame > kPendingReadbackTTL) {
                    it2 = g_pendingReadbacks.erase(it2);
                } else {
                    ++it2;
                }
            }
            if (g_pendingReadbacks.size() >= MaxPendingReadbacks()) {
                return ReadbackStatus::Pending;
            }
        }

        D3D11_TEXTURE2D_DESC desc;
        tex2D->GetDesc(&desc);

        if (ComputeMip0Size(desc.Width, desc.Height, desc.Format) == 0) {
            _MESSAGE("FO4RemixPlugin: ReadbackAllMipsAsync - unsupported DXGI format %u, skipping",
                     (unsigned)desc.Format);
            return ReadbackStatus::Failed;
        }

        const uint32_t srcMipCount = desc.MipLevels > 0 ? desc.MipLevels : 1;
        uint32_t blockSize = 0;
        const bool isBC = IsBlockCompressed(desc.Format, blockSize);

        // Upload-resolution cap: skip the leading mips that exceed
        // [Materials] MaxTextureDimension. With 4K texture mods resident at
        // load, full-chain uploads put ~4x the intended bytes in the Remix
        // material-texture pool (2026-07-11: materialTex hit 11.4 GiB and
        // paged the whole process out of the adapter budget). The engine's
        // own chain already contains the smaller mips -- no resampling, the
        // readback simply starts lower. Also 4x less readback bandwidth,
        // decode work, and CPU cache per capped texture.
        uint32_t firstMip = 0;
        if (g_config.maxTextureDimension > 0) {
            while (firstMip + 1 < srcMipCount) {
                uint32_t w = desc.Width  >> firstMip; if (w == 0) w = 1;
                uint32_t h = desc.Height >> firstMip; if (h == 0) h = 1;
                if (w <= g_config.maxTextureDimension &&
                    h <= g_config.maxTextureDimension) break;
                ++firstMip;
            }
        }
        uint32_t topW = desc.Width  >> firstMip; if (topW == 0) topW = 1;
        uint32_t topH = desc.Height >> firstMip; if (topH == 0) topH = 1;

        uint32_t usableMips = 0;
        for (uint32_t i = firstMip; i < srcMipCount; i++) {
            uint32_t mipW = desc.Width  >> i; if (mipW == 0) mipW = 1;
            uint32_t mipH = desc.Height >> i; if (mipH == 0) mipH = 1;
            if (isBC && (mipW < 4 || mipH < 4)) break;
            usableMips++;
        }
        if (usableMips == 0) return ReadbackStatus::Failed;

        D3D11_TEXTURE2D_DESC stagingDesc = {};
        stagingDesc.Width              = topW;
        stagingDesc.Height             = topH;
        stagingDesc.MipLevels          = usableMips;
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
            _MESSAGE("FO4RemixPlugin: ReadbackAllMipsAsync - staging CreateTexture2D failed hr=0x%08X",
                     (unsigned)hr);
            return ReadbackStatus::Failed;
        }

        // Queue all copies; the D3D11 runtime holds a reference on both
        // resources until the commands execute, so the engine freeing the
        // source texture later is safe. Source mips are offset by firstMip
        // when the resolution cap dropped the top of the chain.
        for (uint32_t i = 0; i < usableMips; i++) {
            ctx->CopySubresourceRegion(staging.Get(), i, 0, 0, 0, tex2D,
                                       firstMip + i, nullptr);
        }

        PendingReadback pr;
        pr.staging        = staging;
        pr.format         = desc.Format;
        pr.width          = topW;
        pr.height         = topH;
        pr.mipCount       = usableMips;
        pr.lastTouchFrame = nowFrame;
        g_pendingReadbacks.emplace(cacheKey, std::move(pr));
        return ReadbackStatus::Pending;
    }

    // ---- Phase 2: non-blocking poll, then drain the chain. ----
    PendingReadback& pr = pendingIt->second;
    pr.lastTouchFrame = nowFrame;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = ctx->Map(pr.staging.Get(), 0, D3D11_MAP_READ,
                          D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        if (++pr.ticksWaited > kPendingReadbackTTL) {
            g_pendingReadbacks.erase(pendingIt);
            return ReadbackStatus::Failed;
        }
        return ReadbackStatus::Pending;
    }
    if (FAILED(hr)) {
        _MESSAGE("FO4RemixPlugin: ReadbackAllMipsAsync - Map failed hr=0x%08X", (unsigned)hr);
        g_pendingReadbacks.erase(pendingIt);
        return ReadbackStatus::Failed;
    }

    uint32_t blockSize = 0;
    const bool bc = IsBlockCompressed(pr.format, blockSize);

    outMips.clear();
    outMips.reserve(pr.mipCount);

    // Mip 0 mapped without waiting, so every queued copy in the chain has
    // completed (single in-order command stream) -- mips 1+ map instantly.
    for (uint32_t i = 0; i < pr.mipCount; i++) {
        uint32_t mipW = pr.width  >> i; if (mipW == 0) mipW = 1;
        uint32_t mipH = pr.height >> i; if (mipH == 0) mipH = 1;

        if (i > 0) {
            hr = ctx->Map(pr.staging.Get(), i, D3D11_MAP_READ, 0, &mapped);
            if (FAILED(hr)) {
                _MESSAGE("FO4RemixPlugin: ReadbackAllMipsAsync - Map mip=%u failed hr=0x%08X",
                         i, (unsigned)hr);
                outMips.clear();
                g_pendingReadbacks.erase(pendingIt);
                return ReadbackStatus::Failed;
            }
        }

        uint32_t numRows;       // scanline-rows (or block-rows for BC)
        uint32_t expectedPitch; // tight row pitch
        if (bc) {
            uint32_t bw = (mipW + 3) / 4; if (bw < 1) bw = 1;
            uint32_t bh = (mipH + 3) / 4; if (bh < 1) bh = 1;
            numRows       = bh;
            expectedPitch = bw * blockSize;
        } else {
            numRows       = mipH;
            expectedPitch = mipW * 4; // all supported uncompressed formats are 4 bpp
        }

        ExtractedTexture mip;
        mip.pixels.resize(ComputeMip0Size(mipW, mipH, pr.format));
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
        uint8_t* dst = mip.pixels.data();
        for (uint32_t row = 0; row < numRows; row++) {
            memcpy(dst, src, expectedPitch);
            src += mapped.RowPitch;
            dst += expectedPitch;
        }
        ctx->Unmap(pr.staging.Get(), i);

        mip.width      = mipW;
        mip.height     = mipH;
        mip.dxgiFormat = pr.format;
        mip.mipLevels  = 1;
        outMips.push_back(std::move(mip));
    }

    g_pendingReadbacks.erase(pendingIt);
    return outMips.empty() ? ReadbackStatus::Failed : ReadbackStatus::Ready;
}

// Runtime-format gamma test (defined with the LUT helpers below; needed here
// so the decompressors can preserve the source's sRGB designation).
namespace { bool IsSrgbColorFormat(DXGI_FORMAT f); }

// ---------------------------------------------------------------------------
// BC1/BC3 decode helpers — decompress a 4x4 block to RGBA8
// ---------------------------------------------------------------------------
// `forceFourColor`: BC2/BC3 color blocks are ALWAYS 4-color mode per the
// D3D spec (the c0<=c1 3-color+transparent mode is BC1-only), and encoders
// exploit that by emitting unordered endpoints -- decoding them with BC1
// rules corrupts those blocks (wrong color3 + phantom transparency).
static void DecodeBC1ColorBlock(const uint8_t* block, uint8_t out[4][4][4],
                                bool forceFourColor = false)
{
    uint16_t c0 = block[0] | (block[1] << 8);
    uint16_t c1 = block[2] | (block[3] << 8);

    uint8_t colors[4][4];
    colors[0][0] = ((c0 >> 11) & 0x1F) * 255 / 31;
    colors[0][1] = ((c0 >>  5) & 0x3F) * 255 / 63;
    colors[0][2] = ( c0        & 0x1F) * 255 / 31;
    colors[0][3] = 255;
    colors[1][0] = ((c1 >> 11) & 0x1F) * 255 / 31;
    colors[1][1] = ((c1 >>  5) & 0x3F) * 255 / 63;
    colors[1][2] = ( c1        & 0x1F) * 255 / 31;
    colors[1][3] = 255;

    if (forceFourColor || c0 > c1) {
        for (int i = 0; i < 3; i++) {
            colors[2][i] = (2 * colors[0][i] + colors[1][i] + 1) / 3;
            colors[3][i] = (colors[0][i] + 2 * colors[1][i] + 1) / 3;
        }
        colors[2][3] = 255;
        colors[3][3] = 255;
    } else {
        // 1-bit-alpha mode: index 3 is TRANSPARENT black per the BC1 spec.
        // Decoding it opaque (the pre-2026-07-10 behavior) turned every
        // self-decoded BC1 cutout (foliage atlases through the bake paths)
        // into solid black holes. Callers that synthesize their own alpha
        // (DiffuseAlphaFromLuminance, BC2/BC3 explicit alpha) overwrite
        // this anyway.
        for (int i = 0; i < 3; i++) {
            colors[2][i] = (colors[0][i] + colors[1][i]) / 2;
            colors[3][i] = 0;
        }
        colors[2][3] = 255;
        colors[3][3] = 0;
    }

    uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int idx = indices & 3;
            indices >>= 2;
            out[y][x][0] = colors[idx][0];
            out[y][x][1] = colors[idx][1];
            out[y][x][2] = colors[idx][2];
            out[y][x][3] = colors[idx][3];
        }
    }
}

static void DecodeBC3AlphaBlock(const uint8_t* block, uint8_t alphas[4][4])
{
    uint8_t a0 = block[0], a1 = block[1];
    uint8_t palette[8];
    palette[0] = a0;
    palette[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; i++)
            palette[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
    } else {
        for (int i = 1; i <= 4; i++)
            palette[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
        palette[6] = 0;
        palette[7] = 255;
    }

    uint64_t bits = 0;
    for (int i = 2; i < 8; i++)
        bits |= (uint64_t)block[i] << ((i - 2) * 8);

    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            alphas[y][x] = palette[bits & 7];
            bits >>= 3;
        }
}

// Decompress a BC2 (DXT3) texture to R8G8B8A8
// BC2 = 8 bytes explicit 4-bit alpha + 8 bytes BC1 color per 4x4 block
static bool DecompressBC2(ExtractedTexture& tex)
{
    bool isBC2 = (tex.dxgiFormat == DXGI_FORMAT_BC2_UNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC2_UNORM_SRGB ||
                  tex.dxgiFormat == DXGI_FORMAT_BC2_TYPELESS);
    if (!isBC2) return false;

    uint32_t bw = (tex.width + 3) / 4;
    uint32_t bh = (tex.height + 3) / 4;

    std::vector<uint8_t> rgba(tex.width * tex.height * 4);
    const uint8_t* src = tex.pixels.data();

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            // First 8 bytes: explicit 4-bit alpha for each of the 16 pixels
            uint8_t alphas[4][4];
            for (int y = 0; y < 4; y++) {
                uint8_t lo = src[y * 2];
                uint8_t hi = src[y * 2 + 1];
                alphas[y][0] = (lo & 0x0F) | ((lo & 0x0F) << 4);
                alphas[y][1] = (lo >> 4)   | ((lo >> 4) << 4);
                alphas[y][2] = (hi & 0x0F) | ((hi & 0x0F) << 4);
                alphas[y][3] = (hi >> 4)   | ((hi >> 4) << 4);
            }

            // Next 8 bytes: BC1-layout color block (always 4-color for BC2)
            uint8_t block[4][4][4];
            DecodeBC1ColorBlock(src + 8, block, /*forceFourColor=*/true);

            // Combine color + alpha
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++)
                    block[y][x][3] = alphas[y][x];
            }

            src += 16;

            // Write pixels
            for (int y = 0; y < 4; y++) {
                uint32_t py = by * 4 + y;
                if (py >= tex.height) continue;
                for (int x = 0; x < 4; x++) {
                    uint32_t px = bx * 4 + x;
                    if (px >= tex.width) continue;
                    uint32_t offset = (py * tex.width + px) * 4;
                    memcpy(&rgba[offset], block[y][x], 4);
                }
            }
        }
    }

    tex.pixels = std::move(rgba);
    // Preserve the source's gamma designation: the decoded bytes are still
    // sRGB-encoded when the source was sRGB, and tagging them UNORM made
    // Remix read them as linear (washed-out albedo on every decode path).
    tex.dxgiFormat = IsSrgbColorFormat((DXGI_FORMAT)tex.dxgiFormat)
        ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    return true;
}

// Signed variant of the BC3/BC4 alpha-style block for BC5_SNORM: endpoints
// and palette are int8, remapped to [0,255] with -127 -> 0, 127 -> 255 so
// downstream v/255*2-1 math lands on the authored signed value. (Unsigned
// decode of SNORM data produced garbage normals.)
static void DecodeBC4SignedBlock(const uint8_t* block, uint8_t out[4][4])
{
    auto clampS8 = [](int v) { return v < -127 ? -127 : v; };  // -128 == -127 per spec
    const int a0 = clampS8((int8_t)block[0]);
    const int a1 = clampS8((int8_t)block[1]);
    int palette[8];
    palette[0] = a0;
    palette[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; i++)
            palette[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (int i = 1; i <= 4; i++)
            palette[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        palette[6] = -127;
        palette[7] = 127;
    }

    uint64_t bits = 0;
    for (int i = 2; i < 8; i++)
        bits |= (uint64_t)block[i] << ((i - 2) * 8);

    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            const int s = palette[bits & 7];              // [-127, 127]
            out[y][x] = (uint8_t)(((s + 127) * 255 + 127) / 254);
            bits >>= 3;
        }
}

// Unified BC1/BC3/BC4/BC5 block decompression with per-pixel transform
enum class BCTransform { None, InvertRGB, NormalReconstructZ };

static bool DecompressBC(ExtractedTexture& tex, BCTransform transform)
{
    bool isBC1 = (tex.dxgiFormat == DXGI_FORMAT_BC1_UNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC1_UNORM_SRGB ||
                  tex.dxgiFormat == DXGI_FORMAT_BC1_TYPELESS);
    bool isBC3 = (tex.dxgiFormat == DXGI_FORMAT_BC3_UNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC3_UNORM_SRGB ||
                  tex.dxgiFormat == DXGI_FORMAT_BC3_TYPELESS);
    bool isBC4 = (tex.dxgiFormat == DXGI_FORMAT_BC4_UNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC4_TYPELESS);
    bool isBC5 = (tex.dxgiFormat == DXGI_FORMAT_BC5_UNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC5_SNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC5_TYPELESS);
    const bool isBC5Signed = tex.dxgiFormat == DXGI_FORMAT_BC5_SNORM;

    if (!isBC1 && !isBC3 && !isBC4 && !isBC5) return false;

    uint32_t bw = (tex.width + 3) / 4;
    uint32_t bh = (tex.height + 3) / 4;
    uint32_t blockSize = (isBC1 || isBC4) ? 8 : 16;

    std::vector<uint8_t> rgba(tex.width * tex.height * 4);
    const uint8_t* src = tex.pixels.data();

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t block[4][4][4]; // [y][x][rgba]

            if (isBC4) {
                // BC4: one alpha-style block, single channel. Replicate to
                // RGB (grayscale masks read the same from any channel).
                uint8_t rChan[4][4];
                DecodeBC3AlphaBlock(src, rChan);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        const uint8_t v = (transform == BCTransform::InvertRGB)
                            ? (uint8_t)(255 - rChan[y][x]) : rChan[y][x];
                        block[y][x][0] = v;
                        block[y][x][1] = v;
                        block[y][x][2] = v;
                        block[y][x][3] = 255;
                    }
            } else if (isBC5) {
                // BC5: two alpha-style blocks for R and G channels
                uint8_t rChan[4][4], gChan[4][4];
                if (isBC5Signed) {
                    DecodeBC4SignedBlock(src, rChan);
                    DecodeBC4SignedBlock(src + 8, gChan);
                } else {
                    DecodeBC3AlphaBlock(src, rChan);
                    DecodeBC3AlphaBlock(src + 8, gChan);
                }

                if (transform == BCTransform::InvertRGB) {
                    // R=specular, G=smoothness. Invert G to get roughness.
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++) {
                            uint8_t roughness = 255 - gChan[y][x];
                            block[y][x][0] = roughness;
                            block[y][x][1] = roughness;
                            block[y][x][2] = roughness;
                            block[y][x][3] = 255;
                        }
                } else {
                    // NormalReconstructZ (or None): reconstruct Z from R,G
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++) {
                            float nx = rChan[y][x] / 255.0f * 2.0f - 1.0f;
                            float ny = gChan[y][x] / 255.0f * 2.0f - 1.0f;
                            float nz2 = 1.0f - nx * nx - ny * ny;
                            float nz = nz2 > 0.0f ? sqrtf(nz2) : 0.0f;
                            block[y][x][0] = rChan[y][x];
                            block[y][x][1] = gChan[y][x];
                            block[y][x][2] = (uint8_t)(nz * 127.5f + 127.5f);
                            block[y][x][3] = 255;
                        }
                }
            } else if (isBC3) {
                uint8_t alphas[4][4];
                DecodeBC3AlphaBlock(src, alphas);
                DecodeBC1ColorBlock(src + 8, block, /*forceFourColor=*/true);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++)
                        block[y][x][3] = alphas[y][x];
            } else {
                DecodeBC1ColorBlock(src, block);
            }
            src += blockSize;

            // Write decoded pixels to output buffer
            if (transform == BCTransform::InvertRGB && !isBC5 && !isBC4) {
                // Invert RGB for BC1/BC3 (BC4/BC5 already handled above)
                for (int y = 0; y < 4; y++) {
                    uint32_t py = by * 4 + y;
                    if (py >= tex.height) continue;
                    for (int x = 0; x < 4; x++) {
                        uint32_t px = bx * 4 + x;
                        if (px >= tex.width) continue;
                        uint32_t offset = (py * tex.width + px) * 4;
                        rgba[offset + 0] = 255 - block[y][x][0]; // invert R
                        rgba[offset + 1] = 255 - block[y][x][1]; // invert G
                        rgba[offset + 2] = 255 - block[y][x][2]; // invert B
                        rgba[offset + 3] = block[y][x][3];       // keep A
                    }
                }
            } else {
                // No transform, or BC4/BC5 already transformed above
                for (int y = 0; y < 4; y++) {
                    uint32_t py = by * 4 + y;
                    if (py >= tex.height) continue;
                    for (int x = 0; x < 4; x++) {
                        uint32_t px = bx * 4 + x;
                        if (px >= tex.width) continue;
                        uint32_t offset = (py * tex.width + px) * 4;
                        memcpy(&rgba[offset], block[y][x], 4);
                    }
                }
            }
        }
    }

    tex.pixels = std::move(rgba);
    // Preserve the source's gamma designation for COLOR decodes: the bytes
    // stay sRGB-encoded when the source was sRGB, and the old unconditional
    // UNORM tag made Remix read them as linear -> washed-out albedo on every
    // tint/palette/lum-floor/cutout bake. Data decodes (roughness invert,
    // normal reconstruct; BC4/BC5 sources are UNORM anyway) keep UNORM.
    tex.dxgiFormat = (transform == BCTransform::None &&
                      IsSrgbColorFormat((DXGI_FORMAT)tex.dxgiFormat))
        ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    return true;
}

// Decompress a BC7 texture to RGBA8 via the vendored bcdec decoder
// (bcdec_bc7.h). BC7 used to be pass-through-only: it displays fine
// compressed, but every per-pixel transform (palette LUT remap, tint bake,
// smoothness->roughness inversion, octahedral normal encode, luminance
// floor) DECLINED on it -- BC7 smoothness maps shipped un-inverted (shiny/
// dull flipped) and BC7 diffuses lost their tints. Preserves the source's
// sRGB designation like DecompressBC.
static bool DecompressBC7(ExtractedTexture& tex)
{
    const bool isBC7 = (tex.dxgiFormat == DXGI_FORMAT_BC7_UNORM ||
                        tex.dxgiFormat == DXGI_FORMAT_BC7_UNORM_SRGB ||
                        tex.dxgiFormat == DXGI_FORMAT_BC7_TYPELESS);
    if (!isBC7) return false;

    const uint32_t bw = (tex.width + 3) / 4;
    const uint32_t bh = (tex.height + 3) / 4;
    std::vector<uint8_t> rgba((size_t)tex.width * tex.height * 4);
    const uint8_t* src = tex.pixels.data();

    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx, src += 16) {
            if (bx * 4 + 4 <= tex.width && by * 4 + 4 <= tex.height) {
                // Interior block: decode straight into the output rows.
                uint8_t* dst = rgba.data() +
                    ((size_t)by * 4 * tex.width + (size_t)bx * 4) * 4;
                bcdec_bc7(src, dst, (int)(tex.width * 4));
            } else {
                // Edge block of a non-multiple-of-4 mip: bcdec writes full
                // 4x4 rows, so decode into scratch and copy what's in range.
                uint8_t scratch[4][4][4];
                bcdec_bc7(src, scratch, 16);
                for (uint32_t y = 0; y < 4; ++y) {
                    const uint32_t py = by * 4 + y;
                    if (py >= tex.height) break;
                    for (uint32_t x = 0; x < 4; ++x) {
                        const uint32_t px = bx * 4 + x;
                        if (px >= tex.width) continue;
                        memcpy(&rgba[((size_t)py * tex.width + px) * 4],
                               scratch[y][x], 4);
                    }
                }
            }
        }
    }

    tex.pixels = std::move(rgba);
    tex.dxgiFormat = IsSrgbColorFormat((DXGI_FORMAT)tex.dxgiFormat)
        ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    return true;
}

// Invert an uncompressed RGBA texture in-place (smoothness → roughness)
static void InvertUncompressed(ExtractedTexture& tex)
{
    for (uint32_t i = 0; i < tex.width * tex.height; i++) {
        tex.pixels[i * 4 + 0] = 255 - tex.pixels[i * 4 + 0];
        tex.pixels[i * 4 + 1] = 255 - tex.pixels[i * 4 + 1];
        tex.pixels[i * 4 + 2] = 255 - tex.pixels[i * 4 + 2];
        // leave alpha
    }
}

// Synthesize alpha = max(R,G,B) for diffuse textures whose authored alpha
// channel is unreliable for cutout. BGS LOD foliage atlases (Commonwealth.
// Objects.DDS) are BC1 with no alpha at all; vanilla DX11's rasterizer hides
// this because cutout regions render as dark blobs that blend into the
// distance, but Remix's path tracer applies our converted-from-smoothness
// roughness map at those pixels and produces mirror-reflective rectangles
// where vanilla showed dark.
//
// `forceForBC3` controls behavior on BC3/BC7/RGBA8 inputs:
//   false (default): no-op for non-BC1, preserves their authored alpha
//   true: ALSO decompress + overwrite alpha for BC3 inputs. Used for
//         decal-tagged surfaces, where BGS sometimes packs non-cutout data
//         in BC3.a so the authored alpha doesn't behave as a clean mask.
static void DiffuseAlphaFromLuminance_Apply(ExtractedTexture& tex, bool forceForBC3)
{
    bool isBC1 = (tex.dxgiFormat == DXGI_FORMAT_BC1_UNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC1_UNORM_SRGB ||
                  tex.dxgiFormat == DXGI_FORMAT_BC1_TYPELESS);
    bool isBC3 = (tex.dxgiFormat == DXGI_FORMAT_BC3_UNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC3_UNORM_SRGB ||
                  tex.dxgiFormat == DXGI_FORMAT_BC3_TYPELESS);

    if (!isBC1 && !(forceForBC3 && isBC3)) return;

    // Reuse DecompressBC with BCTransform::None to get RGBA8.
    if (!DecompressBC(tex, BCTransform::None)) return;

    for (uint32_t i = 0; i < tex.width * tex.height; i++) {
        uint8_t r = tex.pixels[i * 4 + 0];
        uint8_t g = tex.pixels[i * 4 + 1];
        uint8_t b = tex.pixels[i * 4 + 2];
        // max(R,G,B) instead of perceptual luminance (0.299R+0.587G+0.114B):
        // BGS atlases pack the silhouette as "dark = cutout, any color = leaf",
        // so the brightest channel is the most reliable cutout signal. A
        // perceptual weighting would discard mid-bright red/blue pixels.
        uint8_t a = (r > g) ? (r > b ? r : b) : (g > b ? g : b);
        tex.pixels[i * 4 + 3] = a;
    }
}

// Hue-preserving luminance floor (metal-conversion albedo treatment).
// FO4 metal albedo is authored near-black (the vanilla look is built from
// specular + envmap on top), and Remix's metal BRDF takes F0 from albedo --
// black albedo means a black metal no matter what the metallic constant
// says. Pixels below the luminance floor are scaled up multiplicatively so
// their hue survives (rust stays rust-colored, just brighter); pixels
// already above the floor are untouched, so bright/painted regions keep
// their authored color. This is deliberately NOT a blend toward the
// material's spec tint -- that variant (506e5e7, reverted) dragged every
// classified surface toward the tint (white-washed weapons).
//
// Scale is capped at 6x to keep quantization noise in very dark texels from
// blowing up into confetti; whatever the cap leaves below the floor is
// topped up with a neutral (gray) fill. Alpha is untouched (cutouts
// survive).
// Multiply every pixel's RGB by an 8-bit tint (0xRRGGBB). FO4 tint materials
// (SkinTint / HairTint) author unpigmented/grayscale diffuse maps and let the
// shader multiply kTintColor at draw time; this bakes that multiply into the
// uploaded texture. Alpha untouched. BC inputs decompressed first (same
// contract as AlbedoLumFloor_Apply); BC7/other left as-is.
static void TintMultiply_Apply(ExtractedTexture& tex, uint32_t tintRGB)
{
    switch (tex.dxgiFormat) {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_BC5_TYPELESS:
            if (!DecompressBC(tex, BCTransform::None)) return;
            break;
        default:
            break;
    }
    const bool rgba = (tex.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
                       tex.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const bool bgra = (tex.dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM ||
                       tex.dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    if (!rgba && !bgra) {
        return;  // BC7/other: can't process without a decoder; leave as-is
    }
    const uint32_t tr = (tintRGB >> 16) & 0xFF;
    const uint32_t tg = (tintRGB >> 8) & 0xFF;
    const uint32_t tb = tintRGB & 0xFF;
    const uint32_t c0 = bgra ? tb : tr;   // channel 0 multiplier
    const uint32_t c2 = bgra ? tr : tb;   // channel 2 multiplier
    for (uint32_t i = 0; i < tex.width * tex.height; i++) {
        uint8_t* p = &tex.pixels[i * 4];
        p[0] = (uint8_t)((p[0] * c0 + 127u) / 255u);
        p[1] = (uint8_t)((p[1] * tg + 127u) / 255u);
        p[2] = (uint8_t)((p[2] * c2 + 127u) / 255u);
    }
}

static void AlbedoLumFloor_Apply(ExtractedTexture& tex, uint8_t lumFloor)
{
    // BC input: decompress to RGBA8 first (no channel transform).
    switch (tex.dxgiFormat) {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_BC5_TYPELESS:
            if (!DecompressBC(tex, BCTransform::None)) return;
            break;
        default:
            break;
    }
    const bool rgba = (tex.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
                       tex.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const bool bgra = (tex.dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM ||
                       tex.dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    if (!rgba && !bgra) {
        return;  // BC7/other: can't process without a decoder; leave as-is
    }

    // Rec.709 luma weights in /256 fixed point (54+183+19 = 256). Channel
    // order only swaps the R/B weights.
    const uint32_t wr = bgra ? 19u : 54u;
    const uint32_t wb = bgra ? 54u : 19u;
    constexpr uint32_t kMaxScaleFP = 6u * 256u;  // 6x cap, 8.8 fixed point

    const uint32_t floorL = lumFloor;
    for (uint32_t i = 0; i < tex.width * tex.height; i++) {
        uint8_t* p = &tex.pixels[i * 4];
        uint32_t lum = (wr * p[0] + 183u * p[1] + wb * p[2]) >> 8;
        if (lum >= floorL) continue;

        if (lum > 0) {
            uint32_t scaleFP = (floorL << 8) / lum;
            if (scaleFP > kMaxScaleFP) scaleFP = kMaxScaleFP;
            for (int c = 0; c < 3; c++) {
                uint32_t v = (p[c] * scaleFP) >> 8;
                p[c] = v > 255u ? 255u : (uint8_t)v;
            }
            lum = (wr * p[0] + 183u * p[1] + wb * p[2]) >> 8;
        }
        if (lum < floorL) {
            // Near-black tail (or capped scale still short): neutral fill.
            const uint32_t add = floorL - lum;
            for (int c = 0; c < 3; c++) {
                uint32_t v = p[c] + add;
                p[c] = v > 255u ? 255u : (uint8_t)v;
            }
        }
    }
}

// Convert FO4 smoothness/spec mask → Remix roughness by inverting RGB
static void SmoothnessToRoughness(ExtractedTexture& tex)
{
    switch (tex.dxgiFormat) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            InvertUncompressed(tex);
            break;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC5_TYPELESS:
            DecompressBC(tex, BCTransform::InvertRGB);
            break;
        default:
            // BC7/other: can't easily invert, leave as-is
            break;
    }
}

// ---------------------------------------------------------------------------
// Tangent-space normal → hemispherical octahedral encoding
// (Port of NVIDIA's LightspeedOctahedralConverter, MIT license)
// ---------------------------------------------------------------------------
static void ConvertNormalToOctahedral(ExtractedTexture& tex)
{
    // Decompress BC formats to RGBA first
    switch (tex.dxgiFormat) {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_BC5_TYPELESS:
            DecompressBC(tex, BCTransform::NormalReconstructZ);
            break;
        default:
            break;
    }

    // Must be uncompressed RGBA at this point
    if (tex.dxgiFormat != DXGI_FORMAT_R8G8B8A8_UNORM &&
        tex.dxgiFormat != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        tex.dxgiFormat != DXGI_FORMAT_B8G8R8A8_UNORM &&
        tex.dxgiFormat != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        return;

    for (uint32_t i = 0; i < tex.width * tex.height; i++) {
        uint8_t* p = &tex.pixels[i * 4];

        // Decode tangent-space normal from [0,255] to [-1,1]
        float x = p[0] / 255.0f * 2.0f - 1.0f;
        float y = p[1] / 255.0f * 2.0f - 1.0f;
        float z = p[2] / 255.0f * 2.0f - 1.0f;

        // Clamp Z >= 0 (hemispherical — inward normals not supported by Remix)
        if (z < 0.0f) z = -z;

        // Normalize
        float len = sqrtf(x * x + y * y + z * z);
        if (len > 1e-6f) { x /= len; y /= len; z /= len; }
        else { x = 0; y = 0; z = 1; }

        // Octahedral projection: project onto octahedron surface
        float absSum = fabsf(x) + fabsf(y) + fabsf(z);
        float ox = x / absSum;
        float oy = y / absSum;

        // Hemisphere encoding
        float rx = (ox + oy) * 0.5f + 0.5f;
        float ry = (ox - oy) * 0.5f + 0.5f;

        p[0] = (uint8_t)fminf(fmaxf(rx * 255.0f + 0.5f, 0.0f), 255.0f);
        p[1] = (uint8_t)fminf(fmaxf(ry * 255.0f + 0.5f, 0.0f), 255.0f);
        p[2] = 0;
        p[3] = 255;
    }

    tex.dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
}

// ---------------------------------------------------------------------------
// Grayscale-to-palette lookup sampling (2026-07-08 over-dirty fix)
// ---------------------------------------------------------------------------
// FO4 recolors clothing/clutter via a grayscale diffuse + a 2D palette LUT
// (spLookupTexture): the shader samples LUT(u = tonal value, v = vertexColor.x
// palette row). We approximate that as a per-material TINT -- sample the LUT
// once at a representative tonal value for the mesh's palette row and multiply
// it into the grayscale diffuse (same mechanism as the hair fix). The decoded
// LUT mip0 is cached by texture name so repeated samples are free.
namespace {
struct DecodedLut {
    uint32_t w = 0, h = 0;
    bool bgra = false;
    bool srgb = false;   // runtime resource format was *_SRGB (captured
                         // BEFORE decompression -- DecompressBC drops the tag)
    std::vector<uint8_t> rgba;
};
std::unordered_map<uint64_t, DecodedLut> g_lutCache;

// Runtime-format gamma test. The engine promotes color textures to sRGB
// formats at load (runtime diffuses log as BC1/BC3_UNORM_SRGB even though the
// ba2 stores plain UNORM), so the RESOURCE format is what its SRV decodes by.
bool IsSrgbColorFormat(DXGI_FORMAT f)
{
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
    }
}

inline float SrgbToLinear(float c)
{
    return c <= 0.04045f ? c * (1.0f / 12.92f)
                         : std::pow((c + 0.055f) * (1.0f / 1.055f), 2.4f);
}

inline float LinearToSrgb(float c)
{
    c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    return c <= 0.0031308f ? c * 12.92f
                           : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// Hardware-style bilinear fetch from a decoded LUT: texel centers at
// (i+0.5)/N, clamp addressing, per-texel sRGB decode BEFORE filtering (D3D11
// filters sRGB textures in linear space). Matches the palette PS's
// sample_l(t5) with a linear-clamp sampler.
void LutSampleLinear(const DecodedLut& dl, float u, float v, float out[3])
{
    auto fetch = [&](int x, int y, float px[3]) {
        x = x < 0 ? 0 : (x >= (int)dl.w ? (int)dl.w - 1 : x);
        y = y < 0 ? 0 : (y >= (int)dl.h ? (int)dl.h - 1 : y);
        const uint8_t* p = &dl.rgba[((size_t)y * dl.w + x) * 4];
        const float r = (dl.bgra ? p[2] : p[0]) * (1.0f / 255.0f);
        const float g = p[1] * (1.0f / 255.0f);
        const float b = (dl.bgra ? p[0] : p[2]) * (1.0f / 255.0f);
        px[0] = dl.srgb ? SrgbToLinear(r) : r;
        px[1] = dl.srgb ? SrgbToLinear(g) : g;
        px[2] = dl.srgb ? SrgbToLinear(b) : b;
    };
    const float fx = u * dl.w - 0.5f;
    const float fy = v * dl.h - 0.5f;
    const int x0 = (int)std::floor(fx);
    const int y0 = (int)std::floor(fy);
    const float wx = fx - x0, wy = fy - y0;
    float p00[3], p10[3], p01[3], p11[3];
    fetch(x0, y0, p00); fetch(x0 + 1, y0, p10);
    fetch(x0, y0 + 1, p01); fetch(x0 + 1, y0 + 1, p11);
    for (int c = 0; c < 3; ++c) {
        const float top = p00[c] + (p10[c] - p00[c]) * wx;
        const float bot = p01[c] + (p11[c] - p01[c]) * wx;
        out[c] = top + (bot - top) * wy;
    }
}

// Grayscale byte -> palette color, precomputed for all 256 gray values so the
// per-pixel remap is a table lookup. Engine math (palette lighting PS, blob
// @0x7b7e68 in Shaders011.fxp, re-disassembled 2026-07-09):
//   u = pow(diffuseSample.g, 1/2.2)   -- t0 sampled through its (sRGB) SRV,
//                                        then re-gamma'd: u ~= the stored byte
//   albedo.rgb = LUT(u, rowV).rgb     -- REPLACES the gray, per pixel
// rowV is the caller's final V (scale - 1 + pow(vc.x, 1/2.2)). Output bytes
// are sRGB-encoded to match every other diffuse byte this plugin ships.
struct PaletteRemapTable { uint8_t rgb[256][3]; };

bool BuildPaletteRemapTable(const DecodedLut& dl, float rowV, bool diffuseSrgb,
                            PaletteRemapTable& out)
{
    if (!dl.w || !dl.h || dl.rgba.size() < (size_t)dl.w * dl.h * 4) return false;
    for (int gb = 0; gb < 256; ++gb) {
        const float sampled = diffuseSrgb ? SrgbToLinear(gb * (1.0f / 255.0f))
                                          : gb * (1.0f / 255.0f);
        const float u = std::pow(sampled, 1.0f / 2.2f);
        float lin[3];
        LutSampleLinear(dl, u, rowV, lin);
        for (int c = 0; c < 3; ++c)
            out.rgb[gb][c] = (uint8_t)(LinearToSrgb(lin[c]) * 255.0f + 0.5f);
    }
    return true;
}
}

// Per-mip palette remap. Same input contract as TintMultiply_Apply: BC1/BC3/
// BC5 are decompressed first, BC7/other decline (return false -> the caller
// falls back to the flat tint, which also declines on BC7 -- net unchanged).
// Gray is read from GREEN (the engine samples t0 and uses .g -- BC1's
// highest-precision channel); alpha is untouched so cutouts survive.
static bool PaletteRemap_Apply(ExtractedTexture& tex, const PaletteRemapTable& table)
{
    switch (tex.dxgiFormat) {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_BC5_TYPELESS:
            if (!DecompressBC(tex, BCTransform::None)) return false;
            break;
        default:
            break;
    }
    const bool rgba = (tex.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
                       tex.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const bool bgra = (tex.dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM ||
                       tex.dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    if (!rgba && !bgra) return false;
    const uint32_t rI = bgra ? 2u : 0u;
    const uint32_t bI = bgra ? 0u : 2u;
    for (uint32_t i = 0; i < tex.width * tex.height; i++) {
        uint8_t* p = &tex.pixels[i * 4];
        const uint8_t g = p[1];
        p[rI] = table.rgb[g][0];
        p[1]  = table.rgb[g][1];
        p[bI] = table.rgb[g][2];
    }
    return true;
}

// Returns 0=ready (outRGB valid), 1=pending (async readback in flight, retry),
// 2=failed (no/undecodable LUT). u,v in [0,1]: u along width (tonal ramp),
// v along height (palette row).
int BsExtraction::SampleLookupColor(NiTexture* lut, ID3D11Device* device,
                                    float u, float v, uint32_t& outRGB) {
    outRGB = 0xFFFFFFu;
    if (!lut || !device) return 2;
    BSRenderData* rd = lut->rendererData;
    if (!rd || !rd->resource) return 2;
    const char* nm = lut->name.c_str();
    const uint64_t key = FnvHashCombine(FnvHash(nm ? nm : ""), 0x1071ULL);

    auto cit = g_lutCache.find(key);
    if (cit == g_lutCache.end() || cit->second.rgba.empty()) {
        ID3D11Texture2D* t2d = nullptr;
        if (FAILED(rd->resource->QueryInterface(
                __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t2d))) || !t2d)
            return 2;
        std::vector<ExtractedTexture> mips;
        ReadbackStatus st = ReadbackAllMipsAsync(device, t2d, key, mips);
        t2d->Release();
        if (st == ReadbackStatus::Pending) return 1;
        if (st != ReadbackStatus::Ready || mips.empty()) return 2;
        ExtractedTexture m = std::move(mips[0]);
        // Runtime gamma, captured before decompression (which drops the
        // _SRGB tag): the palette remap must decode LUT bytes the same way
        // the engine's SRV does.
        const bool lutSrgb = IsSrgbColorFormat(m.dxgiFormat);
        DecompressBC2(m);
        if (m.dxgiFormat != DXGI_FORMAT_R8G8B8A8_UNORM &&
            m.dxgiFormat != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
            m.dxgiFormat != DXGI_FORMAT_B8G8R8A8_UNORM &&
            m.dxgiFormat != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            if (!DecompressBC(m, BCTransform::None)) {
                DecompressBC7(m);  // BC7-compressed palette/hair LUTs
            }
        }
        const bool rgba = (m.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
                           m.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        const bool bgra = (m.dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM ||
                           m.dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        if ((!rgba && !bgra) || m.width == 0 || m.height == 0 ||
            m.pixels.size() < (size_t)m.width * m.height * 4)
            return 2;
        DecodedLut dl;
        dl.w = m.width; dl.h = m.height; dl.bgra = bgra; dl.srgb = lutSrgb;
        dl.rgba = std::move(m.pixels);
        cit = g_lutCache.emplace(key, std::move(dl)).first;
    }
    const DecodedLut& dl = cit->second;
    if (u < 0.0f) u = 0.0f; if (u > 1.0f) u = 1.0f;
    if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    uint32_t x = (uint32_t)(u * (dl.w - 1) + 0.5f);
    uint32_t y = (uint32_t)(v * (dl.h - 1) + 0.5f);
    if (x >= dl.w) x = dl.w - 1;
    if (y >= dl.h) y = dl.h - 1;
    const uint8_t* p = &dl.rgba[((size_t)y * dl.w + x) * 4];
    const uint32_t r = dl.bgra ? p[2] : p[0];
    const uint32_t g = p[1];
    const uint32_t b = dl.bgra ? p[0] : p[2];
    outRGB = (r << 16) | (g << 8) | b;
    return 0;
}

// ---------------------------------------------------------------------------
// Re-capture-on-approach helper (2026-07-08): current resident width of a
// lighting material's diffuse texture. SEH-guarded because it runs from the
// Tick's upgrade poll against a submitted drawable whose material pointer may
// have been freed (cell detach) between polls. No C++ objects with destructors
// in this frame so __try is legal; ID3D11Texture2D is released by hand.
// ---------------------------------------------------------------------------
// NOTE: returns the EFFECTIVE (MaxTextureDimension-capped) resident width --
// the resolution this plugin would actually extract and upload at.
uint32_t BsExtraction::GetMaterialDiffuseResidentWidth(void* material) {
    if (!material) return 0;
    uint32_t width = 0;
    __try {
        BSLightingShaderMaterialBase* mat =
            reinterpret_cast<BSLightingShaderMaterialBase*>(material);
        NiTexture* tex = mat->spDiffuseTexture;
        if (tex) {
            BSRenderData* rd = tex->rendererData;
            if (rd && rd->resource) {
                ID3D11Texture2D* t2d = nullptr;
                if (SUCCEEDED(rd->resource->QueryInterface(
                        __uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(&t2d))) && t2d) {
                    D3D11_TEXTURE2D_DESC d;
                    t2d->GetDesc(&d);
                    width = d.Width;
                    t2d->Release();
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        width = 0;
    }
    return CapDim(width);
}

// ---------------------------------------------------------------------------
// Async texture conversion workers (2026-07-09 pop-in speed).
//
// Everything downstream of the GPU readback -- BC decompression, octahedral
// normal encoding, smoothness inversion, palette/tint/lum-floor bakes, mip
// packing -- is pure CPU work on plugin-owned buffers, and it dominated the
// resolve loop's cost (a 2048^2 BC chain is ~5.6M texels of scalar decode;
// normals additionally pay per-pixel sqrt + octahedral math). Running it
// inline serialized the whole streaming burst onto the game render thread;
// under the ResolveBudgetMs cap that made pop-in crawl (~180ms of decode
// throughput per second of game time). These workers run the conversion in
// parallel off-thread: ExtractMaterialTexture enqueues a job when the
// readback lands and returns "pending" (0); the resolver's normal retry
// finds the finished result a tick or two later and only pays cache-store +
// Remix upload on the game thread.
//
// Thread contract: jobs carry copies/moves of everything they touch (raw
// mips, palette table, params) -- no engine pointers, no shared caches.
// g_textureCache / g_lutCache stay game-thread-only; the only cross-thread
// state is the queue + done map under g_texConvMutex.
// ---------------------------------------------------------------------------
struct TextureConversionJob {
    uint64_t hash = 0;
    std::vector<ExtractedTexture> mips;   // raw readback output, moved in
    TexturePostProcess postProcess = TexturePostProcess::None;
    uint8_t  minRoughness   = 0;
    uint8_t  albedoLumFloor = 0;
    uint32_t tintRGB        = 0xFFFFFFu;
    PaletteRemapTable palTable = {};
    bool     palTableValid  = false;
    bool     isDiffuseSlot  = false;      // debug-dump routing only
    std::string texName;                  // logging only
    // Persistent disk cache (see the block comment above DiskCacheDir).
    // diskLoadKey != 0: this job LOADS the converted chain from disk
    // instead of converting (mips empty). diskWriteKey != 0: write the
    // converted result to disk after a successful convert.
    uint64_t diskLoadKey  = 0;
    uint64_t diskWriteKey = 0;
};

struct CompletedTextureConversion {
    ExtractedTexture packed;   // pixels empty => conversion dropped/failed
    uint64_t doneFrame = 0;    // for the orphan TTL sweep
};

static std::mutex                       g_texConvMutex;
static std::condition_variable          g_texConvCv;
static std::deque<TextureConversionJob> g_texConvJobs;
static std::unordered_map<uint64_t, CompletedTextureConversion> g_texConvDone;
static std::unordered_set<uint64_t>     g_texConvInflight;  // queued or converting
static std::vector<std::thread>         g_texConvWorkers;
static bool                             g_texConvStop = false;  // guarded by g_texConvMutex

// Raw-mip bytes currently parked in g_texConvJobs (guarded by g_texConvMutex).
// MaxPendingTextureReadbacks caps only CONCURRENT readbacks; slots turn over
// every few ticks while the below-normal-priority workers starve exactly when
// the game saturates all cores (streaming bursts), so without a bound the
// conveyor's output -- hundreds of unique textures x several MiB of raw
// chains -- accumulated outside every budget. Past the cap, jobs are dropped
// and the caller's retry re-runs the readback later (bounded by the readback
// cap), so the queue self-heals once the workers catch up.
static size_t           g_texConvJobsBytes = 0;
static constexpr size_t kMaxTexConvJobsBytes = 256ull << 20;  // 256 MiB

static size_t TexConvJobBytes(const TextureConversionJob& job) {
    size_t n = 0;
    for (const auto& mip : job.mips) n += mip.pixels.size();
    return n;
}

// ---- Persistent disk cache of converted chains (2026-07-13) ----
// The convert stage (BC decode + octahedral/invert/tint/palette bakes)
// costs ~11ms per chain across ~2k unique chains in a fresh area -- the
// pop-in floor once everything upstream went async. The output is
// deterministic for a given content hash (name + resident-resolution fold
// + variant params), so persist it: the first session converts and writes,
// every later one streams the converted bytes back on the same worker
// threads. The file key folds the SOURCE resource's desc on top of the
// content hash, which catches texture-pack swaps that change dims, format
// or mip count; a repaint with identical name AND desc is NOT caught --
// documented with the [Materials] DiskTextureCache ini key (clear
// %LOCALAPPDATA%\FO4Remix\texcache after swapping texture mods).
// Threading: probe runs on the game thread; load/store/sweep run on the
// decode workers. Directory init is call_once-guarded.
struct DiskCacheHeader {
    uint32_t magic;        // 'FRT1'
    uint32_t version;
    uint64_t key;
    uint64_t hash;
    uint32_t width;
    uint32_t height;
    uint32_t mipLevels;
    uint32_t dxgiFormat;
    uint64_t pixelBytes;
};
constexpr uint32_t kDiskCacheMagic   = 0x31545246u;  // 'FRT1'
constexpr uint32_t kDiskCacheVersion = 1;            // bump on pipeline changes

static std::once_flag g_diskCacheDirOnce;
static char g_diskCacheDir[MAX_PATH] = {};
static bool g_diskCacheDirOk = false;
// Keys stat'ed and found absent this session (game thread only): the
// resolver re-probes pending textures every 2 frames, and one stat per
// probe per texture adds up during bursts.
static std::unordered_set<uint64_t> g_diskProbeMissing;

static const char* DiskCacheDir() {
    std::call_once(g_diskCacheDirOnce, [] {
        char base[MAX_PATH] = {};
        if (!GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base))) return;
        char path[MAX_PATH];
        sprintf_s(path, "%s\\FO4Remix", base);
        CreateDirectoryA(path, nullptr);
        sprintf_s(g_diskCacheDir, "%s\\FO4Remix\\texcache", base);
        CreateDirectoryA(g_diskCacheDir, nullptr);
        g_diskCacheDirOk =
            GetFileAttributesA(g_diskCacheDir) != INVALID_FILE_ATTRIBUTES;
        _MESSAGE("FO4RemixPlugin: [TexCache] disk cache %s at %s (cap %u GiB)",
                 g_diskCacheDirOk ? "ready" : "UNAVAILABLE",
                 g_diskCacheDir, g_config.diskTextureCacheGiB);
    });
    return g_diskCacheDirOk ? g_diskCacheDir : nullptr;
}

static bool DiskCachePath(uint64_t key, char out[MAX_PATH]) {
    const char* dir = DiskCacheDir();
    if (!dir) return false;
    sprintf_s(out, MAX_PATH, "%s\\%016llX.tex", dir, (unsigned long long)key);
    return true;
}

static uint64_t DiskCacheKeyFold(uint64_t hash, const D3D11_TEXTURE2D_DESC& sd) {
    uint64_t k = FnvHashCombine(hash, 0xD15C0000u | kDiskCacheVersion);
    k = FnvHashCombine(k, ((uint64_t)sd.Format << 48) |
                          ((uint64_t)sd.MipLevels << 40) |
                          ((uint64_t)sd.Width << 20) | sd.Height);
    return k;
}

static bool DiskCacheLoad(uint64_t key, uint64_t expectHash, ExtractedTexture& out) {
    char path[MAX_PATH];
    if (!DiskCachePath(key, path)) return false;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    DiskCacheHeader h = {};
    DWORD got = 0;
    LARGE_INTEGER fileSize = {};
    if (GetFileSizeEx(f, &fileSize) &&
        ReadFile(f, &h, sizeof(h), &got, nullptr) && got == sizeof(h) &&
        h.magic == kDiskCacheMagic && h.version == kDiskCacheVersion &&
        h.key == key && h.hash == expectHash &&
        h.pixelBytes > 0 && h.pixelBytes < (512ull << 20) &&
        (uint64_t)fileSize.QuadPart == sizeof(h) + h.pixelBytes &&
        h.width > 0 && h.width <= 16384 && h.height > 0 && h.height <= 16384) {
        out.hash       = h.hash;
        out.width      = h.width;
        out.height     = h.height;
        out.mipLevels  = h.mipLevels;
        out.dxgiFormat = (DXGI_FORMAT)h.dxgiFormat;
        out.pixels.resize((size_t)h.pixelBytes);
        DWORD pgot = 0;
        ok = ReadFile(f, out.pixels.data(), (DWORD)h.pixelBytes, &pgot, nullptr) &&
             pgot == (DWORD)h.pixelBytes;
    }
    CloseHandle(f);
    if (!ok) out = {};
    return ok;
}

static void DiskCacheStore(uint64_t key, const ExtractedTexture& packed) {
    char path[MAX_PATH];
    if (!DiskCachePath(key, path)) return;
    char tmp[MAX_PATH];
    sprintf_s(tmp, "%s.tmp", path);
    HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DiskCacheHeader h = {};
    h.magic      = kDiskCacheMagic;
    h.version    = kDiskCacheVersion;
    h.key        = key;
    h.hash       = packed.hash;
    h.width      = packed.width;
    h.height     = packed.height;
    h.mipLevels  = packed.mipLevels;
    h.dxgiFormat = (uint32_t)packed.dxgiFormat;
    h.pixelBytes = packed.pixels.size();
    DWORD wrote = 0;
    const bool ok =
        WriteFile(f, &h, sizeof(h), &wrote, nullptr) && wrote == sizeof(h) &&
        WriteFile(f, packed.pixels.data(), (DWORD)packed.pixels.size(),
                  &wrote, nullptr) && wrote == (DWORD)packed.pixels.size();
    CloseHandle(f);
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
    }
}

// Size-cap sweep, once per session on a worker thread: delete oldest files
// (by last write) until the folder is back under 90% of the cap.
static void DiskCacheSweep() {
    const char* dir = DiskCacheDir();
    if (!dir) return;
    struct Ent { std::string path; uint64_t size; FILETIME wt; };
    std::vector<Ent> ents;
    uint64_t total = 0;
    char pat[MAX_PATH];
    sprintf_s(pat, "%s\\*.tex", dir);
    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Ent e;
        e.path = std::string(dir) + "\\" + fd.cFileName;
        e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.wt   = fd.ftLastWriteTime;
        total += e.size;
        ents.push_back(std::move(e));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    const uint64_t capBytes =
        (uint64_t)(std::max)(1u, g_config.diskTextureCacheGiB) << 30;
    if (total <= capBytes) return;
    std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) {
        return CompareFileTime(&a.wt, &b.wt) < 0;
    });
    const uint64_t target = capBytes / 10 * 9;
    size_t deleted = 0;
    for (const Ent& e : ents) {
        if (total <= target) break;
        if (DeleteFileA(e.path.c_str())) {
            total -= e.size;
            ++deleted;
        }
    }
    _MESSAGE("FO4RemixPlugin: [TexCache] size sweep: deleted %zu oldest files, "
             "now %llu MiB (cap %u GiB)",
             deleted, (unsigned long long)(total >> 20),
             g_config.diskTextureCacheGiB);
}

// Age-sweep completed decodes nobody consumed (drawable evicted mid-decode,
// or the resolution fold changed the hash mid-flight). Each orphan pins a
// fully decoded packed mip chain (~22 MiB at the 2048 cap) OUTSIDE every
// budget (CpuTextureCacheMiB never sees it). Caller holds g_texConvMutex.
static void SweepTexConvDoneLocked() {
    if (g_texConvDone.empty()) return;
    const uint64_t now = Diagnostics::CurrentFrameIndex();
    for (auto it = g_texConvDone.begin(); it != g_texConvDone.end();) {
        if (now - it->second.doneFrame > 600) {
            it = g_texConvDone.erase(it);
        } else {
            ++it;
        }
    }
}

// The conversion tail moved out of ExtractMaterialTexture: pure function of
// the job, runs on a worker thread. Returns the packed mip chain; empty
// pixels signal "drop" (exactly the cases that returned 0 inline before).
static ExtractedTexture ConvertReadbackMips(TextureConversionJob& job)
{
    std::vector<ExtractedTexture>& mips = job.mips;
    const char* texName = job.texName.c_str();
    const TexturePostProcess postProcess = job.postProcess;

    // Per-mip pipeline: BC2 (DXT3) -> RGBA8, then any further BC decompression
    // handled by the post-process stage's BC5/BC1 decoders. Each step operates
    // on a single mip so the existing single-mip-aware functions need no
    // changes.
    // Does this job apply any per-pixel work? Pass-through textures stay in
    // their compressed source format (the runtime decodes BC natively and
    // it is far smaller in VRAM); anything with a transform or bake needs
    // real pixels.
    const bool needsDecodedPixels =
        postProcess != TexturePostProcess::None ||
        job.palTableValid ||
        job.tintRGB != 0xFFFFFFu ||
        job.albedoLumFloor > 0;

    for (auto& mip : mips) {
        DecompressBC2(mip);
        // BC4 has no remixapi format, so it can never pass through
        // compressed -- decode to RGBA8 grayscale up front; the transform
        // stages below then treat it like any uncompressed input.
        if (mip.dxgiFormat == DXGI_FORMAT_BC4_UNORM ||
            mip.dxgiFormat == DXGI_FORMAT_BC4_TYPELESS) {
            DecompressBC(mip, BCTransform::None);
        }
        // BC7 decodes only when pixels are actually needed (2026-07-10).
        // Every transform used to DECLINE on BC7 -- smoothness maps shipped
        // un-inverted, octahedral normal encode never ran, tint/palette/
        // floor bakes fell back to flat approximations.
        if (needsDecodedPixels) {
            DecompressBC7(mip);
        }
    }

    // --- Debug dump: BC3 alpha cutout (right after readback) ---
    // See the black-merge investigation notes; fires for the first N BC3
    // diffuse extractions per process. Counters are atomics now that this
    // runs on worker threads (ticket races would at most skew a filename).
    {
        static std::atomic<int> s_dumpBC3Alpha{0};
        static std::atomic<int> s_logDiffuseFormat{0};
        if (job.isDiffuseSlot) {
            const auto& mip0 = mips[0];
            // Gated on LogTextures + a small cap (was <9999 unconditional:
            // effectively one log line per diffuse decode on the streaming
            // hot path, from worker threads).
            if (g_config.logTextures &&
                s_logDiffuseFormat.fetch_add(1, std::memory_order_relaxed) < 64) {
                _MESSAGE("FO4RemixPlugin: DEBUG diffuse-extract tex='%s' fmt=%u %ux%u mips=%zu pp=%d",
                         texName,
                         (unsigned)mip0.dxgiFormat,
                         mip0.width, mip0.height, mips.size(),
                         (int)postProcess);
            }
            const bool isBC3 = (mip0.dxgiFormat == DXGI_FORMAT_BC3_UNORM ||
                                mip0.dxgiFormat == DXGI_FORMAT_BC3_UNORM_SRGB ||
                                mip0.dxgiFormat == DXGI_FORMAT_BC3_TYPELESS);
            if (isBC3 && mip0.width >= 4 && mip0.height >= 4) {
                const int ticket = s_dumpBC3Alpha.fetch_add(1, std::memory_order_relaxed);
                if (ticket < 30) {
                    const uint32_t numBlocksX = mip0.width / 4;
                    const uint32_t numBlocksY = mip0.height / 4;
                    std::vector<uint8_t> rgba(mip0.width * mip0.height * 4, 0);
                    uint32_t aMin = 255, aMax = 0, aSum = 0, aCount = 0;

                    for (uint32_t by = 0; by < numBlocksY; by++) {
                        for (uint32_t bx = 0; bx < numBlocksX; bx++) {
                            const uint8_t* block = mip0.pixels.data() + (by * numBlocksX + bx) * 16;
                            uint8_t alphas[4][4];
                            DecodeBC3AlphaBlock(block, alphas);
                            for (uint32_t y = 0; y < 4; y++) {
                                for (uint32_t x = 0; x < 4; x++) {
                                    const uint8_t a = alphas[y][x];
                                    const uint32_t px = (by * 4 + y) * mip0.width + (bx * 4 + x);
                                    rgba[px * 4 + 0] = a;
                                    rgba[px * 4 + 1] = a;
                                    rgba[px * 4 + 2] = a;
                                    rgba[px * 4 + 3] = 255;
                                    if (a < aMin) aMin = a;
                                    if (a > aMax) aMax = a;
                                    aSum += a;
                                    aCount++;
                                }
                            }
                        }
                    }

                    char path[256];
                    snprintf(path, sizeof(path), "c:/temp/fo4_debug_bc3_alpha_%d.tga", ticket);
                    DebugDumpTGA(path, rgba.data(), mip0.width, mip0.height);
                    const uint32_t aMean = aCount > 0 ? aSum / aCount : 0;
                    _MESSAGE("FO4RemixPlugin: DEBUG dumped BC3 alpha -> %s tex='%s' min=%u max=%u mean=%u",
                             path, texName, aMin, aMax, aMean);
                }
            }
        }
    }

    // --- Debug dump: diffuse control (no post-processing) ---
    if (postProcess == TexturePostProcess::None) {
        static std::atomic<int> s_dumpDiffuse{0};
        if (s_dumpDiffuse.load(std::memory_order_relaxed) < 2) {
            const int ticket = s_dumpDiffuse.fetch_add(1, std::memory_order_relaxed);
            if (ticket < 2) {
                ExtractedTexture tmp = mips[0];
                bool isBC1tmp = (tmp.dxgiFormat == DXGI_FORMAT_BC1_UNORM ||
                                 tmp.dxgiFormat == DXGI_FORMAT_BC1_UNORM_SRGB ||
                                 tmp.dxgiFormat == DXGI_FORMAT_BC1_TYPELESS);
                if (isBC1tmp) {
                    DecompressBC(tmp, BCTransform::None);
                }
                if (tmp.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
                    tmp.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                    char path[256];
                    snprintf(path, sizeof(path), "c:/temp/fo4_debug_diffuse_%d.tga", ticket);
                    DebugDumpTGA(path, tmp.pixels.data(), tmp.width, tmp.height);
                    _MESSAGE("FO4RemixPlugin: DEBUG dumped diffuse -> %s (%ux%u)", path, tmp.width, tmp.height);
                }
            }
        }
    }

    // --- Debug dump: raw BC5 decode (before post-processing) ---
    {
        static std::atomic<int> s_dumpNormalRaw{0}, s_dumpRoughnessRaw{0};
        int ticket = -1;
        const char* rawName = nullptr;
        if (postProcess == TexturePostProcess::Octahedral &&
            s_dumpNormalRaw.load(std::memory_order_relaxed) < 3) {
            ticket = s_dumpNormalRaw.fetch_add(1, std::memory_order_relaxed);
            rawName = "c:/temp/fo4_debug_normal_raw_%d.tga";
        } else if (postProcess == TexturePostProcess::InvertRGB &&
                   s_dumpRoughnessRaw.load(std::memory_order_relaxed) < 3) {
            ticket = s_dumpRoughnessRaw.fetch_add(1, std::memory_order_relaxed);
            rawName = "c:/temp/fo4_debug_roughness_raw_%d.tga";
        }
        if (rawName && ticket >= 0 && ticket < 3) {
            ExtractedTexture tmp = mips[0]; // copy mip 0
            bool isBC5tmp = (tmp.dxgiFormat == DXGI_FORMAT_BC5_UNORM ||
                             tmp.dxgiFormat == DXGI_FORMAT_BC5_SNORM ||
                             tmp.dxgiFormat == DXGI_FORMAT_BC5_TYPELESS);
            if (isBC5tmp) {
                DecompressBC(tmp, BCTransform::NormalReconstructZ);
            }
            if (tmp.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
                tmp.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                char path[256];
                snprintf(path, sizeof(path), rawName, ticket);
                DebugDumpTGA(path, tmp.pixels.data(), tmp.width, tmp.height);
                _MESSAGE("FO4RemixPlugin: DEBUG dumped raw decode -> %s (%ux%u)", path, tmp.width, tmp.height);
            }
        }
    }

    // Apply post-processing per-mip. SmoothnessToRoughness and
    // ConvertNormalToOctahedral both run BC decode internally if needed and
    // emit RGBA8, so mips that started in different BC formats end up in a
    // common output format -- which matters because we concatenate them
    // into a single packed buffer below.
    for (auto& mip : mips) {
        if (postProcess == TexturePostProcess::InvertRGB) {
            SmoothnessToRoughness(mip);
            // Clamp roughness for decal surfaces. Bethesda's smoothness map is
            // often set to "very smooth" on decals; after InvertRGB that
            // becomes near-zero roughness (mirror) which the path tracer
            // renders literally. Vanilla DX11 hides this with specular
            // highlights. Clamping the RGB channels (which carry roughness
            // after our inversion) to >= minRoughness prevents mirror surfaces
            // while preserving relative variation. Only applied on the BC1/
            // BC3/BC5 -> RGBA8 path; BC7 / unknown formats fall through
            // SmoothnessToRoughness as-is and are not clamped here (acceptable
            // since they're rare).
            if (job.minRoughness > 0 &&
                (mip.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
                 mip.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                 mip.dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM ||
                 mip.dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) {
                for (uint32_t i = 0; i < mip.width * mip.height; i++) {
                    if (mip.pixels[i * 4 + 0] < job.minRoughness) mip.pixels[i * 4 + 0] = job.minRoughness;
                    if (mip.pixels[i * 4 + 1] < job.minRoughness) mip.pixels[i * 4 + 1] = job.minRoughness;
                    if (mip.pixels[i * 4 + 2] < job.minRoughness) mip.pixels[i * 4 + 2] = job.minRoughness;
                }
            }
        } else if (postProcess == TexturePostProcess::Octahedral) {
            ConvertNormalToOctahedral(mip);
        } else if (postProcess == TexturePostProcess::DiffuseAlphaFromLuminance) {
            DiffuseAlphaFromLuminance_Apply(mip, /*forceForBC3=*/false);
        } else if (postProcess == TexturePostProcess::DiffuseAlphaFromLuminanceForceBC3) {
            DiffuseAlphaFromLuminance_Apply(mip, /*forceForBC3=*/true);
        } else if (postProcess == TexturePostProcess::ForceRGBA8) {
            // Decompress-only: BC1/BC3 (incl. SRGB) -> RGBA8, authored alpha
            // kept. Non-BC inputs pass through untouched.
            DecompressBC(mip, BCTransform::None);
        }
        // Grayscale-to-palette remap REPLACES the gray RGB with the LUT row
        // (engine-exact; see PaletteRemap_Apply). Composes at the tint slot:
        // AFTER the alpha stage (cutouts survive), BEFORE the luminance
        // floor. When it declines (BC7 input), the flat-tint fallback below
        // applies instead.
        bool remapped = false;
        if (job.palTableValid) {
            remapped = PaletteRemap_Apply(mip, job.palTable);
            // The remap table's output bytes are ALWAYS sRGB-encoded
            // (BuildPaletteRemapTable re-encodes with LinearToSrgb), so the
            // tag must say so even when the grayscale SOURCE was a plain
            // UNORM mask -- otherwise Remix linearizes already-gamma bytes
            // and palette surfaces come out over-dark.
            if (remapped && mip.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM) {
                mip.dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            }
        }
        // Skin/hair tint multiply composes AFTER the alpha stage (alpha
        // untouched -- hair cutouts survive) and BEFORE the luminance floor.
        if (!remapped && job.tintRGB != 0xFFFFFFu) {
            TintMultiply_Apply(mip, job.tintRGB);
        }
        // Metal albedo luminance floor composes AFTER the alpha stage so
        // synthesized / authored cutout alpha is preserved while dark RGB is
        // lifted (hue-preserving; see AlbedoLumFloor_Apply).
        if (job.albedoLumFloor > 0) {
            AlbedoLumFloor_Apply(mip, job.albedoLumFloor);
        }
    }

    // --- Debug dump: after post-processing (mip 0 only) ---
    {
        static std::atomic<int> s_dumpNormalPost{0}, s_dumpRoughnessPost{0};
        int ticket = -1;
        const char* postName = nullptr;
        if (postProcess == TexturePostProcess::Octahedral &&
            s_dumpNormalPost.load(std::memory_order_relaxed) < 3) {
            ticket = s_dumpNormalPost.fetch_add(1, std::memory_order_relaxed);
            postName = "c:/temp/fo4_debug_normal_post_%d.tga";
        } else if (postProcess == TexturePostProcess::InvertRGB &&
                   s_dumpRoughnessPost.load(std::memory_order_relaxed) < 3) {
            ticket = s_dumpRoughnessPost.fetch_add(1, std::memory_order_relaxed);
            postName = "c:/temp/fo4_debug_roughness_post_%d.tga";
        }
        if (postName && ticket >= 0 && ticket < 3 &&
            (mips[0].dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
             mips[0].dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) {
            char path[256];
            snprintf(path, sizeof(path), postName, ticket);
            DebugDumpTGA(path, mips[0].pixels.data(), mips[0].width, mips[0].height);
            _MESSAGE("FO4RemixPlugin: DEBUG dumped post-process -> %s (%ux%u)", path, mips[0].width, mips[0].height);
        }
    }

    // --- Sanity: every mip must share the same final dxgiFormat. The pipeline
    // above produces RGBA8 for any BC source; uncompressed sources stay in
    // their original (BGRA8 / RGBA8) format. Mixed formats across mips would
    // mean a mid-chain decompression diverged -- bail rather than ship a
    // malformed packed buffer that dxvk-remix would interpret as garbage.
    DXGI_FORMAT chainFmt = mips[0].dxgiFormat;
    for (auto& mip : mips) {
        if (mip.dxgiFormat != chainFmt) {
            _MESSAGE("FO4RemixPlugin: ExtractMaterialTexture - mip format mismatch "
                     "(mip0=%u midmip=%u) for hash=0x%016llX, dropping",
                     (unsigned)chainFmt, (unsigned)mip.dxgiFormat, job.hash);
            return {};
        }
    }

    // Concatenate all mips into a single packed buffer. Remix expects the
    // mip chain tightly packed, mip0 first, no padding.
    ExtractedTexture extracted;
    extracted.hash       = job.hash;
    extracted.width      = mips[0].width;
    extracted.height     = mips[0].height;
    extracted.dxgiFormat = chainFmt;
    extracted.mipLevels  = (uint32_t)mips.size();

    size_t total = 0;
    for (const auto& mip : mips) total += mip.pixels.size();
    extracted.pixels.reserve(total);
    for (auto& mip : mips) {
        extracted.pixels.insert(extracted.pixels.end(),
                                mip.pixels.begin(), mip.pixels.end());
    }
    return extracted;
}

static void TextureConversionWorkerMain()
{
    // One-time disk-cache size sweep, off the game thread.
    if (g_config.diskTextureCache) {
        static std::once_flag s_diskSweepOnce;
        std::call_once(s_diskSweepOnce, [] { DiskCacheSweep(); });
    }
    for (;;) {
        TextureConversionJob job;
        {
            std::unique_lock<std::mutex> lk(g_texConvMutex);
            g_texConvCv.wait(lk, [] { return g_texConvStop || !g_texConvJobs.empty(); });
            if (g_texConvStop) return;   // queued jobs dropped on shutdown, by design
            job = std::move(g_texConvJobs.front());
            g_texConvJobs.pop_front();
            const size_t jobBytes = TexConvJobBytes(job);
            g_texConvJobsBytes = g_texConvJobsBytes > jobBytes
                ? g_texConvJobsBytes - jobBytes : 0;
        }
        // Disk-cache load job: no convert, just stream the chain back.
        if (job.diskLoadKey != 0) {
            ExtractedTexture packed;
            if (DiskCacheLoad(job.diskLoadKey, job.hash, packed)) {
                static std::atomic<int> sHitLogs{0};
                const int n = sHitLogs.fetch_add(1, std::memory_order_relaxed);
                if (n < 8) {
                    _MESSAGE("FO4RemixPlugin: [TexCache] disk hit #%d "
                             "hash=0x%016llX %ux%u mips=%u (%zu KiB)",
                             n, (unsigned long long)job.hash,
                             packed.width, packed.height, packed.mipLevels,
                             packed.pixels.size() >> 10);
                }
                std::lock_guard<std::mutex> lk(g_texConvMutex);
                g_texConvDone[job.hash] = { std::move(packed),
                                            Diagnostics::CurrentFrameIndex() };
                g_texConvInflight.erase(job.hash);
            } else {
                // Corrupt/stale file: delete it and post NOTHING -- an empty
                // done entry would negative-cache the hash. With the file
                // gone the resolver's next probe misses and takes the normal
                // readback + convert path.
                char path[MAX_PATH];
                if (DiskCachePath(job.diskLoadKey, path)) DeleteFileA(path);
                _MESSAGE("FO4RemixPlugin: [TexCache] BAD cache file for "
                         "hash=0x%016llX -- deleted, re-converting",
                         (unsigned long long)job.hash);
                std::lock_guard<std::mutex> lk(g_texConvMutex);
                g_texConvInflight.erase(job.hash);
            }
            continue;
        }
        // Exception fence (2026-07-12): this is the most allocation-heavy
        // code the plugin owns (mip-chain vectors, BC7 RGBA expansion,
        // palette remaps) and it used to run BARE on the worker -- one
        // bad_alloc/length_error escaped the thread proc, hit
        // std::terminate, and fast-failed the whole process (WER 0xc0000409
        // at abort in this DLL). A failed decode now just drops the
        // texture: an empty result routes through the existing
        // conversion-dropped path (negative cache) downstream.
        ExtractedTexture packed;
        try {
            packed = ConvertReadbackMips(job);
        } catch (const std::exception& e) {
            static std::atomic<int> sConvCatch{0};
            const int n = sConvCatch.fetch_add(1, std::memory_order_relaxed);
            if (n < 16) {
                _MESSAGE("FO4RemixPlugin: [TexConvert] worker C++ exception #%d "
                         "hash=0x%016llX what=%s -- texture dropped",
                         n, (unsigned long long)job.hash, e.what());
            }
            packed = {};
        } catch (...) {
            static std::atomic<int> sConvCatchU{0};
            const int n = sConvCatchU.fetch_add(1, std::memory_order_relaxed);
            if (n < 16) {
                _MESSAGE("FO4RemixPlugin: [TexConvert] worker unknown C++ exception "
                         "#%d hash=0x%016llX -- texture dropped",
                         n, (unsigned long long)job.hash);
            }
            packed = {};
        }
        // Persist the converted chain so later sessions skip the readback +
        // convert entirely (see DiskCacheDir). Written before publishing so
        // a consumer evicting the CPU-cache copy can't race the write.
        if (job.diskWriteKey != 0 && g_config.diskTextureCache &&
            !packed.pixels.empty()) {
            DiskCacheStore(job.diskWriteKey, packed);
        }
        {
            std::lock_guard<std::mutex> lk(g_texConvMutex);
            g_texConvDone[job.hash] = { std::move(packed),
                                        Diagnostics::CurrentFrameIndex() };
            g_texConvInflight.erase(job.hash);
        }
    }
}

static void EnqueueTextureConversion(TextureConversionJob&& job)
{
    std::lock_guard<std::mutex> lk(g_texConvMutex);
    if (g_texConvStop) return;

    // Orphan sweep (unconditional -- the old size()>64 gate meant up to 64
    // orphans, ~1.4 GiB worst case, survived indefinitely once a streaming
    // burst ended, and even past 64 only a fresh enqueue could reap them).
    // The Tick sweep cadence also calls this via SweepTextureQueues.
    SweepTexConvDoneLocked();

    // Queue byte bound (see g_texConvJobsBytes): drop rather than park
    // unbounded raw chains; the caller keeps reporting pending and its
    // retry re-runs the readback once the workers have drained the queue.
    const size_t jobBytes = TexConvJobBytes(job);
    if (g_texConvJobsBytes + jobBytes > kMaxTexConvJobsBytes) {
        static std::atomic<int> sDropLogs{0};
        const int n = sDropLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 16) {
            _MESSAGE("FO4RemixPlugin: [TexConvert] queue full (%zu MiB), "
                     "dropping job #%d hash=0x%016llX (%zu KiB) -- retry "
                     "re-reads it back",
                     g_texConvJobsBytes >> 20, n,
                     (unsigned long long)job.hash, jobBytes >> 10);
        }
        return;
    }

    g_texConvJobsBytes += jobBytes;
    g_texConvInflight.insert(job.hash);
    g_texConvJobs.push_back(std::move(job));

    if (g_texConvWorkers.empty()) {
        // Pool size = [Performance] DecodeWorkerPercent of logical cores,
        // clamped to [1, cores - 1]: at least one worker (an empty pool
        // would strand every queued texture) and at least one core left
        // untouched. hardware_concurrency() can return 0 on exotic setups;
        // treat that as 4 cores.
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        unsigned count = (hw * g_config.decodeWorkerPercent + 50u) / 100u;
        if (count < 1) count = 1;
        if (hw > 1 && count > hw - 1) count = hw - 1;
        // Bandwidth ceiling: decode is DRAM-bound; more workers past this
        // point contend with the game instead of adding throughput
        // (8 workers measured slower than 4 on a 32-logical-core machine).
        if (g_config.decodeWorkerMax > 0 && count > g_config.decodeWorkerMax) {
            count = g_config.decodeWorkerMax;
        }
        for (unsigned i = 0; i < count; ++i) {
            g_texConvWorkers.emplace_back([] {
                // Below-normal priority: decode throughput matters, but never
                // at the cost of the game's own render/worker threads when the
                // CPU is saturated.
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                TextureConversionWorkerMain();
            });
        }
        _MESSAGE("FO4RemixPlugin: [TexConvert] started %u texture decode workers "
                 "(%u%% of %u logical cores)",
                 count, g_config.decodeWorkerPercent, hw);
    }
    g_texConvCv.notify_one();
}

void BsExtraction::StopTextureConversionWorkers()
{
    {
        std::lock_guard<std::mutex> lk(g_texConvMutex);
        if (g_texConvWorkers.empty()) return;
        g_texConvStop = true;
    }
    g_texConvCv.notify_all();
    for (auto& t : g_texConvWorkers) {
        if (t.joinable()) t.join();
    }
    g_texConvWorkers.clear();
    _MESSAGE("FO4RemixPlugin: [TexConvert] texture decode workers stopped");
}

void BsExtraction::SweepTextureQueues()
{
    std::lock_guard<std::mutex> lk(g_texConvMutex);
    SweepTexConvDoneLocked();
}

// ---------------------------------------------------------------------------
// Generic texture extraction from any NiTexture slot
// ---------------------------------------------------------------------------
uint64_t BsExtraction::ExtractMaterialTexture(NiTexture* tex, const char* slotName,
                                       ID3D11Device* device,
                                       TextureSupply& newTextures,
                                       TexturePostProcess postProcess,
                                       uint8_t minRoughness,
                                       uint8_t albedoLumFloor,
                                       uint32_t tintRGB,
                                       NiTexture* paletteLut,
                                       float paletteRowV,
                                       bool* outPending,
                                       bool supplyPixels)
{
    // Default: not pending. Every in-flight exit below sets it true; plain
    // failures (missing resource, unsupported format) leave it false so the
    // caller can distinguish "retry" from "this slot doesn't exist".
    if (outPending) *outPending = false;
    // Clamp the palette row once: it is both the cache-key quantization and
    // the LUT V coordinate (hardware clamp addressing).
    if (paletteRowV < 0.0f) paletteRowV = 0.0f;
    if (paletteRowV > 1.0f) paletteRowV = 1.0f;
    // Early-bail diagnostic: log when diffuse texture extraction fails because
    // the D3D resource isn't available. Helps distinguish "decal not extracted
    // because path doesn't reach here" vs "decal not extracted because the
    // NiTexture has no backing D3D resource".
    auto bailLog = [&](const char* reason) {
        static int s_bailCount = 0;
        const bool isDiffuseSlot = slotName && std::strcmp(slotName, "diffuse") == 0;
        if (isDiffuseSlot && s_bailCount < 50) {
            _MESSAGE("FO4RemixPlugin: DEBUG diffuse-extract BAIL reason='%s' tex_ptr=%p",
                     reason, (void*)tex);
            s_bailCount++;
        }
    };

    if (!tex) { bailLog("null NiTexture"); return 0; }

    BSRenderData* renderData = tex->rendererData;
    if (!renderData) { bailLog("null renderData"); return 0; }

    ID3D11Resource* resource = renderData->resource;
    if (!resource) { bailLog("null resource"); return 0; }

    // Stable hash from texture name so hashes are consistent across runs
    const char* texName = tex->name.c_str();
    uint64_t hash = FnvHash(texName ? texName : "");
    // Include post-processing mode so variants don't collide
    if (postProcess == TexturePostProcess::InvertRGB)                         hash = FnvHashCombine(hash, 1);
    if (postProcess == TexturePostProcess::Octahedral)                        hash = FnvHashCombine(hash, 2);
    if (postProcess == TexturePostProcess::DiffuseAlphaFromLuminance)         hash = FnvHashCombine(hash, 3);
    if (postProcess == TexturePostProcess::DiffuseAlphaFromLuminanceForceBC3) hash = FnvHashCombine(hash, 5);
    if (postProcess == TexturePostProcess::ForceRGBA8)                        hash = FnvHashCombine(hash, 6);
    // Fold roughness clamp into the cache key so a roughness texture extracted
    // for a decal (clamped) and the same texture for a non-decal (un-clamped)
    // hash differently. Otherwise the first-seen variant would poison the
    // cache for the other case.
    if (minRoughness > 0)                                             hash = FnvHashCombine(hash, 4 | (uint64_t)minRoughness << 8);
    // Fold the metal albedo luminance floor into the cache key: metal and
    // non-metal materials sharing a source texture need distinct lifted/
    // unlifted variants (same poisoning argument as the roughness clamp).
    if (albedoLumFloor > 0)                                           hash = FnvHashCombine(hash, 7 | (uint64_t)albedoLumFloor << 8);
    // Tint variants: differently-tinted NPCs sharing one grayscale source
    // (every hair color uses HairShortXXGrayscale_d) must not collide.
    if (tintRGB != 0xFFFFFFu)                                         hash = FnvHashCombine(hash, 8 | (uint64_t)tintRGB << 8);
    // Palette-remap variants: meshes sharing one grayscale source but
    // selecting different LUTs or different palette rows must not collide.
    // Row quantized to the byte grid the remap table is built on.
    if (paletteLut) {
        const char* lutName = paletteLut->name.c_str();
        hash = FnvHashCombine(hash, FnvHash(lutName ? lutName : ""));
        hash = FnvHashCombine(hash, 10 | ((uint64_t)(uint8_t)(paletteRowV * 255.0f + 0.5f) << 8));
    }
    // Resident RESOLUTION variant (2026-07-08 re-capture-on-approach): FO4
    // streams textures in progressively, so `resource` is whatever mip level
    // is currently resident -- often reduced (1/2, 1/4) when the object first
    // appears at distance. Folding the resident width/height into the cache key
    // means that when the engine later streams a sharper mip in, the SAME
    // texture NAME hashes to a NEW key -> cache miss -> full-res re-extract. The
    // Tick's upgrade poll (semantic_capture.cpp) releases + re-resolves the
    // affected drawables so the sharper texture actually reaches Remix. Without
    // this the name-only key locked the first (blurry) capture for the whole
    // session (blobby textures, washed-out detail, flat NPC skin -- 2048->512).
    // Non-Texture2D resources (cubemaps) leave the fold off (name-based, as
    // before); nothing streams them progressively. Opt-in ([Materials]
    // TextureUpgradeOnApproach): default OFF keeps the name-only key (and thus
    // byte-identical caching + no upgrade churn).
    const uint64_t preResolutionHash = hash;  // for superseded-variant eviction
    if (g_config.textureUpgradeOnApproach) {
        ID3D11Texture2D* t2d = nullptr;
        if (SUCCEEDED(resource->QueryInterface(
                __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t2d))) && t2d) {
            D3D11_TEXTURE2D_DESC rd; t2d->GetDesc(&rd);
            t2d->Release();
            // Fold the CAPPED dims: two resident sizes that clamp to the
            // same upload resolution must share one cache key, or the same
            // capped pixels would upload twice under different hashes.
            if (rd.Width) hash = FnvHashCombine(
                hash, 0xA00000000ULL | ((uint64_t)CapDim(rd.Width) << 16) |
                      (CapDim(rd.Height) & 0xFFFFu));
        }
    }

    // Check cache first. A hit normally returns just the hash -- the pixels
    // were already handed to SubmitDrawable when the texture was first
    // extracted. But the Remix-side handle can be destroyed while this cache
    // entry lives on: the PreLoadGame release wave drops every drawable's
    // refcounts (observed 2026-07-02: 547 -> 6 texture handles across a save
    // load), and the orphan LRU sweep reaps refcount-zero handles mid-session.
    // If we return hash-only in that state, SubmitDrawable's diffuse-loaded
    // gate fails silently and the drawable can NEVER submit again (the
    // "player's area empty after loading a save" + submitFailed-retry-storm
    // symptom). Re-supply the cached pixels so SubmitDrawable recreates the
    // handle; the supply is a shared_ptr alias of the cache entry (refcount,
    // not a pixel copy), taken only in the handle-missing case.
    if (g_texKnownBad.count(hash)) return 0;  // decode permanently dropped

    auto it = g_textureCache.find(hash);
    if (it != g_textureCache.end()) {
        it->second.touch      = ++g_textureCacheTouchCounter;
        it->second.touchFrame = Diagnostics::CurrentFrameIndex();
        // Probe mode never copies: the pixels are re-supplied by the caller's
        // supplying pass on the attempt that actually submits.
        if (supplyPixels && !RemixRenderer::HasTextureHandle(hash)) {
            newTextures.push_back(it->second.tex);
        }
        return hash;
    }

    // Async conversion rendezvous: a prior attempt's readback completed and
    // its decode ran (or is running) on a worker thread. Consume the result
    // here, or keep reporting "pending". Checked BEFORE the readback path so
    // a completed decode can't re-trigger a fresh GPU readback for the same
    // hash.
    {
        ExtractedTexture packed;
        bool havePacked = false;
        {
            std::lock_guard<std::mutex> lk(g_texConvMutex);
            auto dit = g_texConvDone.find(hash);
            if (dit != g_texConvDone.end()) {
                packed = std::move(dit->second.packed);
                g_texConvDone.erase(dit);
                havePacked = true;
            } else if (g_texConvInflight.count(hash)) {
                if (outPending) *outPending = true;
                return 0;  // decode in flight; resolver retries next tick
            }
        }
        if (havePacked) {
            if (packed.pixels.empty()) {
                // Conversion dropped (format mismatch). Remember it so the
                // resolver's retries short-circuit here instead of re-running
                // the readback + decode forever.
                g_texKnownBad.insert(hash);
                return 0;
            }

            if (g_config.logTextures) {
                _MESSAGE("FO4RemixPlugin: Extracted %s texture \"%s\" %ux%u mips=%u fmt=%u hash=0x%016llX%s",
                         slotName, texName ? texName : "<null>",
                         packed.width, packed.height, packed.mipLevels,
                         (unsigned)packed.dxgiFormat, hash,
                         postProcess == TexturePostProcess::InvertRGB ? " (inverted)" :
                         postProcess == TexturePostProcess::Octahedral ? " (octahedral)" : "");
            }

            // Evict the superseded smaller resolution variant (see
            // g_texResVariantIndex): keeps the CPU pixel cache at ~one variant
            // per texture instead of accumulating every mip level the streamer
            // ever had resident. Only when the resolution fold is active (hash
            // differs from the pre-fold hash); only supersede strictly-smaller
            // variants.
            if (hash != preResolutionHash) {
                auto [vit, inserted] = g_texResVariantIndex.try_emplace(
                    preResolutionHash, hash, packed.width);
                if (!inserted && vit->second.first != hash) {
                    if (packed.width > vit->second.second) {
                        TextureCacheErase(vit->second.first);
                        vit->second = { hash, packed.width };
                    }
                }
            }

            // One shared chain serves both the cache and (when supplying)
            // the submit list -- the old path deep-copied the ~22MB chain
            // here so the cache could keep one while the submit list took
            // the other.
            const auto& cached = TextureCacheInsert(hash, std::move(packed));
            if (supplyPixels) {
                newTextures.push_back(cached);
            }
            TextureCacheEnforceBudget();
            return hash;
        }
    }

    // QueryInterface to ID3D11Texture2D
    ID3D11Texture2D* tex2D = nullptr;
    HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                          reinterpret_cast<void**>(&tex2D));
    if (FAILED(hr) || !tex2D) return 0;

    // Persistent disk cache probe: a prior session already converted this
    // exact chain -- stream it back on a worker instead of paying readback
    // + convert. Probed at most once per hash per session
    // (g_diskProbeMissing suppresses the per-retry stat; the resolver polls
    // pending textures every 2 frames).
    D3D11_TEXTURE2D_DESC srcDescForCache = {};
    tex2D->GetDesc(&srcDescForCache);
    uint64_t diskKey = 0;
    if (g_config.diskTextureCache) {
        diskKey = DiskCacheKeyFold(hash, srcDescForCache);
        if (!g_diskProbeMissing.count(hash)) {
            char cachePath[MAX_PATH];
            if (DiskCachePath(diskKey, cachePath) &&
                GetFileAttributesA(cachePath) != INVALID_FILE_ATTRIBUTES) {
                TextureConversionJob loadJob;
                loadJob.hash        = hash;
                loadJob.diskLoadKey = diskKey;
                loadJob.texName     = texName ? texName : "";
                EnqueueTextureConversion(std::move(loadJob));
                tex2D->Release();
                if (outPending) *outPending = true;
                return 0;
            }
            g_diskProbeMissing.insert(hash);
        }
    }

    // Read every mip from the source D3D texture. Each entry is a single-mip
    // ExtractedTexture so we can reuse the existing per-mip decompression and
    // post-process functions unchanged. They get concatenated into a packed
    // mip chain at the end.
    //
    // Async: the first attempt queues GPU copies and returns Pending; the
    // resolver's retry loop calls back next tick(s) until the copies have
    // landed. Returning 0 here is the existing "no texture yet" signal the
    // resolver already handles by retrying -- no stalls on the game thread.
    std::vector<ExtractedTexture> mips;
    ReadbackStatus rbStatus = ReadbackAllMipsAsync(device, tex2D, hash, mips);
    tex2D->Release();

    if (rbStatus == ReadbackStatus::Pending) {
        if (outPending) *outPending = true;
        return 0;
    }
    if (rbStatus != ReadbackStatus::Ready || mips.empty()) return 0;

    // Readback landed: package the conversion (BC decode + transforms + mip
    // packing) for the worker pool and keep reporting "pending" (0). The
    // resolver's normal retry finds the finished result in the rendezvous
    // block above a tick or two later. Everything the job needs is copied or
    // moved -- no engine pointers cross the thread boundary.
    TextureConversionJob job;
    job.hash           = hash;
    job.postProcess    = postProcess;
    job.minRoughness   = minRoughness;
    job.albedoLumFloor = albedoLumFloor;
    job.tintRGB        = tintRGB;
    job.isDiffuseSlot  = slotName && std::strcmp(slotName, "diffuse") == 0;
    job.texName        = texName ? texName : "";
    job.diskWriteKey   = diskKey;  // 0 when the disk cache is off

    // Runtime gamma of the source resource, captured before any decompression
    // (DecompressBC drops the _SRGB tag). Drives the palette remap's U decode:
    // the engine samples the grayscale diffuse through THIS format's SRV.
    const bool srcIsSrgb = IsSrgbColorFormat(mips[0].dxgiFormat);

    // Grayscale-to-palette remap table: built here on the game thread (it
    // reads g_lutCache, which is game-thread-only) and copied into the job.
    // Requires the LUT to be decoded already -- the resolver's
    // SampleLookupColor pending-gate guarantees that before the diffuse
    // extraction runs.
    if (paletteLut) {
        const char* lutName = paletteLut->name.c_str();
        const uint64_t lutKey = FnvHashCombine(FnvHash(lutName ? lutName : ""), 0x1071ULL);
        auto lit = g_lutCache.find(lutKey);
        if (lit != g_lutCache.end() && !lit->second.rgba.empty()) {
            job.palTableValid = BuildPaletteRemapTable(lit->second, paletteRowV,
                                                       srcIsSrgb, job.palTable);
        }
    }

    job.mips = std::move(mips);
    EnqueueTextureConversion(std::move(job));
    if (outPending) *outPending = true;  // decode just queued; retry consumes it
    return 0;
}



// Get the BSLightingShaderMaterialBase from a shape, or nullptr
BSLightingShaderMaterialBase* BsExtraction::GetLightingMaterial(BSTriShape* shape)
{
    if (!shape) return nullptr;
    NiProperty* prop = shape->shaderProperty;
    if (!prop) return nullptr;
    BSShaderProperty* shaderProp = static_cast<BSShaderProperty*>(prop);
    BSShaderMaterial* material = shaderProp->shaderMaterial;
    if (!material || material->GetFeature() != 2) return nullptr;
    return static_cast<BSLightingShaderMaterialBase*>(material);
}

// Extract emissive data from a shape's shader property and material
void BsExtraction::ExtractEmissiveData(BSTriShape* shape, BSLightingShaderMaterialBase* lightingMat,
                                ID3D11Device* device, TextureSupply& newTextures,
                                uint64_t& outTexHash, float& outR, float& outG, float& outB, float& outIntensity,
                                bool* outPending, bool supplyPixels)
{
    outTexHash = 0;
    outR = outG = outB = 0.0f;
    outIntensity = 0.0f;
    if (outPending) *outPending = false;

    if (!shape || !lightingMat) return;

    // HAIR GUARD (2026-07-08 "bright teal hair"): FO4 NPC hair is genuinely a
    // BSLightingShaderMaterialGlowmap (GetType()==2) -- but it is NOT emissive.
    // The engine repurposes this class for the grayscale-to-palette hair path:
    // pEmissiveColor holds the per-NPC HAIR TINT (consumed as a diffuse tint by
    // the resolver's [Tint] block), and the +0xC0 glow slot / a set EmitColor
    // flag are not real light -- extracting either made hair glow in false
    // color. Discriminate on the shared HairColor gradient LUT bound in
    // spLookupTexture: the F4SE kShaderFlags_Hair bit is wrong for this build
    // (it tags landscape, never real hair -- see lighting_static.cpp), but the
    // LUT name is build-independent (one such texture game-wide, hair-only).
    {
        NiTexture* lut = lightingMat->spLookupTexture;
        const char* ln = (lut && lut->name.c_str()) ? lut->name.c_str() : nullptr;
        if (ln) {
            // case-insensitive substring "haircolor"
            static const char kNeedle[] = "haircolor";
            for (const char* p = ln; *p; ++p) {
                size_t i = 0;
                while (kNeedle[i] && p[i] &&
                       (char)std::tolower((unsigned char)p[i]) == kNeedle[i]) ++i;
                if (!kNeedle[i]) return;  // hair: skip all emissive
            }
        }
    }

    // 1. Glow map texture from BSLightingShaderMaterialGlowmap. RTTI-gated so
    // any other class that happens to report type 2 can't be misread as a
    // glow-slot owner.
    if (g_config.emissiveGlowMapsEnabled &&
        lightingMat->GetType() == BSLightingShaderMaterialBase::kType_Glowmap)
    {
        char matLeaf[64] = "";
        SemanticCapture::GetLeafClassName(lightingMat, matLeaf, sizeof(matLeaf));
        auto* glowMat = static_cast<BSLightingShaderMaterialGlowmap*>(lightingMat);
        if (std::strcmp(matLeaf, "BSLightingShaderMaterialGlowmap") == 0 &&
            glowMat->spGlowMapTexture) {
            outTexHash = ExtractMaterialTexture(glowMat->spGlowMapTexture, "emissive", device, newTextures,
                                                TexturePostProcess::None,
                                                /*minRoughness=*/0, /*albedoLumFloor=*/0,
                                                /*tintRGB=*/0xFFFFFFu,
                                                /*paletteLut=*/nullptr, /*paletteRowV=*/0.0f,
                                                outPending, supplyPixels);
        }
    }

    // 2. Emissive color/scale from BSLightingShaderProperty
    if (g_config.emissiveColorEnabled) {
        NiProperty* prop = shape->shaderProperty;
        if (prop) {
            BSShaderProperty* shaderProp = static_cast<BSShaderProperty*>(prop);
            // Check EmitColor flag
            if (shaderProp->flags & BSShaderProperty::kShaderFlags_EmitColor) {
                BSLightingShaderProperty* lightingProp = static_cast<BSLightingShaderProperty*>(shaderProp);
                if (lightingProp->pEmissiveColor) {
                    outR = lightingProp->pEmissiveColor->r;
                    outG = lightingProp->pEmissiveColor->g;
                    outB = lightingProp->pEmissiveColor->b;
                }
                outIntensity = lightingProp->fEmitColorScale;
            }
        }
    }

    if (g_config.logEmissive && (outTexHash != 0 || outIntensity > 0.0f)) {
        const char* shapeName = shape->m_name.c_str();
        _MESSAGE("FO4RemixPlugin: [EMISSIVE] Shape \"%s\" glowTex=0x%016llX color=(%.3f,%.3f,%.3f) scale=%.3f",
                 shapeName ? shapeName : "<null>",
                 (unsigned long long)outTexHash, outR, outG, outB, outIntensity);
    }
}

// Parse vertices and indices from a BSTriShape. Returns false if the shape
// should be skipped (effect shader, missing data, NaN positions, bad indices).
// When logRejections is false, NaN/Inf and bad-index rejections are silent.
bool BsExtraction::ParseShapeGeometry(BSTriShape* shape, ParsedGeometry& out, bool logRejections,
                                      bool applyVertexColors, bool parseSkinning)
{
    if (!shape)
        return false;

    // Capped bail logging (2026-07-08 missing-heads investigation): 85
    // drawables sat permanently at parseFailed with no evidence trail
    // because every early-out here was silent. Skinned-shape bails always
    // log; the effect-shader gate additionally requires the skinned flag so
    // routine particle/effect rejects don't burn the cap.
    auto logParseBail = [&](const char* reason) {
        if (!logRejections) return;
        static std::atomic<int> s_bails{0};
        if (s_bails.fetch_add(1, std::memory_order_relaxed) < 20) {
            _MESSAGE("FO4RemixPlugin: [ParseBail] shape \"%s\" %s (dyn=%d skinned=%d desc=%016llX nV=%u nT=%u)",
                     shape->m_name.c_str() ? shape->m_name.c_str() : "",
                     reason,
                     shape->GetAsBSDynamicTriShape() ? 1 : 0,
                     (shape->vertexDesc & BSGeometry::kFlag_Skinned) ? 1 : 0,
                     (unsigned long long)shape->vertexDesc,
                     (unsigned)shape->numVertices, (unsigned)shape->numTriangles);
        }
    };
    const bool descSkinned = (shape->vertexDesc & BSGeometry::kFlag_Skinned) != 0;

    if (shape->numVertices == 0 || shape->numTriangles == 0) {
        if (descSkinned) logParseBail("zero verts/tris");
        return false;
    }

    // Skip effect shaders — not real geometry. Per F4SE NiMaterials.h:32,
    // GetFeature() returns 2 for lighting and 1 for effect; water returns
    // a different value. The original "!= 2" check over-rejected water as
    // a side effect of being written when only lighting was hooked.
    {
        NiProperty* prop = shape->shaderProperty;
        if (prop) {
            BSShaderProperty* sp = static_cast<BSShaderProperty*>(prop);
            BSShaderMaterial* mat = sp->shaderMaterial;
            if (!mat || mat->GetFeature() == 1) {
                if (descSkinned)
                    logParseBail(!mat ? "null shader material" : "effect-shader feature");
                return false;
            }
        }
    }

    // Renderer data -> vertex/index buffers (bails log via logParseBail).
    auto* gfxData = static_cast<BSGraphics::TriShape*>(shape->pRendererData);
    if (!gfxData || !gfxData->pVB || !gfxData->pIB) {
        logParseBail(!gfxData ? "null pRendererData"
                              : (!gfxData->pVB ? "null pVB" : "null pIB"));
        return false;
    }

    uint8_t* vbData = static_cast<uint8_t*>(gfxData->pVB->pData);
    uint8_t* ibData = static_cast<uint8_t*>(gfxData->pIB->pData);
    if (!vbData || !ibData) {
        logParseBail(!vbData ? "null pVB->pData" : "null pIB->pData");
        return false;
    }

    uint64_t desc = shape->vertexDesc;
    uint16_t vertexSize = shape->GetVertexSize();
    if (vertexSize == 0) {
        logParseBail("vertexSize 0");
        return false;
    }

    bool hasUVs     = (desc & BSGeometry::kFlag_UVs) != 0;
    bool hasNormals = (desc & BSGeometry::kFlag_Normals) != 0;
    bool hasColors  = (desc & BSGeometry::kFlag_VertexColors) != 0;

    BSDynamicTriShape* dynShape = shape->GetAsBSDynamicTriShape();

    // Attribute offsets are ABSOLUTE dword offsets within the static vertex
    // record -- including on real dynamic shapes. The engine's facegen
    // conversion (BSTriShape::CreateDynamicTriShape, VA 0x141831770,
    // decompiler-proven 2026-07-08, scripts/dynamic_trishape_desc.md) strips
    // the half4 position from the static record (n0 -= 2: head stride
    // 32 -> 24) and REBASES every attribute nibble by -2 dwords (UV 8 -> 0,
    // normal 12 -> 4, skin 20 -> 12), so post-conversion nibbles are already
    // record-relative. Adding the dynamic-vertex size n1 (:= 3, float3
    // positions in dynamicVertices) shifted every attribute read 12 bytes
    // high on heads: UVs/normals decoded garbage and oSkin (24+12) failed
    // the stride-24 fit gate, whose caller silently dropped every FaceGen
    // head ("missing heads", 2026-07-08). Non-dynamic shapes keep the n1
    // term: it is 0 on every well-formed static desc (F4SE: "szVertex: 0
    // when not dynamic"), which makes the formulas identical there, and the
    // PR#1 population of non-dynamic shapes carrying a junk n1 nibble keeps
    // its long-verified behavior.
    uint32_t szVertex = (desc >> 4) & 0xF;
    const uint32_t attrShift = dynShape ? 0 : szVertex;
    uint32_t oUV     = (attrShift + ((desc >>  8) & 0xF)) * 4;
    uint32_t oNormal = (attrShift + ((desc >> 16) & 0xF)) * 4;
    uint32_t oColor  = (attrShift + ((desc >> 24) & 0xF)) * 4;

    bool posHalfFloat = !(desc & BSGeometry::kFlag_FullPrecision);

    // BSDynamicTriShape stores morphed positions outside the normal vertex
    // buffer. Only take that path for the actual dynamic subclass
    // (RTTI-checked): some plain BSTriShape vertex descriptors have the
    // szVertex nibble set even though the object has no dynamicVertices
    // field -- on those, reading +0x180 dereferences memory past the
    // object (plain BSTriShape) or an unrelated field (subclass
    // neighbors), and the resulting "positions" render as garbled or
    // misplaced geometry. Cherry-picked from PR #1 (Kralich),
    // user-verified 2026-07-07 as the PR's load-bearing fix.
    uint8_t* posData = vbData;
    uint32_t posStride = vertexSize;
    bool isDynamic = false;
    if (dynShape) {
        uint8_t* dynVerts = dynShape->dynamicVertices;
        const uint16_t dynVertexSize = dynShape->GetDynamicVertexSize();
        if (dynVerts && dynVertexSize != 0) {
            posData = dynVerts;
            posStride = dynVertexSize;
            isDynamic = true;
        }
    }

    // Parse vertices
    out.vertices.resize(shape->numVertices);

    for (uint16_t i = 0; i < shape->numVertices; i++) {
        uint8_t* v = vbData + (uint32_t)i * vertexSize;
        remixapi_HardcodedVertex& out_v = out.vertices[i];
        memset(&out_v, 0, sizeof(out_v));

        // Position. Dynamic shapes read from dynamicVertices whose element
        // size is GetDynamicVertexSize() = szVertex nibble * 4. FaceGen
        // conversion authors n1=3: a 12-byte element that is HALF4 position
        // (x, y, z, bitangentX) in the first 8 bytes plus a 4-byte tail
        // (zeros/flags) -- NOT float3, despite the FullPrecision flag the
        // conversion sets. Proven byte-exact on 2026-07-08 run 3: the live
        // buffer of MaleMouthHumanoidDefault decodes as halfs to the
        // authored NIF half4 positions INCLUDING exact bitangent-X matches
        // (0.4834/0.4331/0.3613); read as float3 the same bytes produced
        // the giant "sail" vertices (half pairs reinterpreted as float32
        // reach 1e3..1e38). Only a 16-byte dynamic element (none observed;
        // defensive) takes the float path.
        uint8_t* pv = posData + (uint32_t)i * posStride;
        const bool posIsHalf = isDynamic ? (posStride <= 12) : posHalfFloat;
        if (posIsHalf) {
            uint16_t* pos = reinterpret_cast<uint16_t*>(pv);
            out_v.position[0] = HalfToFloat(pos[0]);
            out_v.position[1] = HalfToFloat(pos[1]);
            out_v.position[2] = HalfToFloat(pos[2]);
        } else {
            float* pos = reinterpret_cast<float*>(pv);
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

        // Normals
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

        // Color. Gated on the shader property's SLSF2_Vertex_Colors flag
        // (threaded in by the caller): FO4 meshes often carry a painted
        // color stream the vanilla shader ignores unless the flag is set.
        if (applyVertexColors && hasColors && oColor + 4 <= vertexSize) {
            memcpy(&out_v.color, v + oColor, 4);
        } else {
            out_v.color = 0xFFFFFFFF;
        }
    }

    // Skinning attributes. Nibble 7 of the vertexDesc is the skinning-data
    // offset (F4SE VertexDesc bitfield; same szVertex-relative convention as
    // the UV/normal/color nibbles above). Layout at that offset, RenderDoc-
    // confirmed: 4x float16 blend weights then 4x uint8 bone indices. The
    // engine's vertex shader consumes weights .xyz and reconstructs the 4th
    // as 1-(x+y+z); replicate that (the 4th stored half is not trusted).
    out.hasSkinning = false;
    if (parseSkinning && (desc & BSGeometry::kFlag_Skinned)) {
        // attrShift, not szVertex: on converted facegen heads the skin
        // nibble is already rebased (3 -> byte 12 of the 24-byte record).
        const uint32_t oSkin = (attrShift + (uint32_t)((desc >> 28) & 0xF)) * 4;
        if (oSkin + 12 <= vertexSize) {
            out.blendWeights.resize((size_t)shape->numVertices * 4);
            out.blendIndices.resize((size_t)shape->numVertices * 4);
            for (uint16_t i = 0; i < shape->numVertices; i++) {
                const uint8_t* v = vbData + (uint32_t)i * vertexSize;
                const uint16_t* wRaw = reinterpret_cast<const uint16_t*>(v + oSkin);
                float w0 = HalfToFloat(wRaw[0]);
                float w1 = HalfToFloat(wRaw[1]);
                float w2 = HalfToFloat(wRaw[2]);
                // Defensive: NaN/garbage halves become rigid bind to bone 0.
                if (!(w0 >= 0.0f && w0 <= 1.0f)) w0 = 0.0f;
                if (!(w1 >= 0.0f && w1 <= 1.0f)) w1 = 0.0f;
                if (!(w2 >= 0.0f && w2 <= 1.0f)) w2 = 0.0f;
                // The runtime's skinning shader derives the 4th weight as
                // 1-(w0+w1+w2) and SKIPS it when negative (skinning.h:103)
                // -- a triple sum above 1 therefore skins with effective
                // weight sum > 1 and the vertex overshoots along its world
                // position vector (world-fixed spike direction). Renormalize
                // the triple when it exceeds 1; no-op for well-formed data.
                const float triple = w0 + w1 + w2;
                if (triple > 1.0f) {
                    const float inv = 1.0f / triple;
                    w0 *= inv; w1 *= inv; w2 *= inv;
                }
                float w3 = 1.0f - (w0 + w1 + w2);
                if (w3 < 0.0f) w3 = 0.0f;
                const uint8_t* bi = v + oSkin + 8;
                float* wOut = &out.blendWeights[(size_t)i * 4];
                uint32_t* iOut = &out.blendIndices[(size_t)i * 4];
                wOut[0] = w0; wOut[1] = w1; wOut[2] = w2; wOut[3] = w3;
                iOut[0] = bi[0]; iOut[1] = bi[1]; iOut[2] = bi[2]; iOut[3] = bi[3];
                if (w0 + w1 + w2 + w3 <= 0.0f) {
                    wOut[0] = 1.0f;  // degenerate row: rigid to its first bone
                }
            }
            out.hasSkinning = true;
        } else if (logRejections) {
            _MESSAGE("FO4RemixPlugin: [Skinning] shape \"%s\" skinned desc=%016llX but "
                     "oSkin=%u+12 > stride=%u -- skinning attributes skipped",
                     shape->m_name.c_str() ? shape->m_name.c_str() : "",
                     (unsigned long long)desc, oSkin, vertexSize);
        }
    }

    // Validate vertex positions
    const char* shapeName = shape->m_name.c_str();
    for (uint16_t i = 0; i < shape->numVertices; i++) {
        const auto& pos = out.vertices[i].position;
        for (int j = 0; j < 3; j++) {
            if (std::isnan(pos[j]) || std::isinf(pos[j])) {
                if (logRejections)
                    _MESSAGE("FO4RemixPlugin: Rejecting mesh \"%s\" - vertex %u has NaN/Inf position",
                             shapeName ? shapeName : "<null>", i);
                return false;
            }
        }
    }

    // Index conversion (uint16 -> uint32), with per-triangle winding flip.
    //
    // Winding flip: BuildRemixTransform swaps Bethesda's X/Y axes, which is a
    // reflection (determinant -1). A mirroring transform inverts triangle
    // winding, so the game's front faces arrive at Remix as back faces. That
    // was invisible while every instance was submitted doubleSided=1; with
    // backface culling honored (2026-07-02 two-sided fix), the mirrored
    // winding culled the VISIBLE side of every single-sided mesh (hollow
    // terrain shells). Swapping the 2nd/3rd index of each triangle restores
    // the authored facing under the mirrored transform, and also makes
    // geometric normals (derived from winding) agree with the vertex normals.
    uint32_t indexCount = shape->numTriangles * 3;
    uint16_t* indices16 = reinterpret_cast<uint16_t*>(ibData);
    out.indices.resize(indexCount);
    for (uint32_t i = 0; i < indexCount; i++) {
        uint32_t idx = indices16[i];
        if (idx >= shape->numVertices) {
            if (logRejections)
                _MESSAGE("FO4RemixPlugin: Rejecting mesh \"%s\" - index[%u]=%u >= numVertices=%u",
                         shapeName ? shapeName : "<null>", i, idx, shape->numVertices);
            return false;
        }
        // Destination slot swaps indices 1<->2 within each triangle.
        const uint32_t tri = i / 3;
        const uint32_t corner = i % 3;
        const uint32_t dst = tri * 3 + (corner == 0 ? 0 : (corner == 1 ? 2 : 1));
        out.indices[dst] = idx;
    }

    out.vertexDesc = desc;
    out.vertexSize = vertexSize;
    out.vbData = vbData;
    out.isDynamic = isDynamic;

    return true;
}

// ---------------------------------------------------------------------------
// Extract alpha-test + alpha-blend state from the geometry's NiAlphaProperty
// ---------------------------------------------------------------------------
void BsExtraction::ExtractAlphaState(BSGeometry* geo, ExtractedMesh& mesh) {
    mesh.alphaTestEnabled = false;
    mesh.alphaTestType    = 7;   // Always (no test)
    mesh.alphaTestRef     = 128;
    mesh.alphaBlendEnabled    = false;
    mesh.srcColorBlendFactor  = 1;  // VK_BLEND_FACTOR_ONE
    mesh.dstColorBlendFactor  = 0;  // VK_BLEND_FACTOR_ZERO

    if (!geo) return;
    NiProperty* alphaPropRaw = geo->effectState;
    if (!alphaPropRaw) return;

    NiAlphaProperty* alphaProp = static_cast<NiAlphaProperty*>(alphaPropRaw);

    // Alpha test: bit 9 = enabled, bits 10-12 = function
    bool testEnabled = (alphaProp->alphaFlags >> 9) & 1;
    if (testEnabled) {
        int niTestFunc = (alphaProp->alphaFlags >> 10) & 7;
        // Bethesda quirk: many alpha-tested surfaces (rubble, roof debris,
        // postwar-house support posts) ship NiAlphaProperty with function=Always
        // despite needing real cutout. Vanilla DX11's shader appears to apply
        // a hardcoded discard against the threshold regardless of the function
        // field. Translating Always literally to VK_COMPARE_OP_ALWAYS makes
        // Remix's path tracer reject nothing -> surfaces render as solid
        // opaque rectangles. Override Always->Greater when the artist set a
        // meaningful reference value, matching what vanilla actually does.
        // Foliage and workshop supports already ship with function=Greater
        // and aren't affected.
        if (niTestFunc == 0 && alphaProp->alphaThreshold > 0) {
            niTestFunc = 4;  // NI Greater
        }
        // NI:  Always=0, Less=1, Equal=2, LessEq=3, Greater=4, NotEq=5, GreaterEq=6, Never=7
        // VK:  Never=0,  Less=1, Equal=2, LessEq=3, Greater=4, NotEq=5, GreaterEq=6, Always=7
        static const int niToVk[] = { 7, 1, 2, 3, 4, 5, 6, 0 };
        mesh.alphaTestEnabled = true;
        mesh.alphaTestType    = niToVk[niTestFunc];
        mesh.alphaTestRef     = alphaProp->alphaThreshold;
    }

    // Alpha blend: bit 0 = enabled, bits 1-4 = src factor, bits 5-8 = dst factor
    static const uint32_t niBlendToVk[] = {
        1,   //  0 kOne          -> VK_BLEND_FACTOR_ONE
        0,   //  1 kZero         -> VK_BLEND_FACTOR_ZERO
        2,   //  2 kSrcColor     -> VK_BLEND_FACTOR_SRC_COLOR
        3,   //  3 kInvSrcColor  -> VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR
        4,   //  4 kDestColor    -> VK_BLEND_FACTOR_DST_COLOR
        5,   //  5 kInvDestColor -> VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR
        6,   //  6 kSrcAlpha     -> VK_BLEND_FACTOR_SRC_ALPHA
        7,   //  7 kInvSrcAlpha  -> VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
        8,   //  8 kDestAlpha    -> VK_BLEND_FACTOR_DST_ALPHA
        9,   //  9 kInvDestAlpha -> VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA
        14,  // 10 kSrcAlphaSat  -> VK_BLEND_FACTOR_SRC_ALPHA_SATURATE
    };

    const bool blendEnabled = alphaProp->alphaFlags & 1;
    if (blendEnabled) {
        const int niSrc = (alphaProp->alphaFlags >> 1) & 0xF;
        const int niDst = (alphaProp->alphaFlags >> 5) & 0xF;
        const uint32_t vkSrc = (niSrc < 11) ? niBlendToVk[niSrc] : 1;
        const uint32_t vkDst = (niDst < 11) ? niBlendToVk[niDst] : 0;
        mesh.alphaBlendEnabled    = true;
        mesh.srcColorBlendFactor  = vkSrc;
        mesh.dstColorBlendFactor  = vkDst;
    }
}

// ---------------------------------------------------------------------------
// Return the player's current parent cell pointer (0 if unavailable)
// ---------------------------------------------------------------------------
uintptr_t BsExtraction::GetPlayerCellPtr()
{
    uintptr_t* ppPlayer = reinterpret_cast<uintptr_t*>(s_g_player.GetPtr());
    if (!ppPlayer || !*ppPlayer) return 0;
    uintptr_t player = *ppPlayer;
    return *reinterpret_cast<uintptr_t*>(player + OFF_REFR_PARENT_CELL);
}

// ---------------------------------------------------------------------------
// Read the player's world position. Outputs unchanged (remain 0) if the
// player singleton is absent (e.g. main menu).
// ---------------------------------------------------------------------------
void BsExtraction::GetPlayerPosition(float& outX, float& outY, float& outZ)
{
    uintptr_t* ppPlayer = reinterpret_cast<uintptr_t*>(s_g_player.GetPtr());
    if (!ppPlayer || !*ppPlayer) return;
    uintptr_t player = *ppPlayer;
    const float* pos = reinterpret_cast<const float*>(player + OFF_REFR_POS);
    outX = pos[0];
    outY = pos[1];
    outZ = pos[2];
}

// ---------------------------------------------------------------------------
// Lightweight readiness check — is the player in a cell with loaded 3D?
// ---------------------------------------------------------------------------
bool BsExtraction::IsPlayerCellReady()
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
// Return all cells currently loaded by the engine (from DataHandler::cellList)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Read exterior grid cells from the TES singleton's GridCellArray.
//
// TES singleton (RelocPtr at 0x032D2048) has GridCellArray* at +0x18.
// GridCellArray has:
//   +0x10: int32 gridDimension  (= uGridsToLoad, typically 5)
//   +0x18: TESObjectCELL**      (flat dim*dim array of cell pointers)
//
// We iterate the flat array and collect cells that have loaded 3D.
// ---------------------------------------------------------------------------
static void CollectGridCells(
    std::vector<CellInfo>& result,
    std::unordered_set<uintptr_t>& seen)
{
    uintptr_t* ppTES = reinterpret_cast<uintptr_t*>(s_g_tes.GetPtr());
    if (!ppTES || !*ppTES) return;
    uintptr_t tes = *ppTES;

    uintptr_t gridPtr = *reinterpret_cast<uintptr_t*>(tes + OFF_TES_GRID_CELLS);
    if (!gridPtr) return;

    int32_t dim = *reinterpret_cast<int32_t*>(gridPtr + OFF_GRID_DIMENSION);
    if (dim <= 0 || dim > 11) return;  // sanity check (uGridsToLoad is 3-11)

    uintptr_t* cellArray = *reinterpret_cast<uintptr_t**>(gridPtr + OFF_GRID_CELL_ARRAY);
    if (!cellArray) return;

    int32_t total = dim * dim;
    for (int32_t i = 0; i < total; i++) {
        uintptr_t cellPtr = cellArray[i];
        if (!cellPtr) continue;
        if (seen.count(cellPtr)) continue;

        // Verify the cell has objects with loaded 3D
        struct SimpleArray {
            uintptr_t* entries;
            uint32_t capacity;
            uint32_t pad0C;
            uint32_t count;
        };
        auto& objectList = *reinterpret_cast<SimpleArray*>(cellPtr + OFF_CELL_OBJECT_LIST);
        if (!objectList.entries || objectList.count == 0) continue;

        bool hasLoaded3D = false;
        uint32_t limit = objectList.count < 32u ? objectList.count : 32u;
        for (uint32_t j = 0; j < limit; j++) {
            uintptr_t refrPtr = objectList.entries[j];
            if (!refrPtr) continue;
            uintptr_t ld = *reinterpret_cast<uintptr_t*>(refrPtr + OFF_REFR_LOADED_DATA);
            if (!ld) continue;
            NiNode* rn = *reinterpret_cast<NiNode**>(ld + OFF_LOADED_ROOT_NODE);
            if (rn) { hasLoaded3D = true; break; }
        }
        if (!hasLoaded3D) continue;

        uint32_t formID = *reinterpret_cast<uint32_t*>(cellPtr + OFF_FORM_ID);
        result.push_back({ cellPtr, formID });
        seen.insert(cellPtr);
    }
}

std::vector<CellInfo> BsExtraction::GetLoadedCells()
{
    std::vector<CellInfo> result;
    std::unordered_set<uintptr_t> seen;

    // Always include the player's parentCell — this is the one cell guaranteed
    // to have loaded 3D.
    uintptr_t* ppPlayer = reinterpret_cast<uintptr_t*>(s_g_player.GetPtr());
    if (!ppPlayer || !*ppPlayer) return result;
    uintptr_t player = *ppPlayer;

    uintptr_t playerCell = *reinterpret_cast<uintptr_t*>(player + OFF_REFR_PARENT_CELL);
    if (!playerCell) return result;

    uint32_t playerCellFormID = *reinterpret_cast<uint32_t*>(playerCell + OFF_FORM_ID);
    result.push_back({ playerCell, playerCellFormID });
    seen.insert(playerCell);

    // Check if the player is in an exterior worldspace
    uint16_t cellFlags = *reinterpret_cast<uint16_t*>(playerCell + OFF_CELL_FLAGS);
    bool isExterior = (cellFlags & CELL_FLAG_IS_INTERIOR) == 0;

    if (isExterior) {
        // Exterior: read the GridCellArray from the TES singleton.
        // DataHandler::cellList doesn't contain grid-loaded exterior cells.
        CollectGridCells(result, seen);
        return result;
    }

    // Interior: supplement with DataHandler::cellList for attached cells
    uintptr_t* ppDataHandler = reinterpret_cast<uintptr_t*>(s_g_dataHandler.GetPtr());
    if (!ppDataHandler || !*ppDataHandler) return result;
    uintptr_t dh = *ppDataHandler;

    // NiTArray<TESObjectCELL*> cellList at offset 0xF58
    uintptr_t cellListBase = dh + 0xF58;
    uintptr_t* cellData = *reinterpret_cast<uintptr_t**>(cellListBase + 0x08);
    uint16_t emptyRunStart = *reinterpret_cast<uint16_t*>(cellListBase + 0x12);

    if (!cellData || emptyRunStart == 0) return result;

    for (uint16_t i = 0; i < emptyRunStart; i++) {
        uintptr_t cellPtr = cellData[i];
        if (!cellPtr) continue;
        if (seen.count(cellPtr)) continue;

        struct SimpleArray {
            uintptr_t* entries;
            uint32_t capacity;
            uint32_t pad0C;
            uint32_t count;
        };
        auto& objectList = *reinterpret_cast<SimpleArray*>(cellPtr + OFF_CELL_OBJECT_LIST);
        if (!objectList.entries || objectList.count == 0) continue;

        bool hasLoaded3D = false;
        for (uint32_t j = 0; j < objectList.count; j++) {
            uintptr_t refrPtr = objectList.entries[j];
            if (!refrPtr) continue;
            uintptr_t ld = *reinterpret_cast<uintptr_t*>(refrPtr + OFF_REFR_LOADED_DATA);
            if (!ld) continue;
            NiNode* rn = *reinterpret_cast<NiNode**>(ld + OFF_LOADED_ROOT_NODE);
            if (rn) { hasLoaded3D = true; break; }
        }
        if (!hasLoaded3D) continue;

        uint32_t formID = *reinterpret_cast<uint32_t*>(cellPtr + OFF_FORM_ID);
        result.push_back({ cellPtr, formID });
        seen.insert(cellPtr);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Clear the texture readback cache
// ---------------------------------------------------------------------------
void BsExtraction::ClearTextureCache()
{
    _MESSAGE("FO4RemixPlugin: ClearTextureCache - clearing %zu entries (%zu MiB)",
             g_textureCache.size(), g_textureCacheBytes / (1024u * 1024u));
    g_textureCache.clear();
    g_texResVariantIndex.clear();
    g_textureCacheBytes = 0;
    g_texKnownBad.clear();
}

bool BsExtraction::GetCachedTextureStats(uint64_t hash, uint32_t* outW, uint32_t* outH,
                                         uint32_t* outFmt, uint32_t outMeanRGBA[4])
{
    auto it = g_textureCache.find(hash);
    if (it == g_textureCache.end() || !it->second.tex) return false;
    const ExtractedTexture& tex = *it->second.tex;
    if (outW) *outW = tex.width;
    if (outH) *outH = tex.height;
    if (outFmt) *outFmt = (uint32_t)tex.dxgiFormat;
    if (outMeanRGBA) {
        outMeanRGBA[0] = outMeanRGBA[1] = outMeanRGBA[2] = outMeanRGBA[3] = 0;
        if ((tex.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
             tex.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
            tex.pixels.size() >= (size_t)tex.width * tex.height * 4) {
            uint64_t sum[4] = {};
            const size_t px = (size_t)tex.width * tex.height;
            const size_t step = px > 4096 ? px / 4096 : 1;
            size_t n = 0;
            for (size_t i = 0; i < px; i += step, ++n) {
                const uint8_t* p = tex.pixels.data() + i * 4;
                sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; sum[3] += p[3];
            }
            if (n) {
                for (int c = 0; c < 4; ++c) outMeanRGBA[c] = (uint32_t)(sum[c] / n);
            }
        } else {
            // BC1/BC2/BC3 (incl. SRGB variants): decode the 565 endpoint pair
            // of each sampled color block into a mean RGB, and report the
            // percentage of ALL-ZERO blocks in [3]. A zero-filled readback
            // (streaming stub / failed copy) shows as mean=(0,0,0) with
            // zeroPct=100; real content shows plausible endpoint means.
            uint32_t blockBytes = 0, colorOff = 0;
            switch (tex.dxgiFormat) {
            case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM:
            case DXGI_FORMAT_BC1_UNORM_SRGB: blockBytes = 8;  colorOff = 0; break;
            case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM:
            case DXGI_FORMAT_BC2_UNORM_SRGB:
            case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM:
            case DXGI_FORMAT_BC3_UNORM_SRGB: blockBytes = 16; colorOff = 8; break;
            default: break;
            }
            if (blockBytes) {
                const size_t bw = (tex.width + 3) / 4, bh = (tex.height + 3) / 4;
                size_t nBlocks = bw * bh;
                const size_t avail = tex.pixels.size() / blockBytes;
                if (nBlocks > avail) nBlocks = avail;
                const size_t step = nBlocks > 4096 ? nBlocks / 4096 : 1;
                uint64_t rSum = 0, gSum = 0, bSum = 0;
                size_t n = 0, zero = 0;
                for (size_t i = 0; i < nBlocks; i += step, ++n) {
                    const uint8_t* blk = tex.pixels.data() + i * blockBytes;
                    bool allZero = true;
                    for (uint32_t b = 0; b < blockBytes; ++b) {
                        if (blk[b]) { allZero = false; break; }
                    }
                    if (allZero) ++zero;
                    const uint16_t c0 = (uint16_t)(blk[colorOff] | (blk[colorOff + 1] << 8));
                    const uint16_t c1 = (uint16_t)(blk[colorOff + 2] | (blk[colorOff + 3] << 8));
                    rSum += (((c0 >> 11) & 0x1F) + ((c1 >> 11) & 0x1F)) * 255 / 62;
                    gSum += (((c0 >>  5) & 0x3F) + ((c1 >>  5) & 0x3F)) * 255 / 126;
                    bSum += (( c0        & 0x1F) + ( c1        & 0x1F)) * 255 / 62;
                }
                if (n) {
                    outMeanRGBA[0] = (uint32_t)(rSum / n);
                    outMeanRGBA[1] = (uint32_t)(gSum / n);
                    outMeanRGBA[2] = (uint32_t)(bSum / n);
                    outMeanRGBA[3] = (uint32_t)(zero * 100 / n);  // % all-zero blocks
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Placed-light extraction (revived 2026-07-07)
// ---------------------------------------------------------------------------
// Ported from the retired cell-pipeline light_extractor (deleted in phase 1B,
// recovered from 375ec3e~1). Offsets were memory-scan-verified then and are
// unchanged in 1.10.980. Runs on the game thread; raw reads of the cell's
// object list, matching this file's other cell walks.

static constexpr uintptr_t OFF_REFR_ROT_L       = 0xC0;   // Euler radians (3 floats)
static constexpr uintptr_t OFF_REFR_BASE_FORM_L = 0xE0;
static constexpr uintptr_t OFF_FORM_TYPE_L      = 0x1A;
static constexpr uint8_t   FORM_TYPE_LIGH_L     = 34;

// TESObjectLIGH DATA subrecord (radius=+0x4, color=+0x8, flags=+0xC, fov=+0x14
// from base+0x148; FNAM fade right after the 0x38-byte DATA block).
static constexpr uintptr_t OFF_LIGH_DATA_L   = 0x148;
static constexpr uintptr_t OFF_DATA_RADIUS_L = 0x04;
static constexpr uintptr_t OFF_DATA_COLOR_L  = 0x08;
static constexpr uintptr_t OFF_DATA_FLAGS_L  = 0x0C;
static constexpr uintptr_t OFF_DATA_FOV_L    = 0x14;
static constexpr uintptr_t OFF_LIGH_FADE_L   = OFF_LIGH_DATA_L + 0x38;
static constexpr uint32_t  LIGH_FLAG_SPOTLIGHT_L = 0x100;

// FO4 light -> HDR radiance conversion; radius-scaled so lights carry to
// their intended range at Bethesda scene scale (~70 units/meter). Tuned so
// ini [Lights] Intensity=1.0 is reasonable.
static constexpr float kLightIntensityScale = 0.1f;

std::vector<ExtractedLight> BsExtraction::ExtractCellLights(uintptr_t cellPtr)
{
    std::vector<ExtractedLight> result;
    if (!cellPtr) return result;

    struct SimpleArray {
        uintptr_t* entries;
        uint32_t capacity;
        uint32_t pad0C;
        uint32_t count;
    };
    auto& objectList = *reinterpret_cast<SimpleArray*>(cellPtr + OFF_CELL_OBJECT_LIST);
    if (!objectList.entries) return result;

    for (uint32_t i = 0; i < objectList.count && i < 65536; i++) {
        uintptr_t refrPtr = objectList.entries[i];
        if (!refrPtr) continue;

        uintptr_t baseForm = *reinterpret_cast<uintptr_t*>(refrPtr + OFF_REFR_BASE_FORM_L);
        if (!baseForm) continue;
        if (*reinterpret_cast<uint8_t*>(baseForm + OFF_FORM_TYPE_L) != FORM_TYPE_LIGH_L) continue;

        float* refrPos = reinterpret_cast<float*>(refrPtr + OFF_REFR_POS);
        float* refrRot = reinterpret_cast<float*>(refrPtr + OFF_REFR_ROT_L);

        uintptr_t dataBase = baseForm + OFF_LIGH_DATA_L;
        uint32_t rawRadius = *reinterpret_cast<uint32_t*>(dataBase + OFF_DATA_RADIUS_L);
        uint32_t rawColor  = *reinterpret_cast<uint32_t*>(dataBase + OFF_DATA_COLOR_L);
        uint32_t rawFlags  = *reinterpret_cast<uint32_t*>(dataBase + OFF_DATA_FLAGS_L);
        float spotFOV      = *reinterpret_cast<float*>(dataBase + OFF_DATA_FOV_L);
        float fade         = *reinterpret_cast<float*>(baseForm + OFF_LIGH_FADE_L);

        if (rawRadius == 0 || rawRadius > 100000) continue;
        if (!(fade > 0.0f) || fade > 100.0f) fade = 1.0f;

        const float r = (rawColor & 0xFF) / 255.0f;
        const float g = ((rawColor >> 8) & 0xFF) / 255.0f;
        const float b = ((rawColor >> 16) & 0xFF) / 255.0f;
        const float intensity = fade * kLightIntensityScale * (float)rawRadius;

        ExtractedLight light = {};
        const uint32_t refrFormID = *reinterpret_cast<uint32_t*>(refrPtr + OFF_FORM_ID);
        light.hash = FnvHashCombine(0xCBF29CE484222325ULL, (uint64_t)refrFormID);

        // Beth X/Y swap, matching BuildRemixTransform's mesh convention.
        light.position[0] = refrPos[1];
        light.position[1] = refrPos[0];
        light.position[2] = refrPos[2];

        light.radiance[0] = r * intensity;
        light.radiance[1] = g * intensity;
        light.radiance[2] = b * intensity;
        light.radius = (float)rawRadius;

        light.isSpotLight = (rawFlags & LIGH_FLAG_SPOTLIGHT_L) != 0;
        if (light.isSpotLight) {
            const float rx = refrRot[0];  // pitch
            const float rz = refrRot[2];  // yaw
            // Default light direction is -Z (down), rotated by the REFR.
            const float dx = sinf(rz) * cosf(rx);
            const float dy = cosf(rz) * cosf(rx);
            const float dz = -sinf(rx);
            light.spotDirection[0] = dy;  // X/Y swap
            light.spotDirection[1] = dx;
            light.spotDirection[2] = dz;
            light.spotFOV = spotFOV;
            light.spotSoftness = 0.2f;
        }

        result.push_back(light);
    }

    if (g_config.logLights && !result.empty()) {
        _MESSAGE("FO4RemixPlugin: [Lights] cell=0x%llX extracted %zu placed lights",
                 (unsigned long long)cellPtr, result.size());
    }
    return result;
}
