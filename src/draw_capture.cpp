#include "draw_capture.h"

#include <d3d11.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <cstring>

#include "f4se/PluginAPI.h"  // _MESSAGE

namespace DrawCapture {

typedef void (STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(
    ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void (STDMETHODCALLTYPE* PFN_DrawIndexed)(
    ID3D11DeviceContext*, UINT, UINT, INT);
typedef void (STDMETHODCALLTYPE* PFN_DrawInstanced)(
    ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE* PFN_DrawIndexedInstancedIndirect)(
    ID3D11DeviceContext*, ID3D11Buffer*, UINT);
typedef void (STDMETHODCALLTYPE* PFN_SetShaderResources)(
    ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
static PFN_DrawIndexedInstanced g_original = nullptr;
static PFN_DrawIndexed g_originalDX = nullptr;
static PFN_DrawInstanced g_originalDI = nullptr;
static PFN_DrawIndexedInstancedIndirect g_originalDIII = nullptr;
static PFN_SetShaderResources g_originalVSSet = nullptr;
static PFN_SetShaderResources g_originalPSSet = nullptr;
static PFN_SetShaderResources g_originalCSSet = nullptr;
static std::atomic<bool> g_hooked{false};

constexpr int      kMaxWatches       = 16;
constexpr int      kMaxDrawsPerFrame = 24;
constexpr int      kVsSrvSlots       = 16;     // scan t0..t15 for the record SRV
constexpr uint64_t kDeadlineMs       = 15000;  // then the caller falls back

// How the engine actually renders BSMergeInstancedTriShape (established by
// the 2026-07-03 diagnostic runs): the shape's own record SRV (wrapper q1
// at shape+0x170) is bound at VS t8 -- t7 gets the same SRV too, most
// likely as the previous-frame copy for motion vectors -- and the draws
// are NOT hardware-instanced: 161k DrawIndexedInstanced calls never had a
// watched SRV bound (only stray inst=1 draws with leftover bindings, e.g.
// idx offset 91880 into a large SHARED index buffer, correctly rejected by
// validation). The engine CPU-instances the merged shape: plain
// DrawIndexed per instance with the record index fed via constant buffer.
// So the capture is: while a desc-verified watched SRV sits at t7/t8,
// count identical DrawIndexed tuples per frame -- each unique index range
// is a sub-model (possibly at a shared-IB offset; the consumer normalizes
// by the smallest captured start), and its per-frame repeat count is that
// sub-model's instance count (times the number of render passes, which
// the consumer divides out).
// Safety: every pointer match desc-verifies (structured, stride 80,
// ByteWidth == recordCount*80) -- recycled pointers produced false
// captures in run 2 and cannot pass that check.
static std::atomic<uint64_t> g_diiCalls{0};
static std::atomic<uint64_t> g_diCalls{0};
static std::atomic<uint64_t> g_diiiCalls{0};
static std::atomic<uint64_t> g_stride80Hits{0};
static std::atomic<uint64_t> g_bindHits{0};
static std::atomic<int>      g_diagLogs{0};
static std::atomic<int>      g_bindLogs{0};
static std::atomic<bool>     g_typeLogged{false};

struct Watch {
    enum State { kFree, kActive, kDone, kExpired };
    State    state = kFree;
    void*    buffer = nullptr;
    void*    srv = nullptr;
    uint64_t key = 0;
    uint32_t expectedBytes = 0;  // recordCount * 80; desc-verified on match
    uint64_t registeredTick = 0;
    uint32_t registeredFrame = 0;
    uint32_t bindCount = 0;      // times a Set*ShaderResources bound us
    uint32_t rearms = 0;         // invalid-frame retries granted so far
    // Draws accumulate per frame in cur[]; the previous frame's complete
    // list rolls into done[] so Query never reads a half-recorded frame.
    uint32_t curFrame = 0;
    int      curCount = 0;
    SegDraw  cur[kMaxDrawsPerFrame];
    uint32_t doneFrame = 0;
    int      doneCount = 0;
    SegDraw  done[kMaxDrawsPerFrame];
};

static std::mutex            g_lock;
static Watch                 g_watches[kMaxWatches];
static std::atomic<int>      g_activeCount{0};
static std::atomic<uint32_t> g_frame{1};

static void RollFrame(Watch& w) {
    if (w.curCount > 0) {
        std::memcpy(w.done, w.cur, sizeof(SegDraw) * (size_t)w.curCount);
        w.doneCount = w.curCount;
        w.doneFrame = w.curFrame;
    }
    w.curCount = 0;
}

// Pointer-equality is necessary but no longer sufficient: a destroyed
// watched object's address can be recycled into an unrelated live one
// (proven by run 2's particle-draw false matches). Confirm the resource
// really is THIS shape's record buffer before trusting a match.
static bool DescMatches(ID3D11Resource* r, uint32_t expectedBytes) {
    if (!r) return false;
    ID3D11Buffer* b = nullptr;
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Buffer), (void**)&b))) return false;
    D3D11_BUFFER_DESC bd = {};
    b->GetDesc(&bd);
    b->Release();
    return (bd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) &&
           bd.StructureByteStride == 80 && bd.ByteWidth == expectedBytes;
}

static void STDMETHODCALLTYPE hkDrawIndexedInstanced(
    ID3D11DeviceContext* ctx, UINT idxCount, UINT instCount,
    UINT startIdx, INT baseVtx, UINT startInst)
{
    g_diiCalls.fetch_add(1, std::memory_order_relaxed);
    // Fast path: one relaxed load per draw when nothing is being watched.
    if (g_activeCount.load(std::memory_order_relaxed) > 0) {
        if (!g_typeLogged.exchange(true)) {
            _MESSAGE("FO4RemixPlugin: [DrawCap] first watched-window draw: "
                     "ctxType=%d tid=%lu inst=%u",
                     (int)ctx->GetType(), GetCurrentThreadId(), instCount);
        }
        ID3D11ShaderResourceView* srvs[kVsSrvSlots] = {};
        ID3D11Resource* res[kVsSrvSlots] = {};
        ctx->VSGetShaderResources(0, kVsSrvSlots, srvs);
        for (int i = 0; i < kVsSrvSlots; ++i) {
            if (srvs[i]) srvs[i]->GetResource(&res[i]);
        }
        // Diagnostic: log EVERY stride-80 structured buffer on a VS t-slot
        // for the first draws, with usage/cpu flags (dynamic copy theory).
        if (instCount >= 2 && g_diagLogs.load(std::memory_order_relaxed) < 40) {
            bool any = false;
            for (int i = 0; i < kVsSrvSlots; ++i) {
                if (!res[i]) continue;
                ID3D11Buffer* b = nullptr;
                if (FAILED(res[i]->QueryInterface(__uuidof(ID3D11Buffer),
                                                  (void**)&b))) {
                    continue;
                }
                D3D11_BUFFER_DESC bd = {};
                b->GetDesc(&bd);
                b->Release();
                if ((bd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) &&
                    bd.StructureByteStride == 80) {
                    any = true;
                    const int dl = g_diagLogs.load(std::memory_order_relaxed);
                    if (dl < 40) {
                        _MESSAGE("FO4RemixPlugin: [DrawCapDiag] #%d f=%u t%d "
                                 "buf=%p srv=%p bytes=%u usage=%u cpu=0x%X "
                                 "idx=%u+%u inst=%u+%u bv=%d",
                                 dl, g_frame.load(std::memory_order_relaxed), i,
                                 (void*)res[i], (void*)srvs[i], bd.ByteWidth,
                                 (unsigned)bd.Usage, bd.CPUAccessFlags,
                                 startIdx, idxCount, startInst, instCount,
                                 baseVtx);
                    }
                }
            }
            if (any) {
                g_stride80Hits.fetch_add(1, std::memory_order_relaxed);
                g_diagLogs.fetch_add(1, std::memory_order_relaxed);
            }
        }
        {
            std::lock_guard<std::mutex> g(g_lock);
            const uint32_t f = g_frame.load(std::memory_order_relaxed);
            for (Watch& w : g_watches) {
                if (w.state != Watch::kActive) continue;
                bool match = false;
                for (int i = 0; i < kVsSrvSlots && !match; ++i) {
                    if (!srvs[i]) continue;
                    if (srvs[i] == w.srv || (res[i] && res[i] == w.buffer)) {
                        match = DescMatches(res[i], w.expectedBytes);
                    }
                }
                if (!match) continue;
                if (w.curFrame != f) {
                    RollFrame(w);
                    w.curFrame = f;
                }
                bool dup = false;
                for (int k = 0; k < w.curCount && !dup; ++k) {
                    const SegDraw& d = w.cur[k];
                    dup = d.kind == 0 &&
                          d.indexCount == idxCount && d.startIndex == startIdx &&
                          d.baseVertex == baseVtx && d.instanceCount == instCount &&
                          d.startInstance == startInst;
                }
                if (!dup && w.curCount < kMaxDrawsPerFrame) {
                    w.cur[w.curCount] = { idxCount, startIdx, baseVtx,
                                          instCount, startInst,
                                          (uint32_t)w.curCount, 0 };
                    ++w.curCount;
                }
            }
        }
        for (int i = 0; i < kVsSrvSlots; ++i) {
            if (res[i]) res[i]->Release();
            if (srvs[i]) srvs[i]->Release();
        }
    }
    g_original(ctx, idxCount, instCount, startIdx, baseVtx, startInst);
}

// The merge-shape draw path: one plain DrawIndexed per instance with the
// record SRV at VS t7/t8. Hot -- DrawIndexed is the game's most frequent
// draw call -- so the peek is exactly two slots and pointer-compares
// against the watch table before anything heavier runs.
static void STDMETHODCALLTYPE hkDrawIndexed(
    ID3D11DeviceContext* ctx, UINT idxCount, UINT startIdx, INT baseVtx)
{
    if (g_activeCount.load(std::memory_order_relaxed) > 0) {
        ID3D11ShaderResourceView* srvs[2] = {};
        ctx->VSGetShaderResources(7, 2, srvs);  // t7..t8, the proven slots
        for (int i = 0; i < 2; ++i) {
            ID3D11ShaderResourceView* srv = srvs[i];
            if (!srv) continue;
            // Unlocked pointer pre-filter; races here only cost a
            // GetResource on a stale candidate, never a wrong capture
            // (the locked pass re-checks and desc-verifies).
            bool candidate = false;
            for (const Watch& w : g_watches) {
                if (w.state == Watch::kActive && srv == w.srv) {
                    candidate = true;
                    break;
                }
            }
            if (!candidate) continue;
            ID3D11Resource* r = nullptr;
            srv->GetResource(&r);
            {
                std::lock_guard<std::mutex> g(g_lock);
                const uint32_t f = g_frame.load(std::memory_order_relaxed);
                for (Watch& w : g_watches) {
                    if (w.state != Watch::kActive || srv != w.srv) continue;
                    if (!DescMatches(r, w.expectedBytes)) continue;
                    if (w.curFrame != f) {
                        RollFrame(w);
                        w.curFrame = f;
                    }
                    bool counted = false;
                    for (int k = 0; k < w.curCount && !counted; ++k) {
                        SegDraw& d = w.cur[k];
                        if (d.kind == 1 && d.indexCount == idxCount &&
                            d.startIndex == startIdx && d.baseVertex == baseVtx) {
                            ++d.instanceCount;  // repeat = another instance
                            counted = true;
                        }
                    }
                    if (!counted && w.curCount < kMaxDrawsPerFrame) {
                        w.cur[w.curCount] = { idxCount, startIdx, baseVtx,
                                              1, 0, (uint32_t)w.curCount, 1 };
                        ++w.curCount;
                    }
                }
            }
            if (r) r->Release();
        }
        for (int i = 0; i < 2; ++i) {
            if (srvs[i]) srvs[i]->Release();
        }
    }
    g_originalDX(ctx, idxCount, startIdx, baseVtx);
}

static void STDMETHODCALLTYPE hkDrawInstanced(
    ID3D11DeviceContext* ctx, UINT vtxCount, UINT instCount,
    UINT startVtx, UINT startInst)
{
    g_diCalls.fetch_add(1, std::memory_order_relaxed);
    g_originalDI(ctx, vtxCount, instCount, startVtx, startInst);
}

static void STDMETHODCALLTYPE hkDrawIndexedInstancedIndirect(
    ID3D11DeviceContext* ctx, ID3D11Buffer* args, UINT offset)
{
    g_diiiCalls.fetch_add(1, std::memory_order_relaxed);
    g_originalDIII(ctx, args, offset);
}

// Shared body for the Set*ShaderResources hooks: answer, cheaply and
// decisively, whether the engine EVER binds a watched SRV -- or any view
// of a watched buffer -- on this stage, and at which slot.
static void CheckBind(const char* stage, UINT startSlot, UINT numViews,
                      ID3D11ShaderResourceView* const* views) {
    if (g_activeCount.load(std::memory_order_relaxed) == 0 || !views) return;
    for (UINT v = 0; v < numViews; ++v) {
        ID3D11ShaderResourceView* srv = views[v];
        if (!srv) continue;
        // Pointer pre-filter against the watch table without the lock;
        // confirm under the lock only on candidate hits.
        bool candidate = false;
        for (const Watch& w : g_watches) {
            if (w.state != Watch::kActive) continue;
            if (srv == w.srv) { candidate = true; break; }
        }
        ID3D11Resource* r = nullptr;
        if (!candidate) {
            srv->GetResource(&r);
            for (const Watch& w : g_watches) {
                if (w.state != Watch::kActive) continue;
                if (r && r == w.buffer) { candidate = true; break; }
            }
        }
        if (candidate) {
            g_bindHits.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> g(g_lock);
            for (Watch& w : g_watches) {
                if (w.state != Watch::kActive) continue;
                if (srv == w.srv || (r && r == w.buffer)) {
                    ++w.bindCount;
                    const int bl = g_bindLogs.fetch_add(1, std::memory_order_relaxed);
                    if (bl < 40) {
                        _MESSAGE("FO4RemixPlugin: [DrawCap] BIND %s t%u f=%u "
                                 "key=0x%llX srv=%p",
                                 stage, startSlot + v,
                                 g_frame.load(std::memory_order_relaxed),
                                 (unsigned long long)w.key, (void*)srv);
                    }
                }
            }
        }
        if (r) r->Release();
    }
}

static void STDMETHODCALLTYPE hkVSSetShaderResources(
    ID3D11DeviceContext* ctx, UINT startSlot, UINT numViews,
    ID3D11ShaderResourceView* const* views)
{
    CheckBind("VS", startSlot, numViews, views);
    g_originalVSSet(ctx, startSlot, numViews, views);
}

static void STDMETHODCALLTYPE hkPSSetShaderResources(
    ID3D11DeviceContext* ctx, UINT startSlot, UINT numViews,
    ID3D11ShaderResourceView* const* views)
{
    CheckBind("PS", startSlot, numViews, views);
    g_originalPSSet(ctx, startSlot, numViews, views);
}

static void STDMETHODCALLTYPE hkCSSetShaderResources(
    ID3D11DeviceContext* ctx, UINT startSlot, UINT numViews,
    ID3D11ShaderResourceView* const* views)
{
    CheckBind("CS", startSlot, numViews, views);
    g_originalCSSet(ctx, startSlot, numViews, views);
}

void InstallHook(ID3D11DeviceContext* ctx) {
    static std::atomic<bool> sAttempted{false};
    if (g_hooked.load(std::memory_order_acquire)) return;
    if (sAttempted.exchange(true)) return;
    void** vtbl = *reinterpret_cast<void***>(ctx);
    void* target = vtbl[20];      // DrawIndexedInstanced
    void* targetDX = vtbl[12];    // DrawIndexed (the actual merge draw path)
    void* targetDI = vtbl[21];    // DrawInstanced (counter only)
    void* targetDIII = vtbl[39];  // DrawIndexedInstancedIndirect (counter only)
    void* targetPSSet = vtbl[8];  // PSSetShaderResources (bind diag)
    void* targetVSSet = vtbl[25]; // VSSetShaderResources (bind diag)
    void* targetCSSet = vtbl[67]; // CSSetShaderResources (bind diag)
    if (MH_CreateHook(target, &hkDrawIndexedInstanced,
                      reinterpret_cast<void**>(&g_original)) == MH_OK &&
        MH_EnableHook(target) == MH_OK &&
        MH_CreateHook(targetDX, &hkDrawIndexed,
                      reinterpret_cast<void**>(&g_originalDX)) == MH_OK &&
        MH_EnableHook(targetDX) == MH_OK) {
        g_hooked.store(true, std::memory_order_release);
        _MESSAGE("FO4RemixPlugin: [DrawCap] DrawIndexedInstanced + DrawIndexed "
                 "hooked at %p / %p (ctxType=%d)", target, targetDX,
                 (int)ctx->GetType());
    } else {
        _MESSAGE("FO4RemixPlugin: [DrawCap] ERROR - draw hooks failed");
    }
    if (MH_CreateHook(targetDI, &hkDrawInstanced,
                      reinterpret_cast<void**>(&g_originalDI)) == MH_OK) {
        MH_EnableHook(targetDI);
    }
    if (MH_CreateHook(targetDIII, &hkDrawIndexedInstancedIndirect,
                      reinterpret_cast<void**>(&g_originalDIII)) == MH_OK) {
        MH_EnableHook(targetDIII);
    }
    if (MH_CreateHook(targetVSSet, &hkVSSetShaderResources,
                      reinterpret_cast<void**>(&g_originalVSSet)) == MH_OK) {
        MH_EnableHook(targetVSSet);
    }
    if (MH_CreateHook(targetPSSet, &hkPSSetShaderResources,
                      reinterpret_cast<void**>(&g_originalPSSet)) == MH_OK) {
        MH_EnableHook(targetPSSet);
    }
    if (MH_CreateHook(targetCSSet, &hkCSSetShaderResources,
                      reinterpret_cast<void**>(&g_originalCSSet)) == MH_OK) {
        MH_EnableHook(targetCSSet);
    }
}

bool Hooked() {
    return g_hooked.load(std::memory_order_acquire);
}

void OnPresent() {
    g_frame.fetch_add(1, std::memory_order_relaxed);
}

QueryResult Query(void* buffer, void* srv, uint64_t key, uint32_t recordCount,
                  std::vector<SegDraw>& out) {
    if (!Hooked()) return kUnavailable;
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> g(g_lock);
    Watch* w = nullptr;
    for (Watch& c : g_watches) {
        if (c.state != Watch::kFree && c.key == key) { w = &c; break; }
    }
    if (w && w->state == Watch::kExpired) {
        // A NEW resolve cycle for a shape whose earlier watch timed out.
        // Give it a fresh window instead of failing forever.
        w->state = Watch::kActive;
        w->registeredTick = now;
        w->registeredFrame = g_frame.load(std::memory_order_relaxed);
        g_activeCount.fetch_add(1, std::memory_order_relaxed);
        return kCapturing;
    }
    if (!w) {
        // New key: claim a free slot, else recycle the oldest finished one.
        Watch* victim = nullptr;
        for (Watch& c : g_watches) {
            if (c.state == Watch::kFree) { victim = &c; break; }
            if (c.state != Watch::kActive &&
                (!victim || c.registeredTick < victim->registeredTick)) {
                victim = &c;
            }
        }
        if (!victim) return kUnavailable;  // all 16 slots actively watching
        *victim = Watch{};
        victim->state = Watch::kActive;
        victim->buffer = buffer;
        victim->srv = srv;
        victim->key = key;
        victim->expectedBytes = recordCount * 80u;
        victim->registeredTick = now;
        victim->registeredFrame = g_frame.load(std::memory_order_relaxed);
        g_activeCount.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> sWatchLogs{0};
        const int wl = sWatchLogs.fetch_add(1, std::memory_order_relaxed);
        if (wl < 24) {
            _MESSAGE("FO4RemixPlugin: [DrawCap] watch #%d key=0x%llX buf=%p srv=%p rc=%u",
                     wl, (unsigned long long)key, buffer, srv, recordCount);
        }
        return kCapturing;
    }
    if (w->state == Watch::kActive) {
        // A frame older than the current one is complete even if no newer
        // draw has rolled it yet.
        const uint32_t f = g_frame.load(std::memory_order_relaxed);
        if (w->curCount > 0 && w->curFrame < f) {
            RollFrame(*w);
        }
        if (w->doneCount > 0) {
            w->state = Watch::kDone;
            g_activeCount.fetch_sub(1, std::memory_order_relaxed);
        } else if (now - w->registeredTick > kDeadlineMs) {
            w->state = Watch::kExpired;
            g_activeCount.fetch_sub(1, std::memory_order_relaxed);
            static std::atomic<int> sExpireLogs{0};
            const int el = sExpireLogs.fetch_add(1, std::memory_order_relaxed);
            if (el < 24) {
                _MESSAGE("FO4RemixPlugin: [DrawCap] expire #%d key=0x%llX "
                         "framesWatched=%u binds=%u dii=%llu di=%llu diii=%llu "
                         "stride80=%llu bindHits=%llu",
                         el, (unsigned long long)key,
                         f - w->registeredFrame, w->bindCount,
                         (unsigned long long)g_diiCalls.load(std::memory_order_relaxed),
                         (unsigned long long)g_diCalls.load(std::memory_order_relaxed),
                         (unsigned long long)g_diiiCalls.load(std::memory_order_relaxed),
                         (unsigned long long)g_stride80Hits.load(std::memory_order_relaxed),
                         (unsigned long long)g_bindHits.load(std::memory_order_relaxed));
            }
            return kUnavailable;
        } else {
            return kCapturing;
        }
    }
    if (w->state == Watch::kDone) {
        out.assign(w->done, w->done + w->doneCount);
        return kReady;
    }
    return kUnavailable;  // unreachable fallthrough
}

bool Rearm(uint64_t key) {
    constexpr uint32_t kMaxRearms = 5;
    std::lock_guard<std::mutex> g(g_lock);
    for (Watch& c : g_watches) {
        if (c.state != Watch::kDone || c.key != key) continue;
        if (c.rearms >= kMaxRearms) return false;
        ++c.rearms;
        c.state = Watch::kActive;
        c.registeredTick = GetTickCount64();
        c.registeredFrame = g_frame.load(std::memory_order_relaxed);
        c.curCount = 0;
        c.doneCount = 0;
        g_activeCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

}  // namespace DrawCapture
