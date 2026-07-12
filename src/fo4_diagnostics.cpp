#include "fo4_diagnostics.h"
#include "bs_extraction.h"
#include "config.h"

#include "f4se/PluginAPI.h"  // _MESSAGE

#include <atomic>
#include <cstdint>

namespace {

// Canonical frame counter. Incremented by Tick(); read by sweep + emit.
std::atomic<uint64_t> g_frameIndex{0};

// Cumulative cell counters. Bumped by OnCellLoaded; read by EmitPeriodic.
std::atomic<uint64_t> g_cumCellsLoaded{0};
std::atomic<uint64_t> g_cumCellsUnloaded{0};
std::atomic<uint64_t> g_cumMeshesExtracted{0};
std::atomic<uint64_t> g_cumSkinnedMeshes{0};
std::atomic<uint64_t> g_cumTexturesExtracted{0};
std::atomic<uint64_t> g_cumLightsExtracted{0};

constexpr uint64_t kFrameLogWarmupCount = 10;
constexpr uint64_t kFrameLogInterval    = 300;

// Per-event cell lines are the classic churn-spam source (fast travel /
// boundary walking attach+detach storms, each line an fflush on the game
// thread). Counters always accumulate -- EmitPeriodic reports them every
// 300 frames -- but the per-event line is gated on Diagnostics.Enabled and
// budgeted: the first kCellLogBudget events log verbatim, then 1 in
// kCellLogSample so a long session stays sampled rather than silent.
constexpr uint64_t kCellLogBudget = 50;
constexpr uint64_t kCellLogSample = 32;
inline bool CellEventShouldLog(uint64_t eventOrdinal) {
    return g_config.diagEnabled &&
           (eventOrdinal <= kCellLogBudget ||
            (eventOrdinal % kCellLogSample) == 0);
}

}  // namespace

namespace Diagnostics {

uint64_t Tick() {
    return g_frameIndex.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint64_t CurrentFrameIndex() {
    return g_frameIndex.load(std::memory_order_relaxed);
}

bool ShouldEmitPeriodic(uint64_t frameIndex) {
    if (frameIndex == 0) return false;
    if (frameIndex <= kFrameLogWarmupCount) return true;
    return (frameIndex % kFrameLogInterval) == 0;
}

void OnCellLoaded(uint32_t cellID, size_t meshes, size_t skinned,
                  size_t textures, size_t lights) {
    const uint64_t n = g_cumCellsLoaded.fetch_add(1, std::memory_order_relaxed) + 1;
    g_cumMeshesExtracted.fetch_add(meshes, std::memory_order_relaxed);
    g_cumSkinnedMeshes.fetch_add(skinned, std::memory_order_relaxed);
    g_cumTexturesExtracted.fetch_add(textures, std::memory_order_relaxed);
    g_cumLightsExtracted.fetch_add(lights, std::memory_order_relaxed);
    if (CellEventShouldLog(n)) {
        _MESSAGE("FO4RemixPlugin: [Diag] CellLoaded #%llu id=0x%08X meshes=%zu skinned=%zu textures=%zu lights=%zu",
                 (unsigned long long)n, cellID, meshes, skinned, textures, lights);
    }
}

void OnCellUnloaded(uint32_t cellID) {
    const uint64_t n = g_cumCellsUnloaded.fetch_add(1, std::memory_order_relaxed) + 1;
    if (CellEventShouldLog(n)) {
        _MESSAGE("FO4RemixPlugin: [Diag] CellUnloaded #%llu id=0x%08X",
                 (unsigned long long)n, cellID);
    }
}

// SEH-guarded read of the cell identity fields: the player-cell pointer can
// be non-null yet mid-teardown during load/unload transitions, and this is
// a diagnostics path -- never worth an AV on the game thread.
static bool ReadCellIdentity(uintptr_t cellPtr, uint32_t* formID, uint32_t* flags) {
    __try {
        *formID = *reinterpret_cast<const uint32_t*>(cellPtr + 0x14);  // TESForm::formID
        *flags  = *reinterpret_cast<const uint32_t*>(cellPtr + 0x40);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

GameStateSnapshot SnapshotGameState() {
    GameStateSnapshot ctx{};

    // Cell identity — formID and interior flag.
    uintptr_t cellPtr = BsExtraction::GetPlayerCellPtr();
    uint32_t formID = 0, cellFlags = 0;
    if (cellPtr && ReadCellIdentity(cellPtr, &formID, &cellFlags)) {
        ctx.cellFormID   = formID;
        ctx.cellInterior = cellFlags & 1;  // flags bit 0
    }

    // Player world position (TESObjectREFR::pos at +0xD0).
    BsExtraction::GetPlayerPosition(ctx.playerX, ctx.playerY, ctx.playerZ);

    // cellName and anyMenuOpen deferred (require deeper singleton access).
    return ctx;
}

void EmitPeriodic(uint64_t frameIndex, const GameStateSnapshot& gs) {
    if (!g_config.diagEnabled) return;
    // [GameState] -- tells the log reader what frame the summary applies to,
    // which cell, and whether the player is in-game vs. paused at a menu.
    _MESSAGE("FO4RemixPlugin: [GameState] frame=%llu cellID=0x%08X interior=%d anyMenu=%d playerPos=(%.1f,%.1f,%.1f)",
             (unsigned long long)frameIndex,
             gs.cellFormID,
             gs.cellInterior ? 1 : 0,
             gs.anyMenuOpen ? 1 : 0,
             gs.playerX, gs.playerY, gs.playerZ);

    // [Plugin] -- cumulative counters + cell-load activity.
    _MESSAGE("FO4RemixPlugin: [Plugin] frame=%llu cellsLoaded=%llu cellsUnloaded=%llu meshesExtracted=%llu skinnedMeshes=%llu textures=%llu lights=%llu",
             (unsigned long long)frameIndex,
             (unsigned long long)g_cumCellsLoaded.load(std::memory_order_relaxed),
             (unsigned long long)g_cumCellsUnloaded.load(std::memory_order_relaxed),
             (unsigned long long)g_cumMeshesExtracted.load(std::memory_order_relaxed),
             (unsigned long long)g_cumSkinnedMeshes.load(std::memory_order_relaxed),
             (unsigned long long)g_cumTexturesExtracted.load(std::memory_order_relaxed),
             (unsigned long long)g_cumLightsExtracted.load(std::memory_order_relaxed));
}

}  // namespace Diagnostics
