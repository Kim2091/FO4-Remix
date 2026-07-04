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

constexpr int      kMaxWatches       = 32;
constexpr int      kMaxDrawsPerFrame = 24;
constexpr int      kVsSrvSlots       = 16;     // scan t0..t15 for the record SRV
// Initial-resolve patience only. Run 4: a visible cluster's chunk draws
// land within a frame or two of the watch arming, while a not-visible
// cluster gets BINDS every frame but never a draw -- waiting 15s just
// delayed the fallback without ever converting a starved watch. Expired
// watches now continue hunting in the background via EnsureWatch
// (upgradeHunt), so the foreground deadline can be short.
constexpr uint64_t kDeadlineMs       = 4000;

// How the engine actually renders BSMergeInstancedTriShape (established by
// the 2026-07-03 diagnostic runs): the shape's own record SRV (wrapper q1
// at shape+0x170) is bound at VS t8 -- t7 gets the same SRV too, most
// likely as the previous-frame copy for motion vectors -- and the draws
// are NOT hardware-instanced: 161k DrawIndexedInstanced calls never had a
// watched SRV bound. The engine CPU-instances the merged shape: plain
// DrawIndexed per instance with the record index fed via constant buffer.
//
// Attribution (run-4 lesson): D3D11 bindings are STICKY -- t8 keeps the
// record SRV across later unrelated draws whose shaders never touch t8,
// so sampling VSGetShaderResources at draw time attributed random scene
// draws (e.g. an 11k-tri mesh against a 7-record shape) to the watch.
// Instead: the VSSetShaderResources hook tracks WHICH watch's SRV is
// currently at t8 (ordered, same-thread bind events), and hkDrawIndexed
// counts a draw for that watch ONLY if its IndexCount equals one of the
// shape's known per-LOD triangle counts (+0x1A0 table) times 3. Each
// unique surviving index range is a sub-model (offset into a shared IB;
// the consumer normalizes by the smallest start), and its per-frame
// repeat count is that sub-model's instance count times the number of
// render passes, which the consumer divides out.
// Safety: every pointer match desc-verifies (structured, stride 80,
// ByteWidth == recordCount*80) -- recycled pointers produced false
// captures in run 2 and cannot pass that check.
static std::atomic<uint64_t> g_diiCalls{0};
static std::atomic<uint64_t> g_diCalls{0};
static std::atomic<uint64_t> g_diiiCalls{0};
static std::atomic<uint64_t> g_stride80Hits{0};
static std::atomic<uint64_t> g_bindHits{0};
static std::atomic<int>      g_bindLogs{0};

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
    bool     upgradeHunt = false;  // background watch for a fallen-back
                                   // drawable: exempt from kDeadlineMs
    uint32_t expectedIdx[4] = {};  // segTris[s]*3 for nonzero, sane slots
    int      nExpected = 0;
    // Upgrade bookkeeping: cdone[] ACCUMULATES across frames (the engine
    // draws only the pieces visible in a given frame, so any one frame is
    // a subset of the cluster); consumedChunks = cdoneCount at the last
    // upgrade so set GROWTH is detectable; upgradesServed caps re-resolve
    // churn per shape.
    int      consumedChunks = 0;
    uint32_t upgradesServed = 0;
    // Baked-chunk capture (run-6 model): ccur[] collects within a frame,
    // RollChunks MERGES it into the accumulated cdone[] union.
    static constexpr int kMaxChunks = 64;
    uint32_t  ccurFrame = 0;
    int       ccurCount = 0;
    ChunkDraw ccur[kMaxChunks];
    int       cdoneCount = 0;
    ChunkDraw cdone[kMaxChunks];
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
// The watch whose (desc-verified) record SRV is currently bound at VS t8.
// Set/cleared by the VSSetShaderResources hook, consumed by hkDrawIndexed
// on the same render thread; Watch storage is static so the pointer is
// always dereferenceable and state is re-checked under the lock.
static std::atomic<Watch*>   g_boundT8{nullptr};

// Draw-window diagnostics (run 5): after a record-SRV bind, log the next
// few draws with FULL IA state -- run 5 showed every draw at
// StartIndexLocation=0, so Bethesda selects geometry via the
// IASetIndexBuffer OFFSET, which the plain draw params never showed.
static std::atomic<int>      g_winRemaining{0};
static std::atomic<uint64_t> g_winKey{0};
static std::atomic<uint64_t> g_lastWinKey{0};
static std::atomic<int>      g_winCount{0};

// Engine call-stack capture: resolve return addresses to module+offset so
// the exact Fallout4.exe functions issuing binds/draws can be read out of
// the on-disk exe afterwards. Capped callers only -- MODULE resolution per
// frame is a loader-lock query.
static void FormatStack(char* out, size_t cap, int skip, int frames) {
    void* bt[16];
    const int n = (int)CaptureStackBackTrace((ULONG)skip,
                                             (ULONG)(frames < 16 ? frames : 16),
                                             bt, nullptr);
    size_t pos = 0;
    out[0] = 0;
    for (int i = 0; i < n; ++i) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        uint64_t off = (uint64_t)bt[i];
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)bt[i], &mod) && mod) {
            char full[MAX_PATH] = "";
            if (GetModuleFileNameA(mod, full, sizeof(full))) {
                const char* base = strrchr(full, '\\');
                strncpy_s(name, base ? base + 1 : full, _TRUNCATE);
            }
            off = (uint64_t)bt[i] - (uint64_t)mod;
        }
        const int w = snprintf(out + pos, cap - pos, "%s%s+0x%llX",
                               i ? " " : "", name, (unsigned long long)off);
        if (w <= 0 || (size_t)w >= cap - pos) break;
        pos += (size_t)w;
    }
}

static void LogWindowDraw(ID3D11DeviceContext* ctx, const char* kind,
                          UINT idxCount, UINT startIdx, INT baseVtx,
                          UINT instCount) {
    if (g_winRemaining.load(std::memory_order_relaxed) <= 0) return;
    if (g_winRemaining.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT off = 0;
    ctx->IAGetIndexBuffer(&ib, &fmt, &off);
    ID3D11ShaderResourceView* s8 = nullptr;
    ctx->VSGetShaderResources(8, 1, &s8);
    Watch* owner = g_boundT8.load(std::memory_order_relaxed);
    const int ours = (owner && s8 == owner->srv) ? 1 : 0;
    char stk[400];
    FormatStack(stk, sizeof(stk), 2, 8);  // skip self + hook thunk
    _MESSAGE("FO4RemixPlugin: [DrawWin] %s key=0x%llX idx=%u+%u bv=%d inst=%u "
             "ib=%p off=%u fmt=%d t8ours=%d stk=[%s]",
             kind, (unsigned long long)g_winKey.load(std::memory_order_relaxed),
             startIdx, idxCount, baseVtx, instCount, (void*)ib, off, (int)fmt,
             ours, stk);
    if (ib) ib->Release();
    if (s8) s8->Release();
}

static void RollFrame(Watch& w) {
    if (w.curCount > 0) {
        std::memcpy(w.done, w.cur, sizeof(SegDraw) * (size_t)w.curCount);
        w.doneCount = w.curCount;
        w.doneFrame = w.curFrame;
    }
    w.curCount = 0;
}

static void RollChunks(Watch& w) {
    // MERGE the frame's chunk draws into the accumulated union instead of
    // replacing it: one frame only contains the pieces the engine deemed
    // visible, so replacement made every capture a view-dependent subset
    // (run 5: half-empty bakes -> holes). Dedup key = (ib, offset, count).
    for (int i = 0; i < w.ccurCount; ++i) {
        const ChunkDraw& c = w.ccur[i];
        bool dup = false;
        for (int k = 0; k < w.cdoneCount && !dup; ++k) {
            dup = w.cdone[k].ib == c.ib && w.cdone[k].ibOffset == c.ibOffset &&
                  w.cdone[k].idxCount == c.idxCount;
        }
        if (!dup && w.cdoneCount < Watch::kMaxChunks) {
            w.cdone[w.cdoneCount++] = c;
        }
    }
    w.ccurCount = 0;
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
    LogWindowDraw(ctx, "DII", idxCount, startIdx, baseVtx, instCount);
    // SegDraw sampling REMOVED (2026-07-04). Run-4 ground truth: with 15
    // watches active for a full 17s window (9.3M DII calls, record SRVs
    // bound 1-2x per frame per watch), the draw-time sampling never
    // attributed a single DII draw to any watch -- the engine's merged-
    // shape draws are plain DrawIndexed chunk draws (hkDrawIndexed), and
    // the resolver already discards kind-0 entries as sticky-binding
    // noise. The sampling cost (VSGetShaderResources(t0..t15) +
    // GetResource x16 per draw while any watch is active) is now
    // permanent overhead with background upgrade-hunt watches, so the
    // dead path is gone: this hook is one atomic add + a capped
    // diagnostic check per draw.
    g_original(ctx, idxCount, instCount, startIdx, baseVtx, startInst);
}

// The merge-shape draw path: one plain DrawIndexed per instance with the
// record SRV at VS t8. Attribution comes from the bind-event tracking in
// hkVSSetShaderResources (g_boundT8), NOT from sampling bound state here
// -- bindings are sticky and draw-time sampling attributed unrelated
// scene draws to the watch (run 4). The per-LOD IndexCount filter kills
// what stickiness remains: only draws sized exactly like one of the
// shape's +0x1A0 sub-ranges can belong to it. No D3D calls on the fast
// path at all.
static void STDMETHODCALLTYPE hkDrawIndexed(
    ID3D11DeviceContext* ctx, UINT idxCount, UINT startIdx, INT baseVtx)
{
    LogWindowDraw(ctx, "DX", idxCount, startIdx, baseVtx, 1);
    Watch* w = g_boundT8.load(std::memory_order_relaxed);
    if (w) {
        // Exact state check kills ownership staleness: is OUR SRV
        // literally at t8 for THIS draw? (Run 4/5 lesson: bindings and
        // even bind-tracked ownership go stale across unrelated draws.)
        ID3D11ShaderResourceView* s8 = nullptr;
        ctx->VSGetShaderResources(8, 1, &s8);
        const bool ours = (s8 == w->srv);
        if (s8) s8->Release();
        if (ours) {
            // Baked-chunk capture: this draw is one ~2k-tri slice of the
            // shape's expanded mesh, selected by the IB offset.
            ID3D11Buffer* ib = nullptr;
            DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
            UINT ibOff = 0;
            ctx->IAGetIndexBuffer(&ib, &fmt, &ibOff);
            ID3D11Buffer* vb = nullptr;
            UINT stride = 0, vbOff = 0;
            ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &vbOff);
            {
                std::lock_guard<std::mutex> g(g_lock);
                if (w->state == Watch::kActive) {
                    const uint32_t f = g_frame.load(std::memory_order_relaxed);
                    if (w->ccurFrame != f) {
                        RollChunks(*w);
                        w->ccurFrame = f;
                    }
                    bool dup = false;
                    for (int k = 0; k < w->ccurCount && !dup; ++k) {
                        dup = w->ccur[k].ibOffset == ibOff &&
                              w->ccur[k].idxCount == idxCount;
                    }
                    if (!dup && w->ccurCount < Watch::kMaxChunks) {
                        w->ccur[w->ccurCount++] = { (void*)ib, ibOff, idxCount,
                                                    (uint32_t)fmt, (void*)vb,
                                                    vbOff, stride };
                    }
                }
            }
            if (ib) ib->Release();
            if (vb) vb->Release();
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
    // t8 ownership tracking for hkDrawIndexed: any VSSet covering slot 8
    // re-decides which watch (if any) owns subsequent DrawIndexed calls.
    // Unlocked watch-table reads are the usual benign pre-filter race --
    // the draw path re-checks state under the lock.
    if (views && startSlot <= 8 && 8 < startSlot + numViews &&
        g_activeCount.load(std::memory_order_relaxed) > 0) {
        ID3D11ShaderResourceView* v8 = views[8 - startSlot];
        Watch* newBound = nullptr;
        if (v8) {
            for (Watch& w : g_watches) {
                if (w.state == Watch::kActive && v8 == w.srv) {
                    ID3D11Resource* r = nullptr;
                    v8->GetResource(&r);
                    if (DescMatches(r, w.expectedBytes)) newBound = &w;
                    if (r) r->Release();
                    break;
                }
            }
        }
        g_boundT8.store(newBound, std::memory_order_relaxed);
        // Open a draw window on a fresh ownership: log the next draws with
        // full IA state. One window at a time, distinct keys preferred,
        // capped per session.
        if (newBound &&
            g_winCount.load(std::memory_order_relaxed) < 12 &&
            g_winRemaining.load(std::memory_order_relaxed) <= 0 &&
            g_lastWinKey.load(std::memory_order_relaxed) != newBound->key) {
            g_winKey.store(newBound->key, std::memory_order_relaxed);
            g_lastWinKey.store(newBound->key, std::memory_order_relaxed);
            g_winCount.fetch_add(1, std::memory_order_relaxed);
            char stk[400];
            FormatStack(stk, sizeof(stk), 2, 8);
            _MESSAGE("FO4RemixPlugin: [DrawWin] open key=0x%llX f=%u stk=[%s]",
                     (unsigned long long)newBound->key,
                     g_frame.load(std::memory_order_relaxed), stk);
            g_winRemaining.store(12, std::memory_order_relaxed);
        }
    } else if (views && startSlot <= 8 && 8 < startSlot + numViews) {
        g_boundT8.store(nullptr, std::memory_order_relaxed);
    }
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
                  const uint32_t segTris[4], std::vector<SegDraw>& out) {
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
        for (int s = 0; s < 4; ++s) {
            // garbage slot-3 values (floats/717-style) stay under this
            // multiplier harmlessly -- an unrelated draw would have to be
            // sized exactly segTris[s]*3 WHILE our SRV owns t8
            if (segTris[s] && segTris[s] < 0x400000u) {
                victim->expectedIdx[victim->nExpected++] = segTris[s] * 3u;
            }
        }
        victim->registeredTick = now;
        victim->registeredFrame = g_frame.load(std::memory_order_relaxed);
        g_activeCount.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> sWatchLogs{0};
        const int wl = sWatchLogs.fetch_add(1, std::memory_order_relaxed);
        if (wl < 24) {
            _MESSAGE("FO4RemixPlugin: [DrawCap] watch #%d key=0x%llX buf=%p srv=%p "
                     "rc=%u expIdx=[%u,%u,%u,%u]",
                     wl, (unsigned long long)key, buffer, srv, recordCount,
                     victim->expectedIdx[0], victim->expectedIdx[1],
                     victim->expectedIdx[2], victim->expectedIdx[3]);
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
        if (w->ccurCount > 0 && w->ccurFrame < f) {
            RollChunks(*w);
        }
        if (w->doneCount > 0 || w->cdoneCount > 0) {
            w->state = Watch::kDone;
            g_activeCount.fetch_sub(1, std::memory_order_relaxed);
        } else if (!w->upgradeHunt && now - w->registeredTick > kDeadlineMs) {
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
        if (c.rearms >= kMaxRearms) {
            // Budget spent on frames that never validated: free the slot so
            // a later EnsureWatch can start a fresh hunt (with a fresh
            // budget) instead of pinning stale done[] data forever.
            c = Watch{};
            return false;
        }
        ++c.rearms;
        c.state = Watch::kActive;
        c.registeredTick = GetTickCount64();
        c.registeredFrame = g_frame.load(std::memory_order_relaxed);
        c.curCount = 0;
        c.doneCount = 0;
        c.ccurCount = 0;
        c.cdoneCount = 0;
        g_activeCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool EnsureWatch(void* buffer, void* srv, uint64_t key, uint32_t recordCount,
                 const uint32_t segTris[4]) {
    if (!Hooked()) return false;
    constexpr uint32_t kMaxUpgradesPerShape = 6;
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> g(g_lock);
    for (Watch& c : g_watches) {
        if (c.state == Watch::kFree || c.key != key) continue;
        if (c.upgradesServed >= kMaxUpgradesPerShape) {
            // Enough resubmissions for this shape; stop hunting and free
            // the slot for others (entry lingers as evictable).
            if (c.state == Watch::kActive) {
                c.state = Watch::kExpired;
                g_activeCount.fetch_sub(1, std::memory_order_relaxed);
            }
            return false;
        }
        if (c.state == Watch::kDone) {
            // Only report when the accumulated union GREW past what the
            // last upgrade consumed -- otherwise every poll would re-serve
            // the same set and churn resubmissions.
            return c.cdoneCount > c.consumedChunks;
        }
        if (c.state == Watch::kExpired) {
            c.state = Watch::kActive;
            c.upgradeHunt = true;
            c.registeredTick = now;
            c.registeredFrame = g_frame.load(std::memory_order_relaxed);
            c.curCount = 0;
            c.doneCount = 0;
            c.ccurCount = 0;
            // cdone[] union intentionally KEPT: it's the accumulated
            // coverage; stale entries are cheap to re-drop at bake time.
            g_activeCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // kActive: roll completed frames so fresh chunk draws land in the
        // union on this poll instead of waiting for the next draw/Query.
        const uint32_t f = g_frame.load(std::memory_order_relaxed);
        if (c.curCount > 0 && c.curFrame < f) RollFrame(c);
        if (c.ccurCount > 0 && c.ccurFrame < f) RollChunks(c);
        if (c.cdoneCount > c.consumedChunks) {
            c.state = Watch::kDone;
            g_activeCount.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
    // No watch for this key: register a background hunt (same slot policy
    // as Query's registration; never evicts an active watch).
    Watch* victim = nullptr;
    for (Watch& c : g_watches) {
        if (c.state == Watch::kFree) { victim = &c; break; }
        if (c.state != Watch::kActive &&
            (!victim || c.registeredTick < victim->registeredTick)) {
            victim = &c;
        }
    }
    if (!victim) return false;
    *victim = Watch{};
    victim->state = Watch::kActive;
    victim->upgradeHunt = true;
    victim->buffer = buffer;
    victim->srv = srv;
    victim->key = key;
    victim->expectedBytes = recordCount * 80u;
    for (int s = 0; s < 4; ++s) {
        if (segTris[s] && segTris[s] < 0x400000u) {
            victim->expectedIdx[victim->nExpected++] = segTris[s] * 3u;
        }
    }
    victim->registeredTick = now;
    victim->registeredFrame = g_frame.load(std::memory_order_relaxed);
    g_activeCount.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void MarkConsumed(uint64_t key) {
    std::lock_guard<std::mutex> g(g_lock);
    for (Watch& c : g_watches) {
        if (c.state == Watch::kFree || c.key != key) continue;
        // Budget only burns on GROWTH: post-cull re-resolves re-consume the
        // same union and shouldn't count against the shape.
        if (c.cdoneCount > c.consumedChunks) ++c.upgradesServed;
        c.consumedChunks = c.cdoneCount;
        if (c.state != Watch::kActive) {
            c.state = Watch::kActive;
            c.upgradeHunt = true;
            c.registeredTick = GetTickCount64();
            c.registeredFrame = g_frame.load(std::memory_order_relaxed);
            g_activeCount.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
}

void ResetAll() {
    std::lock_guard<std::mutex> g(g_lock);
    g_boundT8.store(nullptr, std::memory_order_relaxed);
    for (Watch& c : g_watches) {
        c = Watch{};
    }
    g_activeCount.store(0, std::memory_order_relaxed);
    _MESSAGE("FO4RemixPlugin: [DrawCap] ResetAll (reload): watches purged");
}

int GetChunks(uint64_t key, ChunkDraw* out, int maxOut) {
    std::lock_guard<std::mutex> g(g_lock);
    for (Watch& c : g_watches) {
        if (c.state == Watch::kFree || c.key != key) continue;
        const int n = c.cdoneCount < maxOut ? c.cdoneCount : maxOut;
        std::memcpy(out, c.cdone, sizeof(ChunkDraw) * (size_t)n);
        return n;
    }
    return 0;
}

}  // namespace DrawCapture
