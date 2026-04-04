#pragma once

namespace PresentHook {
    bool Install();
    void Uninstall();
    // Reset extraction state when a new save game is loaded.
    // Clears tracked cells and forces re-extraction with the initial bootstrap path.
    void ResetExtractionState();
}
