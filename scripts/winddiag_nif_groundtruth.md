# NIF Ground-Truth Geometry Dump — Inside-Out Culling Investigation

Extracted 2026-07-08 by the static-analyzer agent for the FO4-Remix inside-out-rendering
bug (PArig power-armor stands and street-lamp posts render backface-culled from the front).

Source archive: `C:\Users\sparkles\Projects\Games\Fallout 4\Data\Fallout4 - Meshes.ba2`
(BA2 GNRL v8, 42426 entries). Parsers: scratchpad `ba2.py` + `nif.py` (offline, no game).

Decode semantics match the plugin exactly (`src/bs_extraction.cpp` ParseGeometry +
`f4se/BSGeometry.h` VertexDesc union / kFlag_* bits):
- `stride = (desc & 0xF) * 4`  (nibble0 = vertexDataSize/4; GetVertexSize)
- `szVertex = (desc>>4)&0xF`  (nibble1, 0 for non-dynamic)
- `oUV = (szVertex + ((desc>>8)&0xF))*4`, `oNormal = (szVertex + ((desc>>16)&0xF))*4`,
  `oColor = (szVertex + ((desc>>24)&0xF))*4`
- flags in vertexDesc bits: Vertex=1<<44, UVs=1<<45, Normals=1<<47, Tangents=1<<48,
  VertexColors=1<<49, Skinned=1<<50, **FullPrecision=1<<54**
- position: half3 (fullPrec off) at offset 0, else float3; normal: 3×u8 → b/255*2−1 at oNormal
- File BSTriShape layout (validated by `dataSize == numVerts*stride + numTri*6`):
  boundingSphere(4f) skin(i32) shader(i32) alpha(i32) **vertexDesc(u64) numTriangles(u32)
  numVertices(u16) dataSize(u32)** vertexData[nv*stride] triangles[nt*3 u16].
  BSMeshLODTriShape appends **3×u32 LOD triangle counts** at the very end (verified: the
  trailing 12 bytes sum to numTriangles).

Every NIF header footer-validated (`header_end + Σblock_sizes + footer == file_size`), so the
block-type table, block names, and per-block byte ranges below are reliable.

---

## 1. Name-table matches (`parig` / `streetlamp`)

| idx | unpacked | path |
|-----|---------:|------|
| 1285 | 358 | Meshes\Armor\PowerArmor\ArmorPARightLeg.nif  *(not a rig — "PARight")* |
| 1287 | 358 | Meshes\Armor\PowerArmor\ArmorPARightArm.nif  *(not a rig)* |
| **3269** | **154612** | **Meshes\SetDressing\PARig\PArig02.nif  ← MAIN PArig** |
| 11184 | 151004 | Meshes\SetDressing\PARig\PARig01.nif |
| **1445** | **15980** | **Meshes\SetDressing\StreetLamps\StreetLamp01.nif  ← MAIN lamp** |
| 3001 | 22879 | Meshes\SetDressing\StreetLamps\StreetLampPost01.nif |
| 4091 | 34515 | Meshes\SetDressing\StreetLamps\StreetLamp02.nif |
| 9840 | 34499 | Meshes\SetDressing\StreetLamps\StreetLamp03.nif |
| 5265 | 9316 | Meshes\SetDressing\StreetLamps\StreetLamp01Base01.nif |
| 6961 | 15192 | Meshes\SetDressing\StreetLamps\StreetLamp01Post01.nif |
| 6539 | 20924 | Meshes\SetDressing\StreetLamps\ResidentialStreetLamp01.nif |
| 35439 | 19695 | Meshes\SetDressing\StreetLamps\Prewar_ResidentialStreetLamp01.nif |
| 7078 | 15086 | Meshes\SetDressing\StreetLamps\ColonialLamppost01.nif |
| 12002 | 18288 | Meshes\SetDressing\StreetLamps\ColonialLamppostTall01.nif |
| 35438 | 13720 | Meshes\SetDressing\StreetLamps\ColonialLampWall01.nif |
| 5240 | 70602 | ...\StreetLamps\StreetLampBanner02.nif  *(+Banner01/03/04/05/06, StreetLamp02Banner01/02)* |
| 9982 | 12059 | Meshes\SetDressing\StreetLamps\StreetLampBase01.nif |
| **31285** | 5975 | Meshes\LOD\SetDressing\StreetLamps\Prewar_ResidentialStreetLamp01_LOD.nif  *(LOD)* |
| **31286** | 5933 | Meshes\LOD\SetDressing\StreetLamps\ResidentialStreetLamp01_LOD.nif  *(LOD)* |

**LOD / damage variants of the two MAIN assets:** none.
`StreetLamp01.nif` and `PArig02.nif` have **no separate `_LOD.nif` and no `_d.nif`**. The only
`Meshes\LOD\...StreetLamps` files belong to the *different* Residential/Prewar-Residential lamp
assets. StreetLamp01's LOD is carried *internally* as BSMeshLODTriShape (see §3). PArig02 is a
plain single BSTriShape with no LOD at all (set-dressing rig, never distant-culled to LOD).

---

## 2. Extracted files

Written to scratchpad: `PArig02.nif`, `StreetLamp01.nif`, `VltWallFree01.nif` (reference).

---

## 3. Per-shape geometry dump

### 3a. PArig02.nif — MAIN PArig  (17 blocks, 1 geometry block)
Block types: NiNode, **BSTriShape**, BSLightingShaderProperty, BSShaderTextureSet, BSXFlags,
bhkNPCollisionObject, bhkPhysicsSystem. (No NiAlphaProperty → opaque.)

**BLOCK #6  BSTriShape  name=`PArig02:0`**
- vertexDesc = `0x0001B00000430205`  →  stride **20**, half-precision
- numVertices **5334**, numTriangles **6075**, dataSize 143130 (identity OK)
- flags: Vertex, UVs, Normals, Tangents  (no VertexColors, no Skinned)
- nibbles: n0=5 n1=0 n2=2(oUV=8) n4=3(oNormal=12) n6=0(oColor=0)
- vertex0 raw (20B): `e451a2d4da50c0b0953aaa2e9b10b6b3faa184f2`
- first 6 indices (file order): **18, 19, 16, 16, 17, 18**
- pos[0..2]: (47.125, −74.125, 38.8125), (45.3125, −74.125, 38.8125), (45.3125, −69.8125, 6.25)
- normal[0]: (0.2157, −0.8745, 0.4275) |n|=0.997
- **WINDING PARITY:  dot>0 = 6075,  dot<0 = 0,  ~0 = 0,  mean_dot = 0.9024  → CONSISTENT**

### 3b. StreetLamp01.nif — MAIN lamp  (12 blocks, 2 geometry blocks)
Block types: NiNode, **BSMeshLODTriShape** (×2), BSLightingShaderProperty, BSShaderTextureSet,
BSXFlags, **NiAlphaProperty**, bhkNPCollisionObject, bhkPhysicsSystem.

**BLOCK #4  BSMeshLODTriShape  name=`L2_deets:4`**
- vertexDesc = `0x0001B00000430205`  →  stride **20**, half-precision
- numVertices 56, numTriangles 48, dataSize 1408 (identity OK)
- flags: Vertex, UVs, Normals, Tangents
- **internal LOD triangle counts (LOD0,LOD1,LOD2) = (0, 0, 48)**  (all detail in LOD2)
- vertex0 raw (20B): `0cc870d9575fecae223b80353718676be8367ffc`
- first 6 indices: 0, 1, 2, 2, 3, 0
- pos[0]: (−8.09375, −174.0, 469.75);  normal[0]: (−0.5686, −0.8118, −0.1922)
- **WINDING PARITY:  dot>0 = 48,  dot<0 = 0,  mean_dot = 0.9960  → CONSISTENT**

**BLOCK #8  BSMeshLODTriShape  name=`StreetLamp01:0 - L2_StreetLamp01:0 - L2_moredeets:0`**
- vertexDesc = `0x0001B00000430205`  →  stride **20**, half-precision
- numVertices 400, numTriangles 412, dataSize 10472 (identity OK)
- flags: Vertex, UVs, Normals, Tangents
- **internal LOD triangle counts (LOD0,LOD1,LOD2) = (44, 0, 368)**  (sum 412 = numTriangles)
- vertex0 raw (20B): `18444b470153eeba4133fe3bbfed80bf7f80007f`
- first 6 indices: 0, 1, 2, 2, 3, 0
- pos[0]: (4.09375, 7.29297, 56.03125);  normal[0]: (0.4980, 0.8588, 0.0039)
- **WINDING PARITY:  dot>0 = 411,  dot<0 = 1,  mean_dot = 0.9415  → CONSISTENT**

### 3c. VltWallFree01.nif — REFERENCE known-good static  (7 blocks, 1 geometry block)
Block types: NiNode, **BSTriShape**, BSLightingShaderProperty, BSShaderTextureSet, BSXFlags,
bhkNPCollisionObject, bhkPhysicsSystem.

**BLOCK #4  BSTriShape  name=`VltWallFree01:21`**
- vertexDesc = `0x0001B00000430205`  →  stride **20**, half-precision
- numVertices 31, numTriangles 14, dataSize 704 (identity OK)
- flags: Vertex, UVs, Normals, Tangents
- vertex0 raw (20B): `00d80056005b003c748dee2b7f007f7f7f7f007f`
- first 6 indices (file order): **2, 3, 0, 0, 1, 2**
- pos[0..2]: (−128, 96, 224), (−128, 96, 192), (128, 96, 192)  (clean grid-aligned wall)
- normal[0]: (−0.0039, −1.0, −0.0039) |n|=1.000
- **WINDING PARITY:  dot>0 = 14,  dot<0 = 0,  mean_dot = 1.0000  → CONSISTENT**

---

## 4. Verdict / anomalies

- **Identical vertexDesc** `0x0001B00000430205` on every shape in all three files (and the
  reference): stride 20, half-precision position, Vertex|UVs|Normals|Tangents, no colors, no
  skinning. Nothing unusual in the PArig/lamp descriptors.
- **Winding parity is CONSISTENT (strongly positive) for every shape**, PArig/lamp and the
  known-good vault wall alike (mean_dot 0.90 / 0.996 / 0.94 / 1.00; zero triangles wound
  backwards except 1 near-degenerate face in the 412-tri lamp block). **The source NIF winding
  is NOT inverted for PArig or StreetLamp** — their authored triangle winding agrees with their
  authored vertex normals exactly as the reference does. There is **no source-geometry winding
  difference** that could explain these objects rendering inside-out.
- Therefore the inside-out culling is a **runtime issue, not a mesh-data issue**: it lives in
  the per-instance transform / culling / winding-order state the plugin+runtime apply to these
  specific objects, not in the BA2 geometry. This corroborates prior findings ("baked IB winding
  NOT reversed"; "PArig/lamps single-sided + consistent winding").
- **Block-type anomaly (informational):** StreetLamp01 uses **BSMeshLODTriShape** (self-contained
  3-level LOD: the triangle list is partitioned by the trailing LOD0/LOD1/LOD2 counts, LOD0 =
  highest detail first). PArig02 and the vault wall are plain **BSTriShape**. This is a legitimate
  layout difference but does not affect winding — the plugin's ParseGeometry treats all
  BSTriShape-family shapes identically (reads the full triangle list; LOD ranges are ignored).
- StreetLamp01 carries a **NiAlphaProperty** (the two BSTriShape statics do not) — worth noting if
  alpha-blend state feeds any cull/two-sided decision, but the geometry itself is opaque-shaped.
- The plugin's index converter swaps corners 1↔2 per triangle to cancel the mirroring X/Y-swap
  world transform. Given the source winding is consistent everywhere, that swap should land these
  meshes correctly — so the live divergence must be in which transform (mirrored vs not) or which
  cull/front-face state actually reaches the PArig/lamp draws.

## 5. Suggested live verification (for the main agent, livetools)

- Trace the *actual* per-instance world transform submitted for a PArig02 / StreetLamp01 instance
  and check its determinant sign — confirm it is the mirroring (det<0) transform the 1↔2 index
  swap assumes. If any of these objects arrive with a **non-mirrored (det>0)** transform, the swap
  double-inverts them → inside-out. That is the most likely culprit given the mesh data is clean.
- Confirm the D3D rasterizer FrontCounterClockwise + CullMode state bound for these draws matches
  the state the two-sided/winding fix assumed (cross-check with prior render-state findings).
- BSMeshLODTriShape vs BSTriShape is the one structural difference between the lamp and the
  known-good wall — verify the runtime isn't taking a different draw/cull path for LOD shapes.
