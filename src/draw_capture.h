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
// `key` identifies the shape across calls (mesh hash).
QueryResult Query(void* buffer, void* srv, uint64_t key, uint32_t recordCount,
                  std::vector<SegDraw>& out);

// The captured frame didn't validate (e.g. a shadow-only frame that drew a
// partial sub-model set): put the watch back to capturing so the next
// frame gets a chance. Returns false when the re-arm budget is exhausted
// -- the caller should fall back instead of deferring again.
bool Rearm(uint64_t key);

}  // namespace DrawCapture
