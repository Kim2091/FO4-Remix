#pragma once

namespace PresentHook {
    bool Install();
    void Uninstall();
    // Reset state when a new save game is loaded.
    // Clears tracked skinned meshes (bone pointers invalidated) and UI RT detection.
    void ResetExtractionState();

    // Latest process-local adapter memory reading (DXGI QueryVideoMemoryInfo),
    // refreshed every ~60 game frames by the present hook. Returns false until
    // the first successful query. Feeds the VRAM-pressure force-eviction in
    // SemanticCapture::Tick (2026-07-20).
    bool GetVramBudgetSnapshot(uint64_t* usedMiB, uint64_t* budgetMiB);
}
