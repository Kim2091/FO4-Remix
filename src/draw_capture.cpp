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
typedef void (STDMETHODCALLTYPE* PFN_DrawInstanced)(
    ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE* PFN_DrawIndexedInstancedIndirect)(
    ID3D11DeviceContext*, ID3D11Buffer*, UINT);
static PFN_DrawIndexedInstanced g_original = nullptr;
static PFN_DrawInstanced g_originalDI = nullptr;
static PFN_DrawIndexedInstancedIndirect g_originalDIII = nullptr;
static std::atomic<bool> g_hooked{false};

constexpr int      kMaxWatches       = 16;
constexpr int      kMaxDrawsPerFrame = 24;
constexpr int      kVsSrvSlots       = 16;     // scan t0..t15 for the record SRV
constexpr uint64_t kDeadlineMs       = 15000;  // then the caller falls back
                                               // (generous: registration can
                                               // happen during loading, before
                                               // the world ever renders)

// 2026-07-03 diagnostics: the first take-8 run captured NOTHING (all
// watches expired; zero [MergeDraw] lines) while the hook itself installed
// fine. These counters pin down where the merge draws actually are:
//  - g_diiCalls / g_diCalls / g_diiiCalls: are instanced draws flowing
//    through the hooked vtable at all (deferred-context miss if not)?
//  - g_stride80Hits: do ANY instanced draws bind a stride-80 structured
//    buffer on a VS t-slot (identity/slot mismatch if yes but no match)?
//  - [DrawCapDiag] lines: the first draws that DO bind stride-80 buffers,
//    with slot + buffer pointer to compare against the watched pointers.
static std::atomic<uint64_t> g_diiCalls{0};
static std::atomic<uint64_t> g_diCalls{0};
static std::atomic<uint64_t> g_diiiCalls{0};
static std::atomic<uint64_t> g_stride80Hits{0};
static std::atomic<int>      g_diagLogs{0};
static std::atomic<bool>     g_typeLogged{false};

struct Watch {
    enum State { kFree, kActive, kDone, kExpired };
    State    state = kFree;
    void*    buffer = nullptr;
    void*    srv = nullptr;
    uint64_t key = 0;
    uint64_t registeredTick = 0;
    uint32_t registeredFrame = 0;
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
        // Diagnostic: does this draw bind ANY stride-80 structured buffer
        // on a VS t-slot? Capped GetDesc probing while watches are active.
        if (instCount >= 2 && g_diagLogs.load(std::memory_order_relaxed) < 24) {
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
                    g_stride80Hits.fetch_add(1, std::memory_order_relaxed);
                    const int dl = g_diagLogs.fetch_add(1, std::memory_order_relaxed);
                    if (dl < 24) {
                        _MESSAGE("FO4RemixPlugin: [DrawCapDiag] #%d t%d buf=%p "
                                 "srv=%p bytes=%u idx=%u+%u inst=%u+%u bv=%d",
                                 dl, i, (void*)res[i], (void*)srvs[i],
                                 bd.ByteWidth, startIdx, idxCount,
                                 startInst, instCount, baseVtx);
                    }
                    break;
                }
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
                    match = (srvs[i] == w.srv) || (res[i] && res[i] == w.buffer);
                }
                if (!match) continue;
                if (w.curFrame != f) {
                    RollFrame(w);
                    w.curFrame = f;
                }
                bool dup = false;
                for (int k = 0; k < w.curCount && !dup; ++k) {
                    const SegDraw& d = w.cur[k];
                    dup = d.indexCount == idxCount && d.startIndex == startIdx &&
                          d.baseVertex == baseVtx && d.instanceCount == instCount &&
                          d.startInstance == startInst;
                }
                if (!dup && w.curCount < kMaxDrawsPerFrame) {
                    w.cur[w.curCount] = { idxCount, startIdx, baseVtx,
                                          instCount, startInst,
                                          (uint32_t)w.curCount };
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

void InstallHook(ID3D11DeviceContext* ctx) {
    static std::atomic<bool> sAttempted{false};
    if (g_hooked.load(std::memory_order_acquire)) return;
    if (sAttempted.exchange(true)) return;
    void** vtbl = *reinterpret_cast<void***>(ctx);
    void* target = vtbl[20];    // DrawIndexedInstanced
    void* targetDI = vtbl[21];  // DrawInstanced (counter only)
    void* targetDIII = vtbl[39];  // DrawIndexedInstancedIndirect (counter only)
    if (MH_CreateHook(target, &hkDrawIndexedInstanced,
                      reinterpret_cast<void**>(&g_original)) == MH_OK &&
        MH_EnableHook(target) == MH_OK) {
        g_hooked.store(true, std::memory_order_release);
        _MESSAGE("FO4RemixPlugin: [DrawCap] DrawIndexedInstanced hooked at %p "
                 "(ctxType=%d)", target, (int)ctx->GetType());
    } else {
        _MESSAGE("FO4RemixPlugin: [DrawCap] ERROR - DrawIndexedInstanced hook failed");
    }
    if (MH_CreateHook(targetDI, &hkDrawInstanced,
                      reinterpret_cast<void**>(&g_originalDI)) == MH_OK) {
        MH_EnableHook(targetDI);
    }
    if (MH_CreateHook(targetDIII, &hkDrawIndexedInstancedIndirect,
                      reinterpret_cast<void**>(&g_originalDIII)) == MH_OK) {
        MH_EnableHook(targetDIII);
    }
}

bool Hooked() {
    return g_hooked.load(std::memory_order_acquire);
}

void OnPresent() {
    g_frame.fetch_add(1, std::memory_order_relaxed);
}

QueryResult Query(void* buffer, void* srv, uint64_t key,
                  std::vector<SegDraw>& out) {
    if (!Hooked()) return kUnavailable;
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> g(g_lock);
    Watch* w = nullptr;
    for (Watch& c : g_watches) {
        if (c.state != Watch::kFree && c.key == key) { w = &c; break; }
    }
    if (w && w->state == Watch::kExpired) {
        // A NEW resolve cycle for a shape whose earlier watch timed out
        // (e.g. registered during loading, before the world rendered).
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
        victim->registeredTick = now;
        victim->registeredFrame = g_frame.load(std::memory_order_relaxed);
        g_activeCount.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> sWatchLogs{0};
        const int wl = sWatchLogs.fetch_add(1, std::memory_order_relaxed);
        if (wl < 24) {
            _MESSAGE("FO4RemixPlugin: [DrawCap] watch #%d key=0x%llX buf=%p srv=%p",
                     wl, (unsigned long long)key, buffer, srv);
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
                         "framesWatched=%u dii=%llu di=%llu diii=%llu stride80=%llu",
                         el, (unsigned long long)key,
                         f - w->registeredFrame,
                         (unsigned long long)g_diiCalls.load(std::memory_order_relaxed),
                         (unsigned long long)g_diCalls.load(std::memory_order_relaxed),
                         (unsigned long long)g_diiiCalls.load(std::memory_order_relaxed),
                         (unsigned long long)g_stride80Hits.load(std::memory_order_relaxed));
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

}  // namespace DrawCapture
