#pragma once

#include <cstdint>
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
// unused slots): only DrawIndexed calls whose IndexCount equals one of
// the nonzero entries times 3 can belong to the shape -- the filter that
// keeps sticky t8 bindings from attributing unrelated scene draws
// (run 4's noise captures) to the watch.
QueryResult Query(void* buffer, void* srv, uint64_t key, uint32_t recordCount,
                  const uint32_t segTris[4], std::vector<SegDraw>& out);

// The captured frame didn't validate (e.g. a shadow-only frame that drew a
// partial sub-model set): put the watch back to capturing so the next
// frame gets a chance. Returns false when the re-arm budget is exhausted
// -- the caller should fall back instead of deferring again.
bool Rearm(uint64_t key);

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

}  // namespace DrawCapture
