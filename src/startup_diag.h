#pragma once

namespace StartupDiag {

// One-shot startup dump emitted right after config load. Logs:
//   - OS version (via RtlGetVersion, bypasses GetVersion's app-compat lie)
//   - Process working set + thread count
//   - Plugin DLL path / size / mtime
//   - Fallout4.exe path / size / mtime
//   - Module ownership of d3d9 / d3d11 / dxgi / dinput8 (flags ENB / ReShade
//     wrappers in the game folder vs. system-loaded)
//   - Detected overlays (RTSS, Afterburner, NVIDIA, Steam, Discord, ReShade,
//     Special K, Fraps, OBS)
//
// GPU adapter info is intentionally NOT collected here -- no D3D device
// exists yet at plugin load.
void DumpEnvironment();

}  // namespace StartupDiag
