#pragma once

// Diagnostic instrumentation for the FO4 Remix plugin. Owns the canonical
// frame counter (consumed by the material-LRU sweep) and emits periodic
// log lines so plugin behavior is observable without attaching a debugger.
//
// Cadence: emit on frames 1..10 (warmup, every frame) and then every
// 300th frame thereafter (~5s @ 60fps). Same as Skyrim.

#include <cstdint>
#include <string>

namespace Diagnostics {

// Atomically bump the frame counter. Returns the new value. Call once per
// Present (the Present hook is the canonical per-frame tick).
uint64_t Tick();

// Read the current frame counter without incrementing. Cheap. Used by the
// material-LRU sweep and by EmitPeriodic.
uint64_t CurrentFrameIndex();

// True on frames where EmitPeriodic should run.
bool ShouldEmitPeriodic(uint64_t frameIndex);

// Cumulative counters bumped by call sites in the present hook + cell pipeline.
// Read by EmitPeriodic. All operations are atomic; no lock taken.
void OnCellLoaded(uint32_t cellID, size_t meshes, size_t skinned,
                  size_t textures, size_t lights);
void OnCellUnloaded(uint32_t cellID);

// Snapshot of game state for the periodic [GameState] log line. Reads
// PlayerCharacter/UI singletons; must be called from the game thread
// (during Present hook).
struct GameStateSnapshot {
    std::string cellName;
    uint32_t    cellFormID  = 0;
    bool        cellInterior = false;
    bool        anyMenuOpen = false;
    float       playerX = 0.0f;
    float       playerY = 0.0f;
    float       playerZ = 0.0f;
};
GameStateSnapshot SnapshotGameState();

// Emit the per-period log block: [GameState] + [Plugin] summary. Caller
// MUST guard with ShouldEmitPeriodic(frameIndex) -- this function does not
// re-check the cadence.
void EmitPeriodic(uint64_t frameIndex, const GameStateSnapshot& gs);

}  // namespace Diagnostics
