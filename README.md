# FO4 RTX Remix Plugin

An F4SE plugin for *Fallout 4* that hooks `IDXGISwapChain::Present`, extracts
loaded scene geometry, textures, and lights from the running game, and submits
them to the [RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix) C API for
path-traced rendering in a separate window.

## Status

Pre-release / experimental. Core extraction (statics, terrain, lights, water,
time-of-day) works; full weather (storms, fog) and a few other systems are still
in progress. See [KNOWLEDGEBASE.md](KNOWLEDGEBASE.md) for the up-to-date list
of what's wired and what isn't.

## Requirements

- **Fallout 4** — runtime versions targeted: `1.10.163`, `1.10.980`, `1.11.191`.
  The CMake build is currently pinned to `1.11.191` via the `RUNTIME_VERSION`
  define; building against an older runtime requires changing that define and
  pulling the matching F4SE SDK headers.
- **F4SE** (Fallout 4 Script Extender) — install per the F4SE instructions.
  This plugin is loaded by F4SE.
- **RTX Remix runtime** — install the Remix runtime (the `.trex` /
  `d3d9.dll` shim) for *Fallout 4* per the RTX Remix project's instructions.
  The plugin links against the Remix C API and submits scene data to it.
- **Visual Studio 2022** with the Desktop C++ workload (CMake, MSVC).

## Build

From the repository root:

```bat
build.bat
```

Or directly with CMake:

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The output DLL lands at `build\Release\FO4RemixPlugin.dll`.

The build expects the F4SE SDK to live at `..\f4se-0.7.7` (sibling to this
repository). At configure time, CMake will refresh `extern\remix\remix_c.h` from
a sibling `dxvk-remix\` checkout if one is present; otherwise the cached
snapshot in `extern\remix\` is used.

## Install

Copy the built DLL into your F4SE plugins directory:

```
<Fallout 4 install>\Data\F4SE\Plugins\FO4RemixPlugin.dll
```

## Configuration

Plugin settings live in `FO4RemixPlugin.ini`, which must sit next to the DLL in
`Data\F4SE\Plugins\`. A sample is included at the repository root.

The file controls extraction logging verbosity, light-system tuning (master
toggle, intensity, radius, color saturation), and per-shape extent limits.
Per-key documentation is in [KNOWLEDGEBASE.md](KNOWLEDGEBASE.md).

## Documentation

[KNOWLEDGEBASE.md](KNOWLEDGEBASE.md) — comprehensive technical reference
(architecture, threading model, per-source-file responsibilities, engine
offsets, extraction pipeline, Remix integration, coordinate conventions,
configuration, known limitations).

## License

No license is currently attached to this repository. Until one is added,
treat the source as all-rights-reserved by the authors.
