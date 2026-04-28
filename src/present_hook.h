#pragma once

namespace PresentHook {
    bool Install();
    void Uninstall();
    // Reset state when a new save game is loaded.
    // Clears tracked skinned meshes (bone pointers invalidated) and UI RT detection.
    void ResetExtractionState();
}
