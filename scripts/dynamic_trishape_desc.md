# BSTriShape -> BSDynamicTriShape conversion: vertexDesc rewrite + static VB layout

Binary: `Fallout4.exe` (F4SE 0.7.7 build). Analysis via Ghidra project `patches/Fallout4`.

> **CORRECTION (2026-07-08, live byte-exact verification): the 12-byte dynamicVertices
> element is NOT float3.** It is **half4 position (x, y, z, bitangentX) in the first
> 8 bytes + a 4-byte tail (zeros/flags)**. Everywhere below that says "float3" about
> the BUFFER CONTENT is wrong (the 12*N size math, desc rewrite, offsets, static-VB
> layout, write-once lifecycle, model-space conclusion, and skin-instance findings all
> stand). Proof: [HeadDiag] hex dump of live MaleMouthHumanoidDefault dynamicVertices
> decodes as halfs to the authored NIF half4 positions including EXACT bitangent-X
> matches (0.4834 / 0.4331 / 0.3613 on v1/v2/v3); read as float3 the same bytes are
> the giant "sail" vertices (half pairs reinterpreted as float32 span 1e3..1e38 —
> also the real explanation of the "1e38 sentinel" claim in the runtime section:
> that was this misread, not authored sentinels; gore parts decode to sane positions
> as halfs). Plugin decode: `src/bs_extraction.cpp` reads halfs at bytes 0/2/4 of
> each element whenever the dynamic element size is <= 12.

## TL;DR

`BSTriShape::CreateDynamicTriShape` at **VA 0x141831770** (RVA 0x01831770) is the real converter
(F4SE RVA confirmed). It is driven by a recursive scene-graph walk
(`0x14226E4D0`, the sole caller at `0x14226E554`) that converts every eligible skinned child
geometry into a `BSDynamicTriShape` and swaps it back into the parent node.

For the head-part disk desc **`0x0005B00050430208`** the conversion produces:

| Field | Disk (source) | Runtime (converted) |
|-------|---------------|---------------------|
| n0 szVertexData (static stride) | 8 -> **32 bytes** | 6 -> **24 bytes** (position stripped) |
| n1 szVertex (dynamic size) | 0 (not dynamic) | 3 -> **12 bytes = float3** (NOT float4) |
| n2 UV0 offset | 2 -> byte 8 | 0 -> **byte 0** |
| n4 Normal offset | 3 -> byte 12 | 1 -> **byte 4** |
| n5 Tangent offset | 4 -> byte 16 | 2 -> **byte 8** |
| n7 SkinningData offset | 5 -> byte 20 | 3 -> **byte 12** |
| Vertex flag (bit44) | set | **cleared** |
| FullPrecision (bit54) | clear | **set** |
| UVs/Normals/Tangents/Skinned (45/47/48/50) | set | unchanged (set) |

**Rewritten desc value:** `0x0045A00030210036` (or `0x4045A02030210036` when the "no-0x20000-shader-flag" branch fires — see local_res8 below). Low dword `0x30210036` is deterministic in all cases.

**Runtime static-VB skin-data offset = 12 bytes. Static stride = 24 bytes. Engine formula = `nibble * 4` on the post-conversion desc (the -8 byte position rebase is baked into the desc at conversion, NOT applied at read time).**

For a tool reading the **on-disk** desc, the equivalent is `(nibble_disk - 2) * 4` for every attribute and `(n0_disk - 2) * 4` for the stride, i.e. subtract the disk position footprint of **2 dwords (8 bytes = half4)**. Note this rebase amount (2) is a hardcoded constant, and does NOT equal the new n1 (3).

---

## 1. The converter (0x141831770) — verified

Signature (F4SE): `BSDynamicTriShape* BSTriShape::CreateDynamicTriShape(BSTriShape* this, NiAVObject* owner)`.

Confirmed behavior:
- Allocates **0x1b0** bytes (`FUN_1416579c0(pool, 0x1b0, 0x10, 1)`; F4SE lists object size 0x1A0, the extra 0x10 is the aligned-pool header).
- Runs the `BSDynamicTriShape` constructor `FUN_1416e4740` which writes `*obj = BSDynamicTriShape::vftable` (`0x142680948`).
- Copies numVertices (+0x164), numTriangles (+0x160 <- source +0x160), allocates the dynamic buffer, rewrites the vertexDesc, builds a new geometry-data wrapper, and copies the shared child pointers.

Constructor `FUN_1416e4740(obj, geomData, numVerts, numTris, dynSize)` key writes:
```
*obj                         = BSDynamicTriShape::vftable   // 0x142680948
*(u8*) (obj + 0x158)         = 4          // constant tag (NOT the dyn vertex size; see below)
*(u16*)(obj + 0x164)         = numVerts
*(u32*)(obj + 0x160)         = numTris
*(u32*)(obj + 0x170)         = dynSize    // uiDynamicDataSize = 12 * numVerts
 obj[0x30] (+0x180)          = alloc(dynSize)   // dynamicVertices buffer, 12*numVerts bytes
```
`dynSize` passed in = `iVar14 = (numVerts + numVerts*2) * 4 = 12 * numVerts` -> **float3 positions**.
(+0x158 = 4 is a constant type/stream tag set unconditionally; it is not the per-vertex size —
`uiDynamicDataSize / numVerts = 12`, so `GetDynamicVertexSize()` == 12 == n1*4, matching n1=3.)

---

## 2. The exact desc-rewrite arithmetic (annotated)

`puVar3` = source `pRendererData` (`this[0x29]` = `*(this+0x148)`); the engine's renderer-side
`BSGraphics::TriShape` whose **first u64 is the vertexDesc**. The rewrite is done **in place** on
that desc, then propagated into the new geometry-data wrapper and the new shape's +0x150/+0x148.

```c
puVar3 = (u64*)this[0x29];              // source rendererData
uVar11 = *puVar3;                       // = vertexDesc (disk value, n1==0)
if (((desc & 0xFF) >> 2 & 0x3c) == 0) { // guard: n1 (dynamic size nibble) == 0  -> only convert once
    // ---- Part A: rewrite n0 (low nibble) and force n1 = 3 (high nibble of byte0) ----
    // ((n0*4) - 8) with bits8-9 masked, OR 0xC0, >> 2
    //   n0*4 - 8  = strip 8 bytes (half4 position, 2 dwords) from the static stride
    //   | 0xC0 >>2 = deposit 0x30 -> n1 = 3  (dynamic vertex = 12 bytes)
    //   >> 2       = realign n0 back into the low nibble  (n0_new = n0 - 2)
    // ---- Part B: FullPrecision (bit54) always set; bit62 set when local_res8==0 ----
    // ---- Part C: keep all higher nibbles + flags, but CLEAR Vertex flag (bit44) ----
    desc = ( (((n0*4) - 8) & 0xFFFFFFFFFFFFFCFF | 0xC0) >> 2 )                 // Part A: byte0
         | ( ((local_res8 ^ 1) << 62) + 0x40000000000000 )                    // Part B: bit62?, bit54
         | ( desc & 0xFFFFEFFFFFFFFF30 );                                      // Part C: clear bit44, keep hi

    // ---- rebase every attribute offset nibble n2..n10 by -2 dwords (-8 bytes) ----
    for (shift in {6,10,14,18,22,26,30,34,38}) {   // n2,n3,n4,n5,n6,n7,n8,n9,n10
        v = (desc >> shift) & 0x3c;                 // = nibble * 4
        if (v > 7)                                  // only nibbles >= 2 (real attrs past position)
            desc = ((v - 8) << shift) | (desc & ~(0xF << (shift+2)));  // nibble -= 2
    }

    if (local_res8 == 0)
        desc = (desc & 0xFFFFFF2FFFFFFFFF) | 0x2000000000;   // clear bits36/38/39, set bit37
    *puVar3 = desc;
}
```

`local_res8` = 1 iff the shape has a shader property at `this[0x27]` (+0x138) **and**
`*(u32*)(prop+0x30) & 0x20000` is set; otherwise 0. It only gates the two auxiliary flag bits
(37 and 62) plus the FullPrecision-vs-nothing in Part B; it does NOT change the nibble layout.

### Worked result for desc `0x0005B00050430208`

- Part A: n0*4 = 32; 32-8 = 24; `|0xC0`=0xD8; `>>2`=**0x36** -> byte0 = 0x36 (n0=6, n1=3).
- Part C: `& 0xFFFFEFFFFFFFFF30` -> clears Vertex(44); byte5 0xB0->0xA0; keeps n2/n4/n5/n7.
- Part B: `| bit54` (FullPrecision) [`| bit62` if local_res8==0].
- Rebase loop: n2 2->0, n4 3->1, n5 4->2, n7 5->3.

Final:
```
low  dword = 0x30210036   (byte0=0x36 n1n0, byte1=0x00 n3n2, byte2=0x21 n5n4, byte3=0x30 n7n6)
high dword = 0x0045A000    (local_res8==1)   -> desc = 0x0045A00030210036
high dword = 0x4045A020    (local_res8==0)   -> desc = 0x4045A02030210036
             (byte5=0xA0 UVs45+Normals47, byte6=0x45 Tangents48+Skinned50+FullPrecision54)
```

Answers to the specific questions:
- **(a)** Static stride shrinks **32 -> 24** (n0 8 -> 6). Position bytes are removed from the static VB.
- **(b)** Every attribute offset nibble is **rebased by -2 dwords (-8 bytes)**: UV 8->0, normal 12->4, tangent 16->8, skin 20->12. (Rebase amount is the hardcoded disk position size, not n1.)
- **(c)** n1 goes **0 -> 3** (dynamic vertex size 12 bytes = **float3**, forced by the `|0xC0 >>2` term), not float4.
- **(d)** **Vertex(44) cleared, FullPrecision(54) set.** UVs/Normals/Tangents/Skinned unchanged. Conditional bit37/bit62 set when the shape's shader property lacks flag 0x20000.

---

## 3. Static VB and dynamicVertices buffer construction

**No new static VB or IB is allocated. The source GPU buffers are SHARED.**

`FUN_14181a8f0(pool, srcRendererData, numVerts)` builds a new geometry-data wrapper `puVar1`:
```c
puVar1    = pool_alloc(...);                        // new descriptor object only
puVar1[0] = srcRendererData[0];                     // = rewritten vertexDesc
puVar1[1] = srcRendererData[1] (+0x8  vbWrapper);   // SHARE source vertex-buffer wrapper
puVar1[2] = srcRendererData[2] (+0x10 ibWrapper);   // SHARE source index-buffer wrapper
*(u32*)(puVar1+0x20) = ((desc >> 2) & 0x3c) * numVerts;  // = n1*4*N = 12*N (dynamic-side size)
InterlockedIncrement(&vbWrapper->refcount +0x38);   // refcount the shared buffers
InterlockedIncrement(&ibWrapper->refcount +0x38);
```
Then `FUN_1416d5600(newShape, puVar1)`:
```c
newShape+0x148 = puVar1;            // pRendererData = the new wrapper (shared vb/ib, rewritten desc)
newShape+0x150 = puVar1[0];         // vertexDesc = rewritten desc
```

**dynamicVertices fill** (positions): straight memcpy, no half->float conversion:
```c
dst = *(newShape+0x180);                        // dynamicVertices buffer (12*N bytes)
src = *(FUN_1416bd0b0(this, DAT_143439408) + 0x18);   // pre-existing float3 model-position array
memcpy(dst, src, 12*N);                          // (FUN_1401e4b50 bounds-checked memcpy)
```
`FUN_1416bd0b0(this, key=DAT_143439408)` fetches a CPU-side geometry-data blob the engine keeps
for skinned shapes; its `+0x18` is an already-**float3** rest-pose position array. The half4->float
widening therefore happens at load time when that blob is built, **not** in the converter.

Consequence: the runtime static VB holds only UV/Normal/Tangent/SkinningData at 24-byte stride;
per-vertex positions live in the separate `dynamicVertices` buffer as **float3**, and are
overwritten each frame by CPU skinning/morph before draw.

---

## 4. Engine offset-computation formula (Task 4)

The desc rewrite bakes the position-stripped offsets directly into the nibbles, so the engine's
runtime readers compute:

> **byte offset of attribute X in the static VB = `nibble_X * 4`** using the **post-conversion**
> desc (n7=3 -> skin @ byte 12). Not `(nibble - n1)*4`, not `(nibble + n1)*4`.

n1 (the dynamic vertex size) is not subtracted or added at read time — the -2-dword shift was
already applied during conversion.

For a tool consuming the **disk/NIF** desc directly (n7=5), the runtime static offset is:

> `offset_runtime = (nibble_disk - 2) * 4`  and  `stride_runtime = (n0_disk - 2) * 4`

i.e. subtract 2 dwords (the disk half4 position). For the head part: skin @ `(5-2)*4 = 12`, stride `(8-2)*4 = 24`.

---

## Implications for the FO4-Remix plugin (heads still missing)

The "skinned meshes working" body/armor path uses **float4 (16-byte)** dynamicVertices. This
converter path produces **float3 (12-byte)** dynamicVertices AND a **position-stripped 24-byte**
static VB (skin data at byte 12, UV at byte 0). A parser that assumes, for heads, the same layout
as the working body meshes — float4 dynamic positions and/or a full 32-byte static VB with position
at byte 0 and skin at byte 20 — will misread head geometry. This is a strong candidate root cause
for "FaceGen heads missing."

Concrete guidance:
- For a converted dynamic shape, read the RUNTIME `+0x150` desc and use `nibble*4` directly.
  Do NOT use the on-disk NIF desc offsets for the runtime static VB.
- Expect `GetDynamicVertexSize() == 12` (float3), `uiDynamicDataSize(+0x170) == 12*numVerts`,
  `dynamicVertices(+0x180)` = float3 array, static stride 24, skin @ 12, UV @ 0, normal @ 4, tangent @ 8.

### Suggested live verification
- On a FaceGen head `BSDynamicTriShape`: read `+0x150` (expect low dword `0x30210036`),
  `+0x170` (expect `12*numVerts`), and the shared `vbWrapper` byte size (expect `24*numVerts`,
  confirming the static VB is physically position-less — the one fact the shared-buffer reuse
  leaves open statically).
- Dump the `+0x180` dynamicVertices buffer and confirm 12-byte stride float3 positions.
- Breakpoint `0x141831770` and `0x14226E4D0` to confirm heads actually traverse this converter
  (vs. being authored as dynamic on disk).

---

## Runtime dynamicVertices semantics (2026-07-08)

Follow-up static analysis (Ghidra project `patches/Fallout4`) answering: who writes `+0x180`
at runtime for facegen heads, what happens for gore parts, and the coordinate space.

### Field / helper map (verified)

| VA | Role |
|----|------|
| `0x1416e4930` / `0x1416e4960` | **BSDynamicTriShape::LockDynamicData** (two identical copies): `SimpleLock::Lock(this+0x178,0)` then `return *(this+0x180)`. |
| `0x1416e4990` | **UnlockDynamicData**: recursive-release on the `+0x178`/`+0x17c` SimpleLock (owner at +0x178, recursion count at +0x17c). |
| `0x1402dd4e0` | SimpleLock/BSSpinLock acquire. |
| `0x1416bd0b0` | **BSGeometry::GetExtraData(key)** — binary-searches the extra-data array at geom `+0x20`, returns object (refcounted). |
| `0x1416bd1d0` | **BSGeometry::RemoveExtraData(key)** — matching release/remove. |
| `0x143439408` | `NiFixedString` **"DynPosData"** (literal string at `0x142680400`). |
| `0x1416d5550` | **BSGeometry::SetSkinInstance(+0x140)** — refcounts skin; when skin present, stamps a default (identity) local transform from `_DAT_143437f60..` into shape `+0x30..+0x58`. |
| `0x1416e55b0` | float32 -> **float16** pack. |
| `0x1406f42f0` | float16 -> **float32** unpack. |
| `0x1418a9b90` | half-precision dynamic WRITE: CPU float pos/nrm/tan arrays -> dyn buffer as **float16** at the desc's static attribute offsets. |
| `0x1418a9610` | half-precision dynamic READ-back: dyn buffer float16 -> CPU float arrays (inverse of above). |
| `0x1418303b0` | alternate BSDynamicTriShape creator (0x1b0 alloc, one-time fill via LockDynamicData). |

### 1. Who writes dynamicVertices at runtime for facegen heads — NOBODY (baked once)

The float3 positions are written **exactly once, at conversion time**, inside
`CreateDynamicTriShape` (`0x141831770`):

```
lVar7 = GetExtraData(this, "DynPosData");        // 0x1416bd0b0, key DAT_143439408
...                                              // ctor allocs +0x180 = 12*N bytes
dst   = LockDynamicData(newShape);               // 0x1416e4930 -> newShape+0x180
memcpy(dst, *(lVar7+0x18), 12*N);                // 0x1401e4b50 — float3 verbatim, no widening
RemoveExtraData(this, "DynPosData");             // 0x1416bd1d0 — the CPU blob is then dropped
```

Enumerating **every** `LockDynamicData` caller in the image (both lock copies) gives only:
one-time creators (`CreateDynamicTriShape` + `0x1418303b0`); the half-precision authored-dynamic
sync pair (`0x1418a9b90` write / `0x1418a9610` read) which pack/unpack **position+normal+tangent
as float16 using the static desc offsets**; and three UI/effect quad generators
(`0x14217e600`, `0x1421858b0`, `0x142255670`, all half + screen-dim constants).

**None writes a FullPrecision, position-only float3 buffer.** The half writers would land
normal/tangent at desc offsets 4/8 — which for a converted head (12-byte position-only buffer)
is inside the position — so they provably do **not** touch heads. There is no per-frame
`UpdateModelFace` / `ApplyMorphs` writer into `+0x180`. Facial animation for these
(preprocessed) heads is done entirely by **GPU skinning of the facebones**, not by rewriting
the dynamic buffer. The character's face shape is already baked into the loaded mesh's
DynPosData (this is the `bUseFaceGenPreprocessedHeads` path).

### 2. Gore / hidden dismemberment parts (RearTEMP, NeckGore) — garbage is EXPECTED, faithfully copied

The conversion gate is `param_1[0x27] != 0 && GetExtraData("DynPosData") != 0`. A part with **no**
DynPosData is **not** converted at all (returns 0, stays BSTriShape). Because the gore parts
*are* BSDynamicTriShapes, they shipped a DynPosData block — whose float3 values are the mesh's
**authored sentinel / degenerate rest positions (~1e38)**. The engine `memcpy`s them verbatim:
the buffer is **neither zero-filled nor malloc-garbage — it is a faithful copy of the mesh's
own (garbage-looking) rest positions.** 1e38 ≈ a collapsed/hidden dismemberment part that only
gets real geometry when dismemberment fires. The engine tolerates it (degenerate/zero-area or
dismember-gated); a consumer that trusts the positions and skins them draws verts kilometers
away = the "sails". So: **treat 1e38-class DynPosData as a hide/skip sentinel, not a parse bug.**

### 3. Coordinate space — bind-pose MODEL space, NOT pre-skinned

`dynamicVertices` holds **bind-pose model-space float3** positions — identical convention to a
normal skinned mesh's position stream, just relocated out of the (position-stripped) static VB
into the dynamic buffer. They are **not** CPU-pre-skinned and carry no head-local / camera-relative
bake. The GPU vertex shader skins them with the **same bone-matrix constant path as body
skinning** (posAdjust camera-relative bones from BSSkin). `SetSkinInstance` stamps a default
(identity) local transform into the shape (`+0x30..+0x58`) so the vertices live in skeleton/model
space. No extra head-bone transform, no normalize, no scale.

### 4. Skin instance — kept, never rebound

The conversion walk `0x14226E4D0` converts any child geometry with a non-null skin instance
(`geom+0x140`) and swaps it back into the parent (vft `+0x208`). `CreateDynamicTriShape` copies
the **source** skin instance verbatim: `SetSkinInstance(newShape, source[0x28] /*+0x140*/)`
(`0x1416d5550`). Same bone list, same `BSSkin::BoneData` **inverse binds** as the source head
geometry — there is **no** rebind to a facebones skeleton and **no** identity-bind trick. The
inverse binds are whatever the head NIF authored.

### Net implication for the plugin

- The "bake dynamicVertices once + GPU-skin with BSSkin bones" model is exactly what the engine
  does for these heads — it is correct, provided the positions are read as **float3 (12B), model
  space, position-only** and skinned by the source skin instance.
- The "sails" is the sentinel DynPosData for hidden/gore parts flowing through unskippable.
  Filter shapes whose baked positions contain non-finite / |coord| > ~1e6 magnitudes (per-vertex
  or per-shape), and/or skip the known collapsed dismember parts (RearTEMP, NeckGore). This is
  engine-expected data, not corruption to "fix".

### Suggested live verification
- Breakpoint `0x141831770`; dump the DynPosData blob (`GetExtraData` result `+0x18`, 12*N bytes)
  for a good head part vs. `MaleHeadHumanRearTEMP`/`MaleNeckGore` — expect plausible float3 for
  the former, ~1e38 sentinels for the latter (confirms garbage originates in the mesh, pre-copy).
- Confirm **no** breakpoint at `0x1416e4930`/`0x1416e4960` fires for a head after creation during
  normal animation (proves no per-frame rewrite; animation is bone-driven only).
- Confirm the head's skin instance pointer (`+0x140`) equals the source geometry's (no rebind).

---

## Hair tint color plumbing (2026-07-08)

Where the per-NPC HAIR RGB lives at render time, readable from a plugin holding the hair
`BSTriShape*`. Binary `Fallout4.exe` 1.10.980, Ghidra project `patches/Fallout4`.

### TL;DR pointer chain (all plain derefs from `BSTriShape*`, no vtable calls)

```
BSTriShape*  (hair = BSDynamicTriShape)
  +0x138  BSShaderProperty*   shaderProperty        // BSLightingShaderProperty; material GetType()==2 (Glowmap)
    +0x30   u64    flags                            // EmitColor=bit22, GrayscaleToPalette=bit31, Hair=bit46, TwoSided=bit36
    +0x58   BSShaderMaterial*  shaderMaterial       // BSLightingShaderMaterialGlowmap: +0x68 spLookupTexture (shared LGrad LUT), +0xB8 fLookupScale
    +0xB8   NiColor*  pEmissiveColor  -> [+0x00 R, +0x04 G, +0x08 B] float   <<< THE PER-NPC HAIR RGB
    +0xC8   float   fEmitColorScale                 // multiplier (default 1.0)
```

**Render-time hair color the GPU receives = `pow(pEmissiveColor.rgb * fEmitColorScale, 2.2)`** (sRGB->linear).
For CPU replication read `pEmissiveColor.rgb` directly (that is the authored/applied color); apply
`* fEmitColorScale` and the 2.2 gamma only if you want the exact linear value uploaded to the PS.

The hair material carries **no** color (Glowmap ends at +0xE0); the tint is 100% on the **property**,
exactly as the Skyrim precedent — the property's only `NiColor` is `pEmissiveColor`.

### Task 1 — where the color is WRITTEN (sink = pEmissiveColor)

- `BSLightingShaderProperty::ctor` **0x142171620**: allocates `pEmissiveColor` (a 12-byte `NiColor`,
  `prop[0x17]` = prop+0xB8), sets `fEmitColorScale`(prop+0xC8)=1.0, and by default sets flags
  `EmitColor(22)+GrayscaleToPalette(31)+bit32` (via `SetFlag` **0x142161950**, which is a direct
  `prop+0x30 |= 1<<bit`).
- **Material-load writer** `MaterialFile_TransferColors` **0x142163B00** (called by
  `MaterialFile_ApplyToGeometry` 0x142162D20): copies the BGSM file colors into the property/material:
  - `prop->pEmissiveColor`: `*puVar = file+0x1c` (R,G); `*(u32*)(puVar+1) = file+0x24` (B)  = BGSM emissive/`cEmittanceColor`
  - `prop->fEmitColorScale`(+0xC8) = `file+0x64`
  - material `kSpecularColor`(+0x38)<-file+0xC/+0x14, `fSpecularColorScale`(+0x8C)<-file+0x18,
    `fSmoothness`(+0x88)<-file+0x34, `fLookupScale`(+0xB8)<-file+0x68.
  So the material's authored emissive color SEEDS `pEmissiveColor`; the per-NPC hair CLFM overrides
  the SAME field at runtime.
- **Per-NPC applier**: copies `TESNPC.headData->hairColor` (`BGSColorForm*`, F4SE GameObjects.cpp:47)
  into `prop->pEmissiveColor`. It lives in the facegen/head-assembly path — the large fn at
  ~`0x1406862A0` reads the race `hairColorLUT` (`TESRace+0x6B8`) to resolve a RemappableIndex CLFM
  (BGSColorForm flags+0x40 bit1) into RGB. The exact byte-level write site was not pinned statically
  (giant function); the DESTINATION field is proven by both the material-load writer above and the
  shader reader below. Recommend a live `memwatch` on `prop+0xB8` during hair equip/chargen.
- `SetHairFlag` **0x1402d9ec0** sets the `Hair` flag (bit46) on the 4 biped hair geometry properties
  (`geom+0x138`), does NOT touch color.

### Task 2 — shader constant-buffer setup (the tint feed)

`BSLightingShader` vtable @ **0x14290BFE0**: SetupGeometry=slot7 **0x1421FDA30**,
RestoreGeometry=slot8 0x142201210, texture-bind/SetupMaterial=slot4 0x1421FC630.

In `SetupGeometry` **0x1421FDA30** (`param2`=BSRenderPass; `pass+0x10`=shaderProperty (`lVar35`);
`pass+0x18`=geometry; `prop+0x58`=material):
- **Emit/tint color** (UNCONDITIONAL), decomp lines ~1176-1185:
  ```
  ec    = *(NiColor**)(prop+0xB8);        // pEmissiveColor
  scale = *(float*)(prop+0xC8);           // fEmitColorScale
  PSCB[ reg(0x5a) ].rgb = pow( float3(ec[0],ec[1],ec[2]) * scale, 2.2 );
  ```
  `reg(0x5a) = *(u8*)(DAT_143e5ae70 + 0x5a)` = the PS constant register index inside the lighting
  constant descriptor `DAT_143e5ae70`; `plVar23[1]` = constant-buffer base pointer.
- **LUT lookup scale**, line ~834 (technique-gated): `PSCB[reg(0x5c)] = material->fLookupScale` (mat+0xB8).
- SkinTint/HairTint MATERIALS (`material->GetType()==5`) instead push `material->kTintColor` (mat+0xC0,
  gamma 2.2) into the same `reg(0x5c)` — this path is NOT taken by NPC hair (which is Glowmap type 2).

So for NPC hair the per-object PS tint constant = `pEmissiveColor * fEmitColorScale` (gamma 2.2); no
color comes from the material.

### Task 3 — how the shader combines grayscale + LGrad LUT + tint

`finalDiffuse.rgb ≈ LUT(u = grayscale[ * fLookupScale]).rgb * tint.rgb`, where the grayscale diffuse
is remapped through the shared `HairColor_LGrad` LUT (tonal/gradient ramp only) and the per-NPC RGB
tint = `pEmissiveColor` colorizes it. The LUT is shared game-wide precisely because it carries only
the ramp; the hue is the RGB tint. (Multiply-vs-row-select inferred from: one shared LUT + arbitrary-
RGB CLFM colors + a full float3 tint constant. Exact HLSL op would need the Shaders011.fxp hair PS to
confirm, but the plugin-relevant fact — per-NPC RGB = `pEmissiveColor * fEmitColorScale` — is proven.)

### Task 4 — each hop confirmed offline

| Hop | Offset | Type | Proof |
|-----|--------|------|-------|
| shape -> shaderProperty | +0x138 | BSShaderProperty* | geom+0x138 (KB); SetupGeometry reads pass+0x10=prop; live GetType()==2 |
| prop -> shaderMaterial | +0x58 | BSShaderMaterial* | F4SE BSShaderProperty+0x58; read in MakeValidForRendering + SetupGeometry |
| prop -> flags | +0x30 | u64 | SetFlag 0x142161950; shader flag tests |
| prop -> pEmissiveColor | +0xB8 | NiColor* | ctor allocs prop[0x17]; writer 0x142163B00; reader 0x1421FDA30 |
| pEmissiveColor -> R/G/B | +0x00/+0x04/+0x08 | float | 12-byte NiColor (ctor alloc 0xC) |
| prop -> fEmitColorScale | +0xC8 | float | ctor=1.0; writer file+0x64; shader multiply |

### Suggested live verification
- On a hair `BSDynamicTriShape`: `prop=*(shape+0x138); ec=*(NiColor**)(prop+0xB8);` dump `ec[0..2]`
  and `*(float*)(prop+0xC8)`. Compare `ec.rgb` to `TESNPC::GetHairColor()` `BGSColorForm` color
  (form+0x30 rgb bytes, or race `hairColorLUT[remappingIndex]` when CLFM flags+0x40 bit1 set).
- `memwatch prop+0xB8`'s target during hair equip / RaceMenu to catch the runtime CLFM applier.

---

## Vertex colors & dirt/detail (2026-07-08)

Binary `Fallout4.exe` 1.10.980 (Ghidra project `patches/Fallout4`) + `Data\Fallout4 - Shaders.ba2`
-> `ShadersFX\Shaders011.fxp` (DXBC blobs disassembled via `d3dcompiler_47!D3DDisassemble`).

Answers the plugin question: what is the true "Vertex Colors" shader flag bit in this build, how does the
vanilla lighting PS combine the vertex color, and why does re-multiplying it look "too dirty / gray".

### TL;DR

1. **Vertex Colors flag = merged BIT 37** (flags2 bit 5, mask `1<<37 = 0x0000002000000000`). The plugin's
   current `1<<37` gate is **CORRECT** and is NOT affected by the enum drift that broke bit 46 (Hair).
2. **Combine op (standard technique) = straight per-component linear multiply**:
   `albedo.rgb = diffuse.rgb * vertexColor.rgb`. Not gentler, not alpha-only.
3. **Dirt is NOT a separate texture slot.** It is either baked into the diffuse texture or *is* the per-vertex
   COLOR RGB itself (artist AO/dirt). The one thing that behaves like a hidden "color layer" is the
   **grayscale-to-palette LUT** (`spLookupTexture +0x68`), used by ~1/3 of lighting permutations.
4. **Recommended plugin test**: multiply `vertexColor.rgb` into albedo **iff `(flags & 1<<37)` AND NOT
   `(flags & 1<<4)`** (GrayscaleToPaletteColor). When bit 4 is set the vertex color is a *palette-row selector*,
   not a tint — multiplying it into the (grayscale) diffuse is the most likely cause of "denim -> gray".

### Task 1 — true Vertex-Colors bit = 37 (proven two independent ways)

`SetFlag` (`0x142161950`) takes a **raw qword bit index** (flags1 = bits 0-31, flags2 = bits 32-63).

- **BGSM flag applier `0x142163480`**: `SetFlag(prop, 0x25 /*=37*/, hasVertexColor)`, where `hasVertexColor`
  is computed from the geometry's runtime vertex descriptor color attribute (`(desc >> 22) & 0x3c`, the
  Color nibble n6 byte-offset), **not** from a BGSM bool. So bit 37 = "this geometry has a COLOR stream".
- **`BSLightingShaderProperty::MakeValidForRendering 0x1421718E0`, first instruction**:
  `SetFlag(prop, 0x25, ((geom->vertexDesc >> 22) & 0x3c) != 0)` — the engine re-derives bit 37 from the
  vertex layout every time it validates the property.

Cross-checked anchors from the same applier confirm the whole SLSF map for this build:
`Skinned=bit1`, `GrayscaleToPaletteColor=bit4 (<-mat+0x1bc)`, `GrayscaleToPaletteAlpha=bit5 (<-mat+0x1bd)`,
`EnvMapping=bit7`, `TwoSided=bit36 (<-mat+0x99)`, `Vertex_Colors=bit37`.
Note: the ctor `0x142171620` default-sets bits 22/31/32 (EmitColor / ZBuffer_Test / ZBuffer_Write) — so the
old F4SE/KB label "GrayscaleToPalette = bit31" is itself **drifted/wrong**; the real grayscale-to-palette-color
flag is **bit 4** (flags1 mask `0x10`).

### Task 2 — how vanilla combines vertex color

Disassembly of the standard **deferred** object/character lighting PS (blob `@0x636dc8` in Shaders011.fxp),
albedo path:
```
sample r1.xyz = t0 (diffuse) at uv
mul   r1.xyz, r1.xyz, v6.xyz          ; albedo.rgb = diffuse.rgb * vertexColor.rgb   (v6 = COLOR0)
mad   o0.xyz, r1.xyz, r0.x, r0.yzw    ; GBufferAlbedo = albedo * matScale + emissive/detail additive
```
- Vertex color RGB is a **straight per-component multiply**, in **linear space**, using the color **raw**
  (no `pow`/gamma applied to it). The diffuse `t0` is sampled sRGB->linear; the product is the linear albedo
  written to the GBuffer albedo target.
- Vertex color **alpha** `v#.w` is handled separately: it multiplies the material/diffuse alpha and gates the
  emissive/detail additive (`mul ... , v6.w` / `mad r0.x, diffuseAlpha, v#.w, -alphaRef`). It is **not** folded
  into RGB.
- Forward-lit variant (blob `@0x366e7c`): `o0.xyz = litColor.rgb * vertexColor.rgb + additive` — same multiply,
  just after shading instead of into the GBuffer.

So the op is exactly `finalAlbedo.rgb = diffuse.rgb * vertexColor.rgb` for the standard technique — the plugin's
straight multiply matches vanilla and is **not** too strong *for standard meshes*.

### Task 3 — is "dirt" a separate mechanism?

No dedicated dirt/detail texture slot in the standard technique. Slots sampled by the default lighting PS:
`spDiffuse(+0x48)=t0`, `spNormal(+0x50)=t1`, `spSmoothnessSpecMask(+0x60)=t2`; a glow/detail *additive* from
`t13/t14` gated by `COLOR1.w`; and (palette path only) `spLookupTexture(+0x68)=t5`. There is **no** second dirt
UV set or dirt layer. FO4 "dirt" is therefore one of:
- baked into the diffuse texture, or
- the per-vertex **COLOR RGB** itself (artist-painted AO/dirt), applied as the straight multiply above, or
- (for palette meshes) part of the grayscale-to-palette ramp.

### Task 3b — the grayscale-to-palette trap (the "gray" cause)

When `SLSF1 GrayscaleToPaletteColor` (**bit 4**) is set, the lighting PS takes a different permutation
(blob `@0x7b7e68`): the diffuse texture is a **grayscale** image, and the final color comes from a 2D palette
LUT (`spLookupTexture +0x68` = t5) sampled at `(grayscale diffuse intensity, palette-row)`, where the
**palette row is selected by `vertexColor.x`** (`pow(vc.x, 1/2.2)` -> V coordinate). Here the vertex color is a
*selector*, not a tint, and RGB is NOT multiplied.

Prevalence: of 242 lighting-family COLOR-input pixel shaders in the package, **132 do the straight RGB multiply,
77 use `vertexColor.x` as a grayscale-palette selector, 33 use only alpha/red**. ~1/3 of permutations must not
receive a naive RGB multiply. FO4 uses grayscale-to-palette heavily for clothing and world clutter, which
matches the "systemic across NPCs AND world objects" symptom.

If the plugin (a) shows the raw grayscale diffuse as albedo and (b) multiplies the near-neutral
`vertexColor.rgb` palette-selector into it, the result is `gray * gray = gray` — exactly the reported
"blue denim comes out mostly gray."

### Task 4 — bottom line for the plugin

Given a `BSTriShape` whose shader flags are readable at `prop = *(shape+0x138); flags = *(u64*)(prop+0x30)`:

- **Test to multiply vertex color into albedo**:
  `useVC = (flags & (1ull<<37)) != 0  &&  (flags & (1ull<<4)) == 0`   // has-vertex-colors AND not grayscale-palette
- **Op when useVC**: `albedo.rgb *= vertexColor.rgb`, where `vertexColor` is the raw UNORM color stream
  ([0,1], no gamma applied). This is a straight per-component **linear** multiply — identical to vanilla, so do
  **not** soften it for these meshes. Keep vertex color **alpha** for alpha, do not fold it into RGB.
- **When `flags & (1<<4)` (GrayscaleToPaletteColor)**: do NOT multiply vertex RGB. The true albedo is
  `spLookupTexture(+0x68)` sampled at `(diffuse grayscale, vertexColor.x)`. Minimum fix = skip the vertex-color
  multiply for these meshes; full fix = replicate the palette LUT (or bake per-instance palette color) so the
  base color is not grayscale.
- Optional (path-tracer hygiene, separate from the gray bug): even on standard meshes the vertex color often
  encodes baked AO/dirt; a path tracer recomputes occlusion, so a remaster may choose to attenuate the vertex
  AO to avoid double-darkening. That is an art choice, not a correctness bug — the gray symptom is the
  grayscale-to-palette case above.

### Suggested live verification

- On a "gray denim" NPC clothing `BSTriShape`: read `flags = *(u64*)(*(shape+0x138)+0x30)` and check
  `flags & 0x10` (bit 4, GrayscaleToPaletteColor) and `flags & (1<<37)` (bit 37). Expect bit 4 set on the
  meshes that render gray.
- For such a mesh, confirm `spDiffuse(+0x48)` is a grayscale texture and `spLookupTexture(+0x68)` is a small
  palette/LUT texture; the LUT + vertexColor.x is the missing color.
- On a standard (non-palette) clothing mesh: bit 4 clear, bit 37 set, diffuse is full-color — the plugin's
  RGB multiply is correct there.
