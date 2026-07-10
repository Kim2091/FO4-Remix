# FO4 RTX Remix Plugin — Technical Reference

## Overview

`FO4RemixPlugin` is an F4SE plugin (`FO4RemixPlugin.dll`) that runs inside the
Fallout 4 process and produces a path-traced view of the live scene through
NVIDIA's RTX Remix runtime. The plugin:

- Hooks `IDXGISwapChain::Present` (DXGI vtable index 8) on the game's swap chain.
- Hooks two `BSShaderProperty::GetRenderPasses` slots inside `Fallout4.exe`
  (RVA `0x02172540` for lighting, RVA `0x021D15A0` for water) to capture
  per-drawable geometry/material/transform data from the engine itself.
- Optionally hooks `ID3D11DeviceContext::OMSetRenderTargets` (vtable index 33)
  and `ClearRenderTargetView` (vtable index 50) to detect the game's UI
  render target and copy it to a Remix screen overlay.
- Loads `d3d9.dll` from the game directory (the Remix runtime ships under that
  name), creates a hidden D3D9Ex device, and registers it via Remix's
  `dxvk_RegisterD3D9Device` extension. A separate `Fallout 4 - RTX Remix`
  window receives the path-traced output.
- Pumps a dedicated render thread (`RemixThreadFunc`) that drives Remix at
  ~60 fps independent of the game's frame pacing.

### Target runtime versions

The version sentinel array in `main.cpp:77` advertises support for
`RUNTIME_VERSION_1_10_163`, `RUNTIME_VERSION_1_10_980`, and
`RUNTIME_VERSION_1_11_191`. The CMake build is currently compiled against
`RUNTIME_VERSION_1_11_191` (set on both the F4SE shim library and the plugin
DLL — see `CMakeLists.txt:56` and `:108`). Address-library compatibility flags
declared in `F4SEPlugin_Version` cover `AddressLibrary_1_10_980` and
`AddressLibrary_1_11_137`; structure-independence flags cover
`1_10_980Layout` and `1_11_137Layout`.

## Architecture

```
                        Fallout4.exe (game thread)
                                     |
                                     | (1) Present hook       (2) GetRenderPasses hooks
                                     v                                 |
                  +------------------+--------------------+            |
                  | hkPresent (present_hook.cpp)          |            |
                  |  - frame tick + diagnostics           |            |
                  |  - publish CameraState                |<-----------+ (event-driven
                  |  - WeatherBridge::PushOncePerFrame    |              capture writes
                  |  - SemanticCapture::Tick(device)      |---+          DrawableState
                  |  - UI RT detection + overlay copy     |   |          into g_drawableMap)
                  |  - launch RemixThread on first call   |   |
                  +-------+-----------+-------------------+   |
                          |           |                       |
                          | shared    | shared                | resolve loop:
                          | camera    | overlay               | parse BSTriShape,
                          v           v                       | extract textures,
                                                              | call SubmitDrawable
                  +------------------+--------------------+   |
                  |  RemixThreadFunc (Remix thread)       |   v
                  |   - RemixAPI::Initialize / Shutdown   | Resolvers::Lighting
                  |   - RemixRenderer::Init / Shutdown    | Resolvers::Water
                  |   - RemixRenderer::OnFrame(cam,ovl)   |   |
                  |     - SetupCamera                     |   v
                  |     - bucket draws by mesh handle     | RemixRenderer::SubmitDrawable
                  |     - DrawInstance per bucket         |   - texture upload + cache
                  |     - LRU sweeps                      |   - material cache (opaque/
                  |     - api->Present                    |     translucent)
                  +------------------+--------------------+   - mesh cache
                                     |                        - g_drawables map entry
                                     v
                              dxvk-remix runtime
                                  (Vulkan)
                                     |
                                     v
                            Remix output window
```

The pipeline has two writer paths into Remix:

1. **Per-frame on the Remix thread** — `RemixRenderer::OnFrame` issues camera
   setup, walks `g_drawables`, batches by mesh handle, and emits
   `DrawInstance` calls plus `Present`.
2. **Event-driven on the game thread** — the `GetRenderPasses` detours record
   per-drawable state into `g_drawableMap`. `SemanticCapture::Tick` (called
   from `hkPresent`) runs the resolver loop, which builds `ExtractedMesh`/
   `ExtractedTexture` objects and calls `RemixRenderer::SubmitDrawable`.
   Submissions populate `g_drawables`, `g_meshCache`, `g_materialCache`, and
   `g_textureHandles` for the OnFrame loop to consume.

## Threading model

There are two long-lived threads of interest:

- **Game thread** — hosts `hkPresent`, all `GetRenderPasses` detours, and
  `SemanticCapture::Tick` (resolver loop + TTL sweep). Reads/writes
  `g_drawableMap` under `g_drawableMutex`. Calls `RemixRenderer::SubmitDrawable`
  / `ReleaseDrawable`, both of which acquire `g_renderStateMutex`.
- **Remix thread** — created lazily on the first `hkPresent` invocation
  (`present_hook.cpp:275`). Runs `RemixThreadFunc`, which initialises the
  Remix API and renderer, then loops `OnFrame` at ~60 Hz with
  `Sleep(16)`. Owns `RemixRenderer::OnFrame`, the LRU sweeps, and the Remix
  `Present` call.

Mutexes:

| Mutex | Type | Owner | Protects |
|-------|------|-------|----------|
| `g_remix.cameraMutex` (`present_hook.cpp:49`) | `std::mutex` | shared | the published `CameraState` snapshot read by the Remix thread |
| `g_overlay.mutex` (`present_hook.cpp:63`) | `std::mutex` | shared | the captured UI overlay pixel buffer |
| `g_drawableMutex` (`semantic_capture.cpp:120`) | `std::mutex` | shared | `g_drawableMap` (PassKey -> DrawableState) |
| `g_renderStateMutex` (`remix_renderer.cpp:262`) | `std::mutex` | shared | `g_drawables`, `g_meshCache`, `g_materialCache`, `g_textureHandles` |
| `g_remixApiMutex` (`remix_renderer.cpp:254`) | `std::recursive_mutex` | Remix thread only | Remix-API option-registry writes vs. `OnFrame` draw submissions. NEVER acquire from the game thread: OnFrame holds it for the whole frame (incl. the multi-ms Present) and re-acquires immediately, so an unfair-lock waiter starves for seconds (the 2026-07-02 "freezes until alt-tab" bug) |
| `g_configQueueMutex` (`remix_renderer.cpp`) | `std::mutex` | shared | `g_pendingConfigVars` + `g_configFailedKeys`; held only for map ops, never across a Remix API call. Game-thread config writes go through `QueueConfigVariable`; OnFrame drains |

Deferred handle destruction (2026-07-10): game-thread release paths
(`ReleaseDrawable` / `DecrementMeshCacheRef`) never call
`DestroyMesh/DestroyMaterial/DestroyTexture` inline — a destroy landing
between OnFrame's `DrawInstance` records and the `Present` that consumes
them invalidates a handle the in-flight frame still references (the
runtime's Present-side `.at()` throws `std::out_of_range`, the 0xc0000409
CTD class). Handles are parked in `g_pendingDestroys` (guarded by
`g_renderStateMutex`, gated by the `g_hasPendingDestroys` atomic) and
destroyed at the top of the next `OnFrame` on the Remix thread. Creates on
the game thread are safe: every remixapi entry point locks the runtime's
internal `s_mutex`.

CRITICAL invariant: remixapi handles are HASH-VALUED in this fork
(`rtx_remix_api.cpp` reinterpret_casts `info->hash` into the handle and the
runtime IGNORES repeated registrations of a live handle). Consequences the
code depends on: (1) `SubmitDrawable` must `CancelParkedHandle` on every
create so a re-created identical resource isn't killed by its predecessor's
still-parked destroy; (2) the OnFrame drain holds `g_renderStateMutex`
ACROSS the destroy calls so a concurrent re-create can't interleave; (3)
mesh handles ALIAS across `g_meshCache` buckets that share `contentHash`
with different materials — `DecrementMeshCacheRef` must not park a handle
another bucket still uses.

Lock-order rules (documented in `remix_renderer.cpp:253-263` and
`remix_renderer.cpp:993-995`):

1. `g_remixApiMutex` is acquired first when held alongside any other
   render-state mutex.
2. Within `OnFrame`: `g_remixApiMutex` -> `g_drawableMutex` (taken inside
   `SnapshotActiveDrawables`) -> `g_renderStateMutex` (the bucket-build /
   draw block). Never reverse.
3. The semantic-capture resolve loop takes `g_drawableMutex` and, inside the
   call to `SubmitDrawable`, takes `g_renderStateMutex`. This is consistent
   with the OnFrame order.
4. `WeatherBridge::PushOncePerFrame` calls
   `RemixRenderer::QueueConfigVariable`, which takes only the lightweight
   `g_configQueueMutex` (a map insert). OnFrame drains the queue on the
   Remix thread while it already holds `g_remixApiMutex`. The blocking
   `SetConfigVariable` is Remix-thread-only (see mutex table).

The Present hook itself does not hold any plugin mutex while calling the
original `IDXGISwapChain::Present` (`present_hook.cpp:464`).

## Per-source-file responsibilities

| File(s) | Responsibility |
|---------|---------------|
| `src/main.cpp` | F4SE plugin entry. Declares `F4SEPlugin_Version`, registers the F4SE messaging listener, installs the Present hook on `kMessage_GameDataReady` / `GameLoaded` / `InputLoaded`, clears the SemanticCapture map on `kMessage_PreLoadGame`. Owns `g_gameDataReady`. |
| `src/config.{cpp,h}` | INI loader (`LoadConfig`) backed by `GetPrivateProfileXxxA`. Defines `PluginConfig g_config` plus shared inline helpers `HalfToFloat`, `FnvHash`, `FnvHashCombine`. |
| `src/present_hook.{cpp,h}` | DXGI Present hook (`hkPresent`) installation via MinHook on a dummy swap chain. Manages the Remix render thread, shared camera/overlay state, the UI render-target detection (`hkClearRenderTargetView` + `hkOMSetRenderTargets`), and the per-frame staging-texture copy + premultiplied-alpha unpremultiply. Calls `Diagnostics::Tick`, `WeatherBridge::PushOncePerFrame`, `SemanticCapture::Tick`, and `RemixAPI::RestoreLegacyKeyboardInput` / `RebindRawInputToGameWindow`. |
| `src/remix_api.{cpp,h}` | Wraps `remixapi_lib_loadRemixDllAndInitialize`, builds the dedicated Remix output window, prefers the `dxvk_CreateD3D9` / `dxvk_RegisterD3D9Device` path to bypass the runtime's default-init dev-menu overlay, and falls back to `Startup()` if the dxvk extensions are missing. Owns the singleton `remixapi_Interface`. Provides `RestoreLegacyKeyboardInput` (RIDEV_REMOVE for keyboard) and `RebindRawInputToGameWindow` (re-claims raw-input for the game HWND every frame). |
| `src/remix_renderer.{cpp,h}` | Per-frame `OnFrame` loop, mesh/material/texture caches with refcounts, `SubmitDrawable` / `ReleaseDrawable`, LRU sweeps (`SweepStaleMaterials`, `SweepStaleTextures`), VRAM telemetry (`GetVramStats`), DXGI -> remixapi format mapping, fallback triangle, screen-overlay submission, `SetConfigVariable` wrapper. SEH + C++ exception fences around every `api->DrawInstance` / `Create*` / `Destroy*` call. |
| `src/camera.{cpp,h}` | `Camera::Get()` reads `g_playerCamera` (F4SE RelocPtr), pulls `cameraNode->m_worldTransform`, applies the Beth -> Remix X/Y swap, and snapshots player world position for the LOD chunk spatial filter. Returns a fallback `CameraState` when the singleton is unavailable. |
| `src/bs_extraction.{cpp,h}` | Engine-pointer reads (player, DataHandler, TES singleton, GridCellArray), `BsExtraction::GetLoadedCells`, BC1/BC2/BC3/BC5 software decompressors, smoothness-to-roughness inversion, normal-to-octahedral encoding, mip-chain readback (`ReadbackAllMips`), `ParseShapeGeometry` (BSTriShape vertex/index parse with half-float/full-precision branches and BSDynamicTriShape morphed-position handling), `GetLightingMaterial`, `ExtractMaterialTexture` (with deterministic FNV-of-name hashing + texture cache), `ExtractEmissiveData`, `ExtractAlphaState` (NiAlphaProperty -> VkBlendFactor / VkCompareOp). Also defines `TexturePostProcess` enum and `ParsedGeometry`/`ExtractedMesh`/`ExtractedTexture`/`CellInfo` structs. |
| `src/fo4_diagnostics.{cpp,h}` | Canonical `Diagnostics::Tick` / `CurrentFrameIndex` (atomic frame counter), `ShouldEmitPeriodic` cadence (every frame for first 10 then every 300th), cumulative cell counters, `SnapshotGameState` (cell formID/interior + player position), `EmitPeriodic` (writes `[GameState]` + `[Plugin]` lines). |
| `src/startup_diag.{cpp,h}` | One-shot `DumpEnvironment` called at plugin load: OS version via `RtlGetVersion`, working set, plugin DLL + Fallout4.exe stat, module-ownership scan for d3d9/d3d11/dxgi/dinput8, overlay detection (RTSS, Afterburner, NVIDIA, Steam, Discord, ReShade, Special K, Fraps, OBS). |
| `src/semantic_capture.{cpp,h}` | MinHook installation of two `GetRenderPasses` detours (Lighting RVA `0x02172540`, Water RVA `0x021D15A0`) plus two diagnostic-only hooks (`SetupGeometry` `0x02233730`, CB-write `0x022347D0`). Computes the FNV PassKey from `(geometry, property, material)`, stores `DrawableState` in `g_drawableMap`, captures live `m_worldTransform` and `NiAVObject::flags` per fire. `Tick(device)` runs the per-frame resolve loop (freshness-gated, VRAM-gated), the rate-limited TTL sweep, the pending-by-gate breakdown log, and the active-set snapshot. `BuildRemixTransform` converts NiTransform to Remix row-major 3x4 with the X/Y swap. `ClearDrawableMap` is called from `kMessage_PreLoadGame`. |
| `src/resolvers/lighting_static.{cpp,h}` | Resolver for `BSLightingShaderProperty` drawables. Parses the BSTriShape, rejects skinned and landscape materials (current scope), tags worldspace LOD chunks via `parent1.name == "chunk"` + `parent2.name in {"4","8","16","32"}` (or `parent2.name == "obj"`), pulls diffuse/normal/roughness/emissive textures with the appropriate `TexturePostProcess` (Octahedral for normals, InvertRGB for smoothness->roughness), submits to Remix. Owns the `Resolvers::Trace` step/hash trace globals consumed by the SEH handler in `semantic_capture.cpp`. |
| `src/resolvers/water.{cpp,h}` | Resolver for `BSWaterShaderProperty` drawables. Submits as translucent with synthetic 1x1 RGBA8 blue diffuse (sentinel hash `0xFA11FA11FA11FA11`) plus the water material's `spNormalMap01` (Octahedral) and `kDeepColor` as `transmittanceColor`. Sets `mesh.isWater = true` so `SubmitDrawable` builds a `MaterialInfoTranslucentEXT` chain and `OnFrame` ORs `REMIXAPI_INSTANCE_CATEGORY_BIT_ANIMATED_WATER` into the bucket's `categoryFlags`. |
| `src/weather_bridge.{cpp,h}` | `PushOncePerFrame()` reads the GameHour TESGlobal (formID `0x00000038`), derives `sunElevation = sin((hour-6)/12 * pi) * 90` and `sunRotation = (hour/24) * 360`, and pushes both via `RemixRenderer::SetConfigVariable("rtx.atmosphere.sunElevation", ...)` / `("rtx.atmosphere.sunRotation", ...)`. Per-key failure dedup so a missing slot in the runtime fork doesn't spam the log. Time-of-day only; storms/fog/volumetric fog/isInterior are not wired (see Known limitations). |
| `src/f4se_compat.h` | Minimal compatibility layer that lets us avoid the full `xse-common` dependency. Provides `UInt8/16/32/64` and `SInt*` typedefs, `STATIC_ASSERT`, `_MESSAGE` / `_WARNING` / `_ERROR` macros (all funnel into a `My Games\Fallout4\F4SE\FO4RemixPlugin.log` file), `ASSERT`. Force-included via `/FI` from CMake. |
| `src/cell_pipeline.{cpp,h}` | **Paused / not in build.** Pre-Phase-1B cell-granular state machine for batched per-cell extraction and Remix loading. Replaced by the event-driven semantic-capture path. Files remain in the tree for reference but are NOT compiled by `CMakeLists.txt`. |

## Build system

- Build target: `FO4RemixPlugin` SHARED library, `PREFIX=""`, `OUTPUT_NAME="FO4RemixPlugin"`. Link result: `build/Release/FO4RemixPlugin.dll`.
- Toolchain: C++17, MSVC static runtime (`MultiThreaded`), VS 2022 generator (per `build.bat`).
- F4SE SDK: expected as a sibling directory at `${CMAKE_CURRENT_SOURCE_DIR}/../f4se-0.7.7`. CMake adds an internal `f4se_minimal` static library that compiles only the three SDK sources we need (`Relocation.cpp`, `GameCamera.cpp`, `GameForms.cpp`) and force-includes `f4se_compat.h` via `/FI`.
- MinHook: pulled in via `FetchContent_Declare(GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git)` at the `master` tag.
- Link deps: `f4se_minimal`, `minhook`, `d3d9`, `d3d11`, `dxgi`, `shell32`, `psapi`.
- `remix_c.h` refresh: `CMakeLists.txt:24-42` searches a list of candidate paths for the `dxvk-remix` checkout (`./dxvk-remix`, `../dxvk-remix`, `../../dxvk-remix`, `../../../dxvk-remix`, then the legacy `../dxvk-remix-gmod`) and copies the in-tree `public/include/remix/remix_c.h` into `extern/remix/`. If no source is found, the build falls back to the cached snapshot already at `extern/remix/remix_c.h`. The copy runs at *configure* time, so an upstream ABI change picked up by `git pull` is reflected on the next `cmake --build` without manual cache deletion.
- Runtime version macro: `RUNTIME_VERSION=RUNTIME_VERSION_1_11_191` is set as a `target_compile_definition` on both `f4se_minimal` and `FO4RemixPlugin`.
- Build entry point: `build.bat` runs `cmake -B build -G "Visual Studio 17 2022" -A x64` then `cmake --build build --config Release`. The CLAUDE.md notes a faster incremental command using the bundled VS CMake binary directly.

## Engine offsets and structs

All offsets verified against the source where they are read; no copy from
CLAUDE.md is trusted. Citations point to the file/line that performs the
deref.

### TES singleton

```
Fallout4.exe RelocPtr (RVA): 0x032D2048      // bs_extraction.cpp:78
TES + 0x18: GridCellArray*                   // bs_extraction.cpp:79 (OFF_TES_GRID_CELLS)
```

### GridCellArray

```
+0x10  int32   gridDimension     // == uGridsToLoad   (bs_extraction.cpp:80)
+0x18  TESObjectCELL** cellArray // flat dim*dim row-major (bs_extraction.cpp:81)
```

`CollectGridCells` reads dim and the flat array, then iterates `dim*dim`
entries (`bs_extraction.cpp:1214-1263`).

### TESForm

```
+0x14  uint32  formID            // bs_extraction.cpp:64 (OFF_FORM_ID)
+0x1A  uint8   formType          // (per CLAUDE.md; not directly read by source under review)
```

### TESObjectREFR

```
+0xB8  TESObjectCELL* parentCell // bs_extraction.cpp:65 (OFF_REFR_PARENT_CELL)
+0xD0  NiPoint3       pos        // bs_extraction.cpp:66 (OFF_REFR_POS)
+0xF0  LoadedData*    unkF0      // bs_extraction.cpp:67 (OFF_REFR_LOADED_DATA)
LoadedData+0x08: NiNode* rootNode // bs_extraction.cpp:68 (OFF_LOADED_ROOT_NODE)
```

### TESObjectCELL

```
+0x40  uint32 flags              // bit 0 = interior      (bs_extraction.cpp:70 / 1284)
+0x58  TESObjectLAND* land       // bs_extraction.cpp:71 (OFF_CELL_LAND)
+0x70  tArray<TESObjectREFR*> objectList  // bs_extraction.cpp:69 (OFF_CELL_OBJECT_LIST)
+0xC8  TESWorldSpace* worldSpace // bs_extraction.cpp:72 (OFF_CELL_WORLD_SPACE)
CELL_FLAG_IS_INTERIOR = 0x0001    // bs_extraction.cpp:73
```

### TESObjectLAND

```
+0x40  BSMultiBoundNode* quadrants[4]   // bs_extraction.cpp:74-75 (OFF_LAND_QUADRANTS)
```

The land-quadrant offsets are declared but currently not consumed by the
event-driven pipeline (terrain regression accepted; see Known limitations).

### NiAVObject (geometry leaf)

Read in the `GetRenderPasses` detour (`semantic_capture.cpp:280-298`):

```
+0x28  NiAVObject* m_parent
+0x70  NiTransform m_worldTransform   (rotation 3x3 + position 3f + scale)
+0x108 uint64      flags              (kFlagIsMeshLOD bit 12, kFlagFadedIn bit 37, ...)
```

### BSGeometry

`+0x130  NiAlphaProperty* effectState` (read at `lighting_static.cpp:279` for
the alpha-test diagnostic). `BsExtraction::ExtractAlphaState` decodes
`NiAlphaProperty::alphaFlags` (bit 0 enable, bits 1-4 src factor, bits 5-8
dst factor, bit 9 alpha-test enable, bits 10-12 test function) and translates
them to Vulkan factor/compare-op enums (`bs_extraction.cpp:1085-1136`).

### BSShaderProperty / BSLightingShaderProperty

`+0x58  BSShaderMaterial* shaderMaterial` (read in the shared
`GetRenderPasses` detour, `semantic_capture.cpp:255-258`). The detour
deliberately uses `+0x58` for *all* `BSShaderProperty` subclasses — Lighting,
Water, Effect — per the comment-cited F4SE `NiProperties.h` layout. An
earlier revision read `+0x48` (which is `BSFadeNode* pFadeNode`); see the
in-source comment.

### Light forms

Light extraction is currently disabled — the cell-walk-driven light
extractor was retired with the cell pipeline. `kFormType_LIGH = 34` is no
longer referenced from compiled sources.

### Player / DataHandler RelocPtrs

```
g_player      RelocPtr at RVA 0x032D2260   // bs_extraction.cpp:55
g_dataHandler RelocPtr at RVA 0x030DC000   // bs_extraction.cpp:56
g_tes         RelocPtr at RVA 0x032D2048   // bs_extraction.cpp:78
DataHandler+0xF58: NiTArray<TESObjectCELL*> cellList   // bs_extraction.cpp:1300-1304
```

## Extraction pipeline

A frame's path from "engine called us" to "Remix DrawInstance issued":

1. **Engine fires `BSLightingShaderProperty::GetRenderPasses` (or the water
   variant) on a leaf BSGeometry.** The detour
   (`DetourGetRenderPassesShared`, `semantic_capture.cpp:244`) computes
   `PassKey = FNV1a(geo, property, material)` and inserts/updates a
   `DrawableState` in `g_drawableMap` under `g_drawableMutex`. On first-seen
   it captures `state.geometry` / `property` / `material` plus
   `parent1`/`parent2` (two levels of `NiAVObject::m_parent` at +0x28). On
   every fire it refreshes `lastSeenFrame`, `lastFlags` (NiAVObject flags at
   +0x108), `fireCount`, and `liveWorldTransform` (Beth -> Remix swap via
   `BuildRemixTransform`). The detour tail-calls the original.

2. **`hkPresent` runs on the game thread.** It increments the Diagnostics
   frame counter, publishes the latest `CameraState`, calls
   `WeatherBridge::PushOncePerFrame`, hooks `OMSetRenderTargets` /
   `ClearRenderTargetView` on the immediate context if not yet hooked,
   captures the UI RT (when both clear + sole-bound flags fired this frame),
   and finally calls `SemanticCapture::Tick(device)`.

3. **`SemanticCapture::Tick`** (`semantic_capture.cpp:578`) does:
   - Load-screen gate: skip the resolve loop between PreLoadGame and
     PostLoadGame (60s failsafe) so the destination cell is never parsed
     against the loader thread's half-built world.
   - VRAM gate: skip the resolve loop if `totalAllocatedBytes > 90% *
     driverBudgetBytes`.
   - Resolve loop: iterate `g_drawableMap`. For each entry where
     `submittedToRemix == false`, `lastSeenFrame` is within
     `[SemanticCapture] ResolveRetryWindowFrames` (default 600) of current,
     and the entry's retry-backoff `nextRetryFrame` is due, call
     `Resolvers::Lighting::TryResolveStatic` or
     `Resolvers::Water::TryResolve`, both wrapped in an SEH handler.
     Failed resolves schedule exponential-backoff retries (4 fast attempts
     for async readback polling, then doubling to a 512-frame cap);
     SEH-caught crashes back off a flat 120 frames rather than being
     skipped permanently (the engine reuses pointer identities when it
     rebuilds a world, so a permanent skip blanked the drawable for the
     whole session).
   - Sweep cadence: every 60 frames, evict entries whose age exceeds
     `kTTLFrames = 18000` (5 minutes at 60 fps), calling
     `RemixRenderer::ReleaseDrawable` for each submitted entry under SEH.

4. **`Resolvers::Lighting::TryResolveStatic`** (`resolvers/lighting_static.cpp:67`):
   - Cast `state.geometry` to `BSTriShape` via `GetAsBSTriShape`. Skip skinned.
   - `BsExtraction::ParseShapeGeometry` parses vertex / index buffers from
     `BSTriShape::pRendererData->pVB / pIB`, branching on
     `BSGeometry::kFlag_FullPrecision` for half-float positions and on
     `kFlag_Skinned` / dynamic-vertex offsets for BSDynamicTriShape morphed
     positions (read at `shape + 0x180`).
   - Reject extreme extents (>1e6) and any NaN/Inf positions.
   - `BuildRemixTransform` converts `tri->m_worldTransform` (NiTransform at
     +0x70) into row-major 3x4. `ExtractAlphaState` populates alpha-test /
     alpha-blend fields from `geo->effectState` (NiAlphaProperty at +0x130).
   - Detect worldspace LOD chunks via parent NiNode names (see
     [Known limitations](#known-limitations)).
   - `BsExtraction::GetLightingMaterial` returns the
     `BSLightingShaderMaterialBase`. Skip if the material type is `kType_Landscape`.
   - For each texture slot — `spDiffuseTexture`, `spNormalTexture`
     (Octahedral post-process), and (via `ExtractEmissiveData`) the glow
     map from `BSLightingShaderMaterialGlowmap::spGlowMapTexture` — call
     `BsExtraction::ExtractMaterialTexture`. (The
     `spSmoothnessSpecMaskTexture` roughness slot was removed 2026-07-02;
     see Known limitations.) That function:
     1. Computes a stable hash from the texture *name* (FNV1a) plus the
        post-process mode.
     2. Hits the per-plugin `g_textureCache` to skip already-extracted
        textures. On a hit it checks `RemixRenderer::HasTextureHandle`:
        if the Remix-side handle was destroyed (PreLoadGame release wave,
        orphan LRU sweep) it re-supplies the cached pixels so
        `SubmitDrawable` recreates the handle — without this, a hash-only
        hit made the drawable fail its diffuse-loaded gate silently and
        permanently (the 2026-07-02 empty-world-after-save-load bug).
     3. Reads every mip via `ReadbackAllMips` (creates a per-mip staging
        texture, `CopySubresourceRegion`, `Map`, copies pixels honouring
        block-compressed row pitch). BC textures truncate the chain at the
        4x4 boundary so D3D11 doesn't reject sub-block standalone resources.
     4. Software-decompresses BC1/BC2/BC3/BC5 to RGBA8 only when the
        post-process pipeline needs an uncompressed input
        (`SmoothnessToRoughness` / `ConvertNormalToOctahedral`).
        Pure-diffuse textures stay in their source BC format.
     5. Concatenates the per-mip buffers into one tightly-packed mip chain
        suitable for `remixapi_TextureInfo`.
   - Pull emissive color/scale from `BSLightingShaderProperty::pEmissiveColor`
     and `fEmitColorScale` when the `kShaderFlags_EmitColor` bit is set.
   - Bail with `false` if the diffuse hash is zero (retry next frame).
   - Call `RemixRenderer::SubmitDrawable(hash, mesh, newTextures)`. On
     success mark `state.submittedToRemix = true` and store `meshHash = hash`.

5. **`Resolvers::Water::TryResolve`** (`resolvers/water.cpp:26`) follows the
   same shape but:
   - Uses a synthetic 1x1 RGBA8 blue diffuse with sentinel hash
     `0xFA11FA11FA11FA11` — only present so `SubmitDrawable`'s diffuse-loaded
     gate passes.
   - Pulls `BSWaterShaderMaterial::spNormalMap01` as the normal slot
     (Octahedral post-process).
   - Copies `BSWaterShaderMaterial::kDeepColor` into
     `mesh.waterTransmittance{R,G,B}` and sets `mesh.isWater = true`.

6. **`RemixRenderer::SubmitDrawable`** (`remix_renderer.cpp:543`) holds
   `g_renderStateMutex` for the entire call and is fully wrapped in a
   `try { ... } catch (...)` C++ exception fence with first-N-per-callsite
   logging:
   - For each new texture: refcount-bump-on-hit, `api->CreateTexture` on
     miss with a deterministic hash-derived path
     (`HashToPath(hash)` formats `L"0x%llX"`). After the upload loop, every
     texture hash the material will reference (diffuse/normal/roughness/
     emissive) that arrived via a cache hit with a live handle is ALSO
     refcount-bumped and added to the drawable's texture set (2026-07-02) —
     previously only the first submitter of a shared texture held a
     reference/stamped `lastDrawnFrame`, so a zeroed lone refcount let the
     sweep or ReleaseDrawable destroy textures still bound to visible
     materials (objects turned black ~10s after load).
   - Validates that all referenced texture hashes loaded; if diffuse
     missing, decrements bumps and returns `kFailed`.
   - Builds a material cache key by hashing
     `(diffuse, normal, roughness, emissive, emissiveColor*, intensity,
     useDrawCallAlphaState, isWater, waterTransmittance*)`. On miss, builds
     a `MaterialInfoOpaqueEXT` (default) or `MaterialInfoTranslucentEXT`
     (`isWater = true`) extension struct, calls `api->CreateMaterial`.
   - Builds a mesh cache keyed on `(contentHash, materialHash)` where
     `contentHash` = FNV1a over vertex+index bytes when GPU instancing is
     enabled (the default), or the per-drawable PassKey when disabled.
     `api->CreateMesh` on miss.
   - Stores the resulting `DrawableInstance` in `g_drawables` with the
     world transform, LOD chunk metadata, and water tag.

7. **Remix thread, `RemixRenderer::OnFrame`** (`remix_renderer.cpp:989`):
   acquires `g_remixApiMutex` first, calls `SnapshotActiveDrawables`
   (`g_drawableMutex`) to gather hashes and live poses, then takes
   `g_renderStateMutex` and walks `g_drawables`. Drawables not in the
   active set, or whose chunk-coverage box contains the player, are
   skipped. Surviving drawables are bucketed by `meshHandle`:
   - Bucket size 1 -> simple `DrawInstance` with the member's
     `worldTransform` baked into `instance.transform`.
   - Bucket size > 1 -> identity base transform plus a
     `remixapi_InstanceInfoGpuInstancingEXT` chained on `pNext`,
     carrying every member's per-instance transform.

   Buckets where any member has `isWater = true` OR `categoryFlags` with
   `REMIXAPI_INSTANCE_CATEGORY_BIT_ANIMATED_WATER`. Each `DrawInstance`
   call is wrapped in both an SEH `__try` and a C++ `try/catch` (mixed-EH
   isolated via the two-helper `CallDrawInstanceCxxGuarded` /
   `CallDrawInstanceGuarded` pattern); on either kind of catch the
   bucket's member meshHandles are nulled out so they're skipped on
   subsequent frames.

   After the draw loop: optional `DrawScreenOverlay` for the captured UI,
   the LRU sweeps (`SweepStaleMaterials` first — the lever — then
   `SweepStaleTextures` as backstop) on the
   `cullingTextureLRUSweepPeriod` cadence, and finally `api->Present`.

## Remix integration

`RemixAPI::Initialize` (`remix_api.cpp:40`):

1. `remixapi_lib_loadRemixDllAndInitialize(L"d3d9.dll", &g_remixInterface,
   &g_remixDll)` loads the runtime fork from the game-folder `d3d9.dll`.
2. Creates a dedicated visible top-level window (`L"FO4RemixWindow"`) sized
   to the game backbuffer.
3. Preferred: `g_remixInterface.dxvk_CreateD3D9(FALSE, &g_d3d9)` -> manually
   creates a `D3DDEVTYPE_HAL` D3D9Ex device on that window ->
   `g_remixInterface.dxvk_RegisterD3D9Device(g_d3d9Device)`. This bypasses
   `Startup()`'s default-init flow that spawns the dev-menu overlay window
   (whose `RegisterRawInputDevices(RIDEV_NOLEGACY)` would steal raw input
   from the game).
4. Fallback: if any of the dxvk extension steps fail or are missing, call
   `g_remixInterface.Startup(&startupInfo)` with `hwnd = g_remixWindow` and
   `editorModeEnabled = 0`. After Startup, the input-hijack-mitigation hooks
   (`RestoreLegacyKeyboardInput`, `RebindRawInputToGameWindow`) run from
   `hkPresent` on a delayed cadence to reclaim keyboard/mouse for the game
   window.

Object creation is wrapped by `RemixRenderer::SubmitDrawable`:

- `api->CreateTexture(remixapi_TextureInfo*)` — `sType = TEXTURE_INFO`,
  `data` points at the packed mip-chain buffer, `dataSize` is the buffer
  size in bytes, `format` is mapped from DXGI via `DxgiToRemixFormat`
  (BC1/BC3/BC5/BC7/RGBA8/BGRA8 in both UNORM and SRGB variants — anything
  else maps to 0 and the texture is skipped).
- `api->CreateMaterial(remixapi_MaterialInfo*)` — opaque branch sets
  `albedoConstant = (1,1,1)`, `opacityConstant = 1`, `roughnessConstant =
  mesh.roughnessConstantOverride` when >= 0 (metal conversion) else `0.5`
  if a roughness texture is present else `0.8`, `metallicConstant =
  mesh.metallicConstant` (0 for non-metals), and copies alpha-test fields
  directly. `useDrawCallAlphaState = 0` so the
  material-level alpha-test fields win. Translucent branch sets
  `refractiveIndex = 1.33`, `transmittanceMeasurementDistance = 500.0` cm,
  `useDiffuseLayer = 0`, and forwards `transmittanceColor` from the water
  material's `kDeepColor`.
- `api->CreateMesh(remixapi_MeshInfo*)` — single
  `remixapi_MeshInfoSurfaceTriangles` with `vertices_values =
  remixapi_HardcodedVertex*` and `indices_values = uint32_t*`. Skinning
  hasvalue is always 0 (skinning regression accepted in current scope).
- `api->DrawInstance(remixapi_InstanceInfo*)` — `doubleSided = 1`,
  `categoryFlags = 0` or `REMIXAPI_INSTANCE_CATEGORY_BIT_ANIMATED_WATER` for
  water buckets. `pNext` is either null or
  `remixapi_InstanceInfoGpuInstancingEXT` for batched buckets.

Filtering / wrapping defaults set on every material (`remix_renderer.cpp:792-794`):
`filterMode = 1`, `wrapModeU = 1` (Repeat), `wrapModeV = 1` (Repeat).

Camera setup uses `remixapi_CameraInfoParameterizedEXT` with
`fovYInDegrees`, `aspect`, `nearPlane`, `farPlane`, plus the four basis
vectors (`position`, `forward`, `up`, `right`). Default fallback when the
camera is unavailable is FOV 75, aspect 1280/720, near 0.1, far 1000.

Radiance values flow through unchanged (HDR; not 0-1 normalised). Emissive
intensity is `mesh.emissiveIntensity * g_config.emissiveIntensity`.

VRAM telemetry: `RemixRenderer::GetVramStats` calls
`remixapi_Interface::GetVramStats` if present and copies the result into
`VramStats`. The Tick VRAM gate uses `totalAllocatedBytes /
driverBudgetBytes` at 90 %; the texture LRU's budget pass uses
`usedMaterialTextureBytes` against `cullingTextureBudgetMiB << 20`.

## Coordinate conventions

Bethesda uses a right-handed system with X-forward / Y-right. Remix expects
X-right / Y-forward. The plugin swaps X/Y on positions and on the basis
vectors of any rotation matrix; Z is unchanged.

Rotation columns 0 and 1 are swapped at write time
(`semantic_capture.cpp:67-77`):

```cpp
void SemanticCapture::BuildRemixTransform(const NiTransform& xf, float out[3][4]) {
    const float scale = xf.scale;
    for (int r = 0; r < 3; ++r) {
        out[r][0] = xf.rot.data[r][1] * scale;
        out[r][1] = xf.rot.data[r][0] * scale;
        out[r][2] = xf.rot.data[r][2] * scale;
    }
    out[0][3] = xf.pos.y;
    out[1][3] = xf.pos.x;
    out[2][3] = xf.pos.z;
}
```

The same rule applies in `Camera::Get` (`camera.cpp:51-65`) for the camera
basis vectors, and to player position when populating
`CameraState::position`. `CameraState::playerWorldPos` is intentionally
*not* swapped (it stays in raw Bethesda coords) so the OnFrame LOD-chunk
spatial filter can compare directly against `chunkOriginX/Y` which were
captured pre-swap from `tri->m_worldTransform.pos.{x,y}`.

## Weather / time-of-day bridge

`weather_bridge.cpp:41-64`. Called once per frame from `hkPresent` after
the Remix thread is ready and `g_gameDataReady` is set:

- Looks up `LookupFormByID(0x00000038)` (the GameHour TESGlobal) and caches
  the pointer for the plugin's lifetime.
- Reads `cachedGameHour->value` (range 0..24).
- Computes:
  - `sunElevation = sin((hour - 6.0) / 12.0 * pi) * 90.0` -> peaks ~+90 at
    noon, sits at the horizon at hours 6 and 18, dips negative at night.
  - `sunRotation = (hour / 24.0) * 360.0` -> linear sweep through the day.
- Calls `RemixRenderer::SetConfigVariable("rtx.atmosphere.sunElevation", ...)`
  and `("rtx.atmosphere.sunRotation", ...)`, formatted as `%.4f`.
- Per-key failure dedup: first failure logs once at warn level; subsequent
  failures for the same key are silent so a runtime-fork without those slots
  doesn't spam 60 lines/sec.

Time-of-day only. Weather signals (rain/snow/fog/volumetric fog/isInterior)
are NOT pushed; the Sky struct reverse engineering required to read those
from FO4 is outstanding.

## Configuration

`FO4RemixPlugin.ini` lives next to the DLL (resolved by
`GetModuleHandleEx(&LoadConfig)` -> swap `.dll` for `.ini`). All
`g_config` consumers read the parsed struct, never the INI directly.

| Section | Key | Type | Default | Effect | Read by |
|---------|-----|------|---------|--------|---------|
| Logging | `LogShapeInfo` | bool | 0 | log shape name + vertex format + flags per extracted shape | (plumbed; current pipeline does not log per-shape) |
| Logging | `LogLargeShapes` | bool | 1 | log shapes with extent > 500 | (plumbed) |
| Logging | `LogRejections` | bool | 1 | log mesh rejections (NaN, bad indices, extent) | `bs_extraction.cpp` (`ParseShapeGeometry`), resolvers |
| Logging | `LogTextures` | bool | 0 | log every extracted texture | `bs_extraction.cpp:858` (`ExtractMaterialTexture`) |
| Logging | `LogLights` | bool | 0 | log extracted light info | (light extraction currently retired) |
| Logging | `LogBoneDiag` | bool | 0 (INI ships 1) | one-shot bone-matrix dump | (plumbed; skinning not in current scope) |
| Lights | `Enabled` | bool | 1 | master toggle for extracted lights | (plumbed) |
| Lights | `Intensity` | float | 1.0 | radiance multiplier | (plumbed) |
| Lights | `RadiusMultiplier` | float | 1.0 | sphere-light radius multiplier | (plumbed) |
| Lights | `ColorStrength` | float | 1.0 | 0 = white, 1 = full game color | (plumbed) |
| Skinning | `Enabled` | bool | 1 | extract animated skinned meshes | (plumbed; resolvers currently skip skinned) |
| Emissive | `GlowMapsEnabled` | bool | 1 | extract `BSLightingShaderMaterialGlowmap::spGlowMapTexture` | `bs_extraction.cpp` (`ExtractEmissiveData`) |
| Emissive | `EmissiveColorEnabled` | bool | 1 | use `pEmissiveColor` + `fEmitColorScale` | `bs_extraction.cpp` (`ExtractEmissiveData`) |
| Emissive | `Intensity` | float | 1.0 | global multiplier on `fEmitColorScale` | `remix_renderer.cpp:787` |
| Emissive | `LogEmissive` | bool | 0 | log emissive extraction details | `bs_extraction.cpp:926` |
| Diagnostics | `Enabled` | bool | 1 | master toggle for periodic `[GameState]` / `[Plugin]` log lines | `fo4_diagnostics.cpp:78` |
| SemanticCapture | `Enabled` | bool | 0 | install the BSLightingShaderProperty + BSWaterShaderProperty `GetRenderPasses` hooks (the entire event-driven extraction path) | `semantic_capture.cpp:460` |
| Culling | `TextureLRUGraceFrames` | uint32 | 600 | TTL for un-drawn textures before the LRU sweep evicts them | `remix_renderer.cpp:1331` |
| Culling | `TextureLRUSweepPeriod` | uint32 | 60 | frames between LRU sweeps in `OnFrame` | `remix_renderer.cpp:1294` |
| Culling | `TextureBudgetMiB` | uint32 | 0 (TTL only) | soft cap on `usedMaterialTextureBytes`; non-zero enables the budget pass | `remix_renderer.cpp:1305` |
| Culling | `MaterialLRUGraceFrames` | uint32 | 600 | TTL for un-drawn materials before refcount-zero entries are destroyed | `remix_renderer.cpp:1310` |
| Culling | `LodChunkStaleFrames` | uint32 | 30 | frames a worldspace LOD chunk can go un-fired before OnFrame stops drawing it (0 = disabled); engine hid the chunk when its cells attached | `remix_renderer.cpp` (OnFrame stale-chunk filter) |
| Overlay | `HudOverlayEnabled` | bool | 0 (code) / 1 (shipped ini) | submit the captured DX11 UI render target via `api->DrawScreenOverlay`. Requires a runtime with the rtx_fork_overlay.cpp layout fix (dxvk-remix 8990aed); the shipped ini enables it as of 2026-07-03 | `remix_renderer.cpp:1273` |
| Overlay | `RestoreLegacyInput` | bool | 1 | issue `RIDEV_REMOVE` for keyboard so the game still receives `WM_KEYDOWN` after Remix's overlay-thread `RIDEV_NOLEGACY` registration | `remix_api.cpp:162` |
| Performance | `GpuInstancing` | bool | 1 | share Remix mesh handles across drawables with byte-identical geometry+material and batch via `InstanceInfoGpuInstancingEXT` | `remix_renderer.cpp:820` |
| Performance | `CpuTextureCacheMiB` | uint32 | 1024 | byte budget for the CPU-side decoded-texture cache in bs_extraction (LRU eviction past it; 0 = unbounded legacy) | `bs_extraction.cpp` (`TextureCacheEnforceBudget`) |
| Precombines | `MergeTwoSided` | bool | 1 | render merge-expanded precombines double-sided (vanilla-faithful). 0 = single-sided experiment: re-enables the per-instance mirrored-record winding flip; potential path-tracing perf win if content winding holds up post-b112e08 | `lighting_static.cpp` (merge submit) |

`[Limits] MaxExtent` was retired 2026-07-10: documented but never consumed
(resolvers use a hard 1e6 NaN backstop), and wiring the shipped 10000 in
would have started rejecting huge-local-extent LOD chunks.

## Known limitations

- **`cell_pipeline.{cpp,h}` is paused.** The cell-granular state machine for
  per-cell extraction and Remix loading was retired with Phase 1B in favour
  of the event-driven `semantic_capture` path. The files remain in `src/`
  for reference but are NOT in the CMake build. Do not consult their API as
  if it were live.
- **Light extraction is retired.** With the cell pipeline gone there is no
  per-cell `ExtractCellLights` walk. Sphere/spot lights from `LIGH` refs
  are currently absent. `[Lights]` config keys exist but have no consumer.
- **Terrain (TESObjectLAND quadrants) is not submitted.** The land-walk
  was tied to the cell pipeline. The lighting resolver explicitly skips
  `BSLightingShaderMaterialBase::kType_Landscape` (`lighting_static.cpp:197-200`).
  The path tracer renders distance via worldspace LOD chunks and falls back
  to the atmospheric model elsewhere.
- **Skinned meshes are skipped.** Both resolvers reject
  `tri->vertexDesc & BSGeometry::kFlag_Skinned`. Characters and creatures
  do not appear in the path-traced view yet.
- **Precombined / merge-instanced transforms are wrong (open, 2026-07-03).**
  The resolver's model is "local-space vertices x leaf `m_worldTransform`",
  which holds for plain refs but not for precombined geometry
  (`BSMergeInstancedTriShape` / `BSMultiStreamInstanceTriShape`): those carry
  per-instance placement in structures F4SE does not declare
  (`BSPackedCombinedGeomDataExtra` is RTTI-only, `GameRTTI.h:883`), their
  leaf transform arrives identity-rotation, and the plugin renders one copy
  at the leaf transform where the engine draws N placed instances -- the
  "roads in Sanctuary / light poles / hedges misplaced, worse with
  precombines" report. The dirty-pose path is NOT the cause (it memcmps the
  full 3x4). The capped `[InstDiag]` diagnostic in `lighting_static.cpp`
  logs, per merge-instanced shape: parsed vertex bbox (local vs
  combined-space discrimination), leaf local+world transforms, two parents,
  and the shape's `NiExtraData` entries (class/name/leading qwords) to
  anchor the runtime layout of the instance-transform array for the real
  fix. Next session: read a log with precombines enabled, identify the
  instance data, then either expand instances plugin-side (one DrawInstance
  per transform, mesh shared) or skip merged shapes when the originals also
  render.
- **Weather is time-of-day only.** Storms, fog, volumetric fog, and the
  interior/exterior signal are not pushed to Remix; the Sky-struct RE
  required for FO4 is outstanding (see `weather_bridge.h:9-13`).
- **HUD/Scaleform overlay (UI passthrough) enabled as of 2026-07-03.** The
  historical blocker -- `dispatchScreenOverlay` asserting in
  `dxvk_barrier.cpp` on a `dstLayout == VK_IMAGE_LAYOUT_UNDEFINED`
  transition -- was fixed runtime-side (dxvk-remix `8990aed`, image created
  in `SHADER_READ_ONLY_OPTIMAL`), and the shipped ini now sets
  `[Overlay] HudOverlayEnabled=1` (code default stays 0 so a missing key on
  an un-fixed runtime cannot crash). Remaining known gaps: the capture is a
  `CopyResource` + blocking `Map(READ)` on the game's render thread each
  frame the UI draws (measurable cost); overlay opacity is hardcoded 1.0;
  input for interactive menus is not routed (pixels only -- fine for the
  passive HUD, but Pip-Boy/inventory focus stays with whichever window has
  it).
- **Worldspace LOD chunk hiding follows engine fire cadence (2026-07-02).**
  Chunks are identified by parent NiNode names (`"chunk"` +
  `"4"|"8"|"16"|"32"`, or `"obj"`). The primary filter is fire-age based:
  the engine calls `GetRenderPasses` every frame for geometry that survives
  its culling and hides LOD chunks when their cells attach at full detail,
  so OnFrame skips any chunk whose fire age exceeds `[Culling]
  LodChunkStaleFrames` (default 30) while the scene is actively firing
  (pause/main menus freeze the filter). The older "skip if player inside
  chunk coverage box" spatial test remains as a backstop. Two earlier
  filters (`kFlagTopFadeNode`, `kFlagIsMeshLOD` blanket-reject) were tried
  and reverted because they over-rejected HQ structural meshes
  (`lighting_static.cpp:101-124`).
- **Smoothness/spec-mask (`_s.dds`) extraction removed (2026-07-02).** FO4's
  packed spec maps translated too inconsistently to naive roughness
  (mirror decals, black-void metal fences/racks). Non-metal opaque materials
  use `roughnessConstant = 0.8`; the `InvertRGB` post-process machinery
  remains in `bs_extraction.cpp` but has no caller. The real spec-gloss ->
  metal-rough conversion now exists for the envmap class (next entry).
- **Metal conversion take 2: kType_Envmap -> metal-rough (2026-07-02).**
  The pure-black objects (power-armor stands, picket-fence LODs, street
  lamps, workstations) were all `GetType() == kType_Envmap` with the
  `SLSF1_Environment_Mapping` propFlags bit (log-verified: every reported
  class matType=1/bit7=1, every fine-rendering control 0/0). FO4 authors
  their diffuse near-black and builds the look from `kSpecularColor *
  fSpecularColorScale` + cubemap; untreated they path-trace as black
  dielectric voids. NOT caused by async readback (content verified via BC3
  alpha stats), texture lifecycle (0 orphans/evictions logged), or vertex
  colors (gate engaged, still black). The resolver
  (`lighting_static.cpp`) classifies on `GetType()` ONLY — take 1 (506e5e7,
  reverted bb2a02f) also gated on `fEnvmapScale > 0.01`, which skipped the
  PA stand (reads ~0); bb2a02f's claim it "was not kType_Envmap" is wrong.
  Derivations: `metallic = MetalMetallic * (0.2 + 0.8*fSmoothness)`
  (smoothness modulation keeps bark/wood wetness-envmaps mostly dielectric;
  take 1's `* fEnvmapScale` under-metallized authored-low scales),
  `roughness = clamp(1-fSmoothness, MetalMinRoughness, 0.95)`, and a
  hue-preserving luminance floor on the diffuse (`AlbedoLumFloor_Apply`,
  multiplicative to 6x + neutral fill, alpha untouched) instead of take 1's
  blend-toward-spec-tint that white-washed weapons. Floor folds into the
  texture cache hash (discriminant 7), metallic/roughness fold into the
  material cache key. `[Materials]` ini section; first 40 classifications
  log as `[Metal]` lines. In-game verification: the luminance floor is the
  piece that recovers the black objects; the metallic/roughness constants
  did NOT help (metal F0 comes from albedo, so lifted-albedo x high
  metallic still reads near-black -- the metallic constant fights the
  floor). Both derivations are opt-in via `MetalMetallicEnabled` /
  `MetalRoughnessEnabled`, default OFF (legacy metallic 0 / rough 0.8).
- **`maxExtent` config is plumbed but unused.** `lighting_static.cpp` and
  `water.cpp` use a hard-coded `1.0e6f` extent guard instead of
  `g_config.maxExtent`.
- **Persistent texture refcount leak under failure paths is accepted.**
  `DecrementTextureRefs` and `DecrementMaterialRef` only decrement counts
  on backout from a failed `SubmitDrawable`; they do *not* call
  `DestroyTexture` / `DestroyMaterial`. Orphans wait for the next LRU
  sweep. This is deliberate to avoid the create-destroy churn cycle that
  was observed corrupting dxvk-remix internal state.
- **Per-Tick submission budget removed.** `semantic_capture.cpp:614-619`
  notes the previous budget cap (4 submissions/frame) starved streaming.
  Protection now lives in the VRAM gate (90 % of `driverBudgetBytes`)
  plus per-call SEH/C++ exception fences inside `SubmitDrawable`.

