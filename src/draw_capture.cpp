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
static PFN_DrawIndexedInstanced g_original = nullptr;
static std::atomic<bool> g_hooked{false};

constexpr int      kMaxWatches       = 16;
constexpr int      kMaxDrawsPerFrame = 24;
constexpr int      kVsSrvSlots       = 8;     // scan t0..t7 for the record SRV
constexpr uint64_t kDeadlineMs       = 4000;  // then the caller falls back

struct Watch {
    enum State { kFree, kActive, kDone, kExpired };
    State    state = kFree;
    void*    buffer = nullptr;
    void*    srv = nullptr;
    uint64_t key = 0;
    uint64_t registeredTick = 0;
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
    // Fast path: one relaxed load per draw when nothing is being watched.
    if (g_activeCount.load(std::memory_order_relaxed) > 0) {
        ID3D11ShaderResourceView* srvs[kVsSrvSlots] = {};
        ID3D11Resource* res[kVsSrvSlots] = {};
        ctx->VSGetShaderResources(0, kVsSrvSlots, srvs);
        for (int i = 0; i < kVsSrvSlots; ++i) {
            if (srvs[i]) srvs[i]->GetResource(&res[i]);
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

void InstallHook(ID3D11DeviceContext* ctx) {
    static std::atomic<bool> sAttempted{false};
    if (g_hooked.load(std::memory_order_acquire)) return;
    if (sAttempted.exchange(true)) return;
    void** vtbl = *reinterpret_cast<void***>(ctx);
    void* target = vtbl[20];  // ID3D11DeviceContext::DrawIndexedInstanced
    if (MH_CreateHook(target, &hkDrawIndexedInstanced,
                      reinterpret_cast<void**>(&g_original)) == MH_OK &&
        MH_EnableHook(target) == MH_OK) {
        g_hooked.store(true, std::memory_order_release);
        _MESSAGE("FO4RemixPlugin: [DrawCap] DrawIndexedInstanced hooked at %p", target);
    } else {
        _MESSAGE("FO4RemixPlugin: [DrawCap] ERROR - DrawIndexedInstanced hook failed");
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
        g_activeCount.fetch_add(1, std::memory_order_relaxed);
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
            w->state = Watch::kExpired;  // remembered: no re-register loop
            g_activeCount.fetch_sub(1, std::memory_order_relaxed);
            return kUnavailable;
        } else {
            return kCapturing;
        }
    }
    if (w->state == Watch::kDone) {
        out.assign(w->done, w->done + w->doneCount);
        return kReady;
    }
    return kUnavailable;  // kExpired
}

}  // namespace DrawCapture
