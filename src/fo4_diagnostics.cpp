#include "fo4_diagnostics.h"

#include "f4se/PluginAPI.h"  // _MESSAGE
// FO4 game-singleton headers needed for SnapshotGameState. Adapt the include
// paths to whatever scene_extractor.cpp uses for PlayerCharacter / TES.
// Placeholder includes; refine when wiring SnapshotGameState in B3.

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
    g_cumCellsLoaded.fetch_add(1, std::memory_order_relaxed);
    g_cumMeshesExtracted.fetch_add(meshes, std::memory_order_relaxed);
    g_cumSkinnedMeshes.fetch_add(skinned, std::memory_order_relaxed);
    g_cumTexturesExtracted.fetch_add(textures, std::memory_order_relaxed);
    g_cumLightsExtracted.fetch_add(lights, std::memory_order_relaxed);
    _MESSAGE("FO4RemixPlugin: [Diag] CellLoaded id=0x%08X meshes=%zu skinned=%zu textures=%zu lights=%zu",
             cellID, meshes, skinned, textures, lights);
}

void OnCellUnloaded(uint32_t cellID) {
    g_cumCellsUnloaded.fetch_add(1, std::memory_order_relaxed);
    _MESSAGE("FO4RemixPlugin: [Diag] CellUnloaded id=0x%08X", cellID);
}

// SnapshotGameState + EmitPeriodic land in B3 + B4. Stubs so the file
// links after this commit:

GameStateSnapshot SnapshotGameState() {
    return GameStateSnapshot{};
}

void EmitPeriodic(uint64_t frameIndex, const GameStateSnapshot& /*gs*/) {
    _MESSAGE("FO4RemixPlugin: [Diag] periodic frame=%llu cellsLoaded=%llu cellsUnloaded=%llu meshes=%llu skinned=%llu textures=%llu lights=%llu",
             (unsigned long long)frameIndex,
             (unsigned long long)g_cumCellsLoaded.load(std::memory_order_relaxed),
             (unsigned long long)g_cumCellsUnloaded.load(std::memory_order_relaxed),
             (unsigned long long)g_cumMeshesExtracted.load(std::memory_order_relaxed),
             (unsigned long long)g_cumSkinnedMeshes.load(std::memory_order_relaxed),
             (unsigned long long)g_cumTexturesExtracted.load(std::memory_order_relaxed),
             (unsigned long long)g_cumLightsExtracted.load(std::memory_order_relaxed));
}

}  // namespace Diagnostics
