# FO4 RTX v0.0.4

Path-traced Fallout 4 via RTX Remix, driven by a custom F4SE capture plugin.

## Notes
- This build targets Fallout 4 **1.11.191**. Bethesda's latest update changed the game executable and breaks the mod's hooks — if your install is on the newest patch, follow the **Downgrading** section below first. A build for the new patch will come once the hook addresses are updated.
- Steam copy only! Don't expect it to work on GOG/Epic/gamepass versions.
- This is still an early build. It's playable, but expect visual bugs — see Known Issues at the bottom.

## Requirements
- Fallout 4 GOTY on Steam, version **1.11.191** (see below)
- F4SE v0.7.7+ (<https://www.nexusmods.com/fallout4/mods/42147?tab=files>)
- A GPU that supports Remix:
  - RTX 2000 series GPUs and newer
  - AMD RX 6000 series GPUs and newer
  - Intel Arc A series GPUs

## Downgrading Fallout 4 to 1.11.191
Skip this if your game is already on 1.11.191. Requires owning **FO4 GOTY on Steam** (the depot list includes all DLC). This downloads the full game again, so expect a large download and make sure you have the disk space.

1. Make sure Steam is running, press Win+R and run `steam://open/console`
2. Steam opens with the Console tab selected. Paste these in one by one (the console prints a "Depot download complete" line as each one finishes):
```
download_depot 377160 377161 5983086794954940044
download_depot 377160 377162 5433405173062582852
download_depot 377160 377163 8360827888850301367
download_depot 377160 377164 8492427313392140315
download_depot 377160 435870 1213339795579796878
download_depot 377160 435880 7708996200055144433
download_depot 377160 435881 1207717296920736193
download_depot 377160 480631 6588493486198824788
download_depot 377160 480630 5527412439359349504
download_depot 377160 490650 4873048792354485093
download_depot 377160 435871 471362073238143096
download_depot 377160 435882 8482181819175811242
download_depot 377160 393885 5000262035721758737
download_depot 377160 393895 7677765994120765493
```
3. When all downloads are complete, go to `C:\Program Files (x86)\Steam\steamapps\content\app_377160`. You'll find one `depot_XXXXXX` folder per command.
4. Merge the **contents** of all the depot folders together into one new folder (e.g. `Fallout 4 1.11.191`) — copy each depot folder's contents in and let the subfolders merge. You can do it manually, or use the PowerShell script here: [LINK]
5. That merged folder is now your 1.11.191 game folder. Install F4SE and the mod into **it** (steps below), and launch from there. Steam still needs to be running when you play. You can move the folder anywhere you like.

## Installation
1. Install F4SE v0.7.7+ into the game folder if you haven't already.
2. Extract the contents of the 7z file into your Fallout 4 game directory (the folder containing `Fallout4.exe`).
3. Launch the game through `f4se_loader.exe`!

The game loads up, then a second window spawns — that's the **Remix window**, the actual path-traced output. Keep the original game window in focus; it controls the Remix window.

Optional: in `Documents\My Games\Fallout4\Fallout4.ini`, you can set `bUseCombinedObjects=0` in the `[General]` section. v0.0.4 renders precombined geometry natively, so try the default first — only flip this if you see broken or misplaced static objects.

## First launch tips
- Your **first visit** to an area converts textures in the background — surfaces start soft and sharpen over a few seconds. Converted textures are cached at `%LOCALAPPDATA%\FO4Remix\texcache` (8 GB cap), so later sessions in the same areas load dramatically faster.
- If you add or remove **texture mods**, delete that texcache folder once so stale textures don't get served.
- Settings live in `Data\F4SE\Plugins\FO4RemixPlugin.ini`, next to the plugin. Pop-in speed vs. frame-time knobs are under `[Performance]`, material appearance under `[Materials]`. The shipped values are the tested configuration.

## Reporting bugs
Please include these two files with any report:
- `Documents\My Games\Fallout4\F4SE\FO4RemixPlugin.log`
- `%LOCALAPPDATA%\CrashDumps\FO4Remix_exitcodes.log` (crash/exit history, written by the bundled watchdog — the plugin works fine without it, but reports are much more useful with it)

## Changelog
- Updated for Fallout 4 **1.11.191**
- **Skinned meshes work**: characters and creatures render with full GPU skinning and per-frame bone animation
- **Lights are fixed** (they were broken in the previous build)
- **Water**: real path-traced translucent water with ripple normal maps
- **Precombined world geometry** (roads, building kits, hedges) now renders correctly: expanded per hardware instance and upgraded to engine-exact geometry as you look at it
- Fixed repeated static objects rendering inside-out/mirrored (street lamps, power armor stands, etc.)
- **Texture quality overhaul**: textures re-capture at full resolution as you approach (no more permanently blurry surfaces), roughness maps, spec-gloss → metal conversion, glow maps/emissives, and grayscale palette tinting (hair colors, robot paint schemes)
- **Streaming overhaul**: nearest-in-view geometry loads first, GPU readbacks are fully async, and a persistent on-disk texture cache makes repeat sessions far faster — pop-in speed and streaming hitches are massively improved
- **Stability**: multiple deadlock and crash classes root-caused and fixed (threading/lock-order issues between the game, plugin, and Remix runtime); crash-diagnostic watchdog now bundled

## Known issues
- NPC faces (FaceGen heads) can be missing — bodies and hair render fine
- Power armor helmets can render magenta
- Hair can show through hats
- Some foliage renders with wrong/default colors, or goes missing
- Some metal surfaces (notably buildings) can look purple/over-tinted — the spec-gloss conversion heuristic needs tuning
- Quitting to desktop may record a crash in the exit log after the game closes — harmless teardown noise, your saves are fine
