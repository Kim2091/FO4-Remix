#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

struct ID3D11DeviceContext;

// Captures the game's own DrawIndexedInstanced parameters for
// BSMergeInstancedTriShape instance buffers. The vanilla renderer already
// draws every merged shape correctly -- per sub-model index ranges, per
// sub-model instance counts, and the currently active LOD slice -- so
// instead of reverse-engineering the packed-geometry tables from engine
// memory (the +0x1D0 descriptor route died 2026-07-03: invalid pointer on
// the road cluster, polymorphic objects elsewhere), we watch the D3D11
// draw stream for draws that bind a shape's instance-record SRV and reuse
// the engine's exact partition for the Remix expansion.
namespace DrawCapture {

struct SegDraw {
    uint32_t indexCount;      // IndexCountPerInstance
    uint32_t startIndex;      // StartIndexLocation
    int32_t  baseVertex;      // BaseVertexLocation
    uint32_t instanceCount;   // kind 0: InstanceCount of the draw
                              // kind 1: times this exact DrawIndexed tuple
                              //         was seen within the frame (the
                              //         engine CPU-instances merged shapes:
                              //         one DrawIndexed per instance)
    uint32_t startInstance;   // kind 0 only: StartInstanceLocation
    uint32_t order;           // first-seen order within the captured frame
    uint32_t kind;            // 0 = DrawIndexedInstanced, 1 = DrawIndexed
};

// Hook ID3D11DeviceContext::DrawIndexedInstanced (vtable slot 20) on the
// context's shared vtable. Attempted once; idempotent afterwards.
void InstallHook(ID3D11DeviceContext* ctx);
bool Hooked();

// Frame boundary, called once per Present.
void OnPresent();

enum QueryResult {
    kUnavailable = -1,  // hook not installed, table full, or deadline passed
    kCapturing   = 0,   // registered; the engine's next draws will be caught
    kReady       = 1,   // out = the draw list of the most recent complete frame
};

// Register-or-poll the watch for one shape. `buffer`/`srv` are raw pointer
// IDENTITIES of the shape's structured instance buffer and its paired SRV
// (no references held -- compared against the bound pipeline state only,
// and desc-verified against recordCount*80 bytes before a match counts:
// recycled pointers produced false captures in run 2).
// `key` identifies the shape across calls (mesh hash). `segTris` is the
// shape's +0x1A0 per-LOD triangle table (4 dwords, zeros/garbage in
// unused slots). NOTE: since the run-6 baked-chunk model the table is
// recorded for diagnostics only -- chunk draws are ~2k-tri slices of the
// expanded mesh whose sizes are unrelated to segTris*3; attribution
// safety comes from the per-draw s8==srv re-read, the stride-80
// desc-verify, and the resolver's record-anchored chunk validation.
QueryResult Query(void* buffer, void* srv, uint64_t key, uint32_t recordCount,
                  const uint32_t segTris[4], std::vector<SegDraw>& out);

// The captured frame didn't validate (e.g. a shadow-only frame that drew a
// partial sub-model set): put the watch back to capturing so the next
// frame gets a chance. Returns false when the re-arm budget is exhausted
// -- the caller should fall back instead of deferring again (the slot is
// freed so a later EnsureWatch can start a fresh hunt).
bool Rearm(uint64_t key);

// Capture-upgrade support (2026-07-04, run-4 evidence): at load, watches
// for clusters the engine isn't currently DRAWING starve -- their record
// SRVs get bound every frame (bind-count proved it) but chunk draws only
// happen for visible clusters -- so the resolver falls back and, before
// this API, could never recover. EnsureWatch keeps a background watch
// alive for such a shape: registers it if missing, re-arms it if expired
// (upgrade-hunt watches never age out on their own), and returns true
// exactly when a completed capture is waiting -- the caller then releases
// its fallback submission and re-resolves, which consumes the capture via
// Query/GetChunks. buffer/srv are pointer identities only.
bool EnsureWatch(void* buffer, void* srv, uint64_t key, uint32_t recordCount,
                 const uint32_t segTris[4]);

// The resolver consumed the current accumulated chunk union for an upgrade
// (or initial bake): snapshot the consumed count so EnsureWatch only fires
// again on GROWTH, and put the watch back to hunting so the union keeps
// merging new chunk draws as more of the cluster becomes visible.
void MarkConsumed(uint64_t key);

// Free this key's watch slot immediately (any state). Called when the
// owning drawable is evicted (TTL sweep) so cell churn can't strand
// upgrade-hunt watches that nobody will poll again -- a stranded kActive
// hunt pins one of the kMaxWatches slots AND keeps the bind-scan hot path
// enabled for the whole session. Orphans that slip through anyway (e.g.
// MarkConsumed re-armed a hunt whose resolver stopped polling) are reaped
// by OnPresent after kHuntOrphanMs without a poll.
void Drop(uint64_t key);

// Drop every watch (and the t8 ownership pointer). Called on PreLoadGame:
// captured IB/VB offsets point into the engine's shared geometry pools,
// which are repacked when a different world loads -- a stale capture
// served after reload slices the wrong pool region and renders garbage
// (run-3's "progressively worse each save reload").
void ResetAll();

// Run-6 discovery: the engine draws merged shapes from a PRE-BAKED
// expanded mesh -- every instance's geometry duplicated into a large
// shared IB/VB, sliced into ~2k-tri chunks at 16-bit-index vertex-window
// boundaries, drawn as plain DrawIndexed with a per-chunk IB OFFSET while
// the record SRV sits at t8 (vertices are piece-local; the VS applies the
// record transform fetched by a per-vertex record index). Chunk draws are
// captured with full IA state so the resolver can read the expanded
// buffers back and recover the exact record->sub-model mapping.
struct ChunkDraw {
    void*    ib;        // ID3D11Buffer* identity (no reference held)
    uint32_t ibOffset;  // bytes into the shared index buffer
    uint32_t idxCount;
    uint32_t ibFormat;  // DXGI_FORMAT (57 = R16_UINT on every sample)
    void*    vb;        // slot-0 vertex buffer identity
    uint32_t vbOffset;  // bytes
    uint32_t vbStride;
};

// Chunk draws of the most recent complete frame for this key (captured
// while the watch's SRV was verified live at t8). Returns the count
// copied into out (0 = none captured).
int GetChunks(uint64_t key, ChunkDraw* out, int maxOut);

// ---------------------------------------------------------------------------
// Occlusion signal (2026-07-21). The engine still ISSUES every scene draw
// each frame (we swallow them at the D3D11 hooks via RasterSuppress after
// observing), and FO4's previs occlusion + frustum culling is CPU-side, so
// the surviving draw stream IS the engine's per-frame visibility verdict for
// exactly the geometry it decided to render. We key each draw by its bound
// index buffer identity -- run-5 [DrawWin] proved the engine selects
// geometry via the IASetIndexBuffer OFFSET (StartIndexLocation stays 0), so
// (ID3D11Buffer*, byte offset) uniquely tags a shape's slice of a shared
// pool. OnFrame compares each submitted drawable's captured key against this
// per-frame "was drawn" set: a key that was drawn recently then stopped is
// occluded (or engine-frustum-culled), and its bucket can leave the TLAS.
//
// Fail-safe by construction: a key NEVER seen (convention mismatch, or a
// draw path we don't hook) is absent from the map and stays EXEMPT, so a
// wrong guess only makes the feature do nothing -- it never culls something
// the engine is drawing.

// Shared key formula -- MUST match between the draw-side stamp and the
// submit-side capture. 0 is reserved for "no key" (exempt).
inline uint64_t EngineIbKey(const void* ib, uint32_t offset) {
    if (!ib) return 0;
    uint64_t h = reinterpret_cast<uintptr_t>(ib);
    h ^= (uint64_t)offset * 0x9E3779B97F4A7C15ull;
    h *= 0xD6E8FEB86659FD93ull;
    h ^= h >> 32;
    return h ? h : 1;
}

// Master gate. Cheap atomic; set from OnFrame each frame off the config
// flag so there is no init-ordering dependency. While false the draw hooks
// skip all visibility bookkeeping (one relaxed load per draw).
void SetOcclusionEnabled(bool enabled);

// Copy the published "drawn recently" map for the Remix thread to read
// lock-free. `outFrame` receives the present-frame counter the ages are
// measured against; `outLastFrameDrawCount` receives how many distinct keys
// the engine drew on the most recent published frame -- OnFrame treats a
// near-zero count as "scene not actively rendering" (pause/menu/load) and
// suspends occlusion culling that frame so nothing mass-culls.
void SnapshotVisible(std::unordered_map<uint64_t, uint32_t>& out,
                     uint32_t& outFrame, uint32_t& outLastFrameDrawCount);

}  // namespace DrawCapture
