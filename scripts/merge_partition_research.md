# BSMergeInstancedTriShape — the REAL per-segment partition rule (static RE, decompiler-proven)

Binary: `Fallout4.exe` 1.10.980 (image base 0x140000000, no rebase needed for file VAs).
Method: Ghidra 11.4.3 full analysis (`patches/Fallout4/ghidra` in Vibe-Reverse-Engineering) + r2ghidra + manual RTTI walks.
Date: 2026-07-06. Every claimed offset below has a decompiled function cited in §3.

---

## 0. TL;DR — the answer

The engine has **no per-segment {recordStart, recordCount} table anywhere**. Draws are plain
`DrawIndexed` (never instanced); the per-vertex→record mapping is done **in the vertex shader**
via a **u16 "triangle-group → record index" table** bound as **t7**, with the 80-byte records
bound as **t8**. Both live on the shape:

- `shape+0x178` → table buffer wrapper — **CPU copy IS retained** (`wrapper+0x8`)
- `shape+0x170` → record buffer wrapper — CPU copy NOT retained (GPU only)

The per-segment record partition is **derivable in O(1)** from the CPU-resident u16 table plus the
3-dword segment table at `+0x1A0`. This works for **off-screen clusters** (no draw needed), which
removes the take-12 "only visible clusters get captured" starvation problem.

"Segments" are **up to 3 distance-detail bands** of the precombined cluster (not materials,
not LODs of one model). At distance the engine draws bands `[0..k)`; band `k` alpha-fades in/out.

---

## 1. Plugin-readable extraction recipe

### 1.1 Identify

`S` is a `BSMergeInstancedTriShape*` iff `*(void**)S == base + 0x2698490`
(vftable VA 0x142698490; COL at −8; RTTI `.?AVBSMergeInstancedTriShape@@` TD at 0x14309D9F8).
Object allocation size is exactly **0x1B0** (proven by ctor & CreateClone).

### 1.2 Shape field map (all decompiler-proven)

| Offset | Type | Meaning | Written by |
|---|---|---|---|
| +0x138 | `BSShaderProperty*` | spProperties[1] | base |
| +0x148 | `BSGraphics::TriShape*` | `{+0x00 u64 vertexDesc, +0x08 VB wrapper, +0x10 IB wrapper}` | bake, via 0x1416D5600 |
| +0x158 | u8 | ucType == **5** (draw-dispatch switch key) | 0x14048F0A0 |
| +0x160 | u32 | numTriangles = Σ padded seg counts | bake 0x1421E27D0 |
| +0x164 | u16 | numVertices (unique verts, models deduped) | bake |
| +0x170 | wrapper* | **record structured buffer** (elem 0x50) | 0x1417E8AA0 |
| +0x178 | wrapper* | **u16 group→record table buffer** | 0x1417E8AF0 |
| +0x180..+0x18C | u32×4 | vertex attribute byte offsets (from baked desc) | 0x1417E89E0 |
| +0x190 | u32 | GPU vertex stride (baked; source stride − 8, see §4.6) | 0x1417E89E0 |
| +0x194 | u32 | **groupSize GS** = triangles per group (pow2 ≥ 16, usually 16/32) | 0x1417E89E0 |
| +0x198 | u32 | zero-init scratch | 0x1417E89E0 |
| +0x1A0/+0x1A4/+0x1A8 | u32×3 | **segTris[3]** — padded triangle count per band | bake loop (`while seg < 3`) |
| +0x1AC | u32 | **NEVER WRITTEN — heap junk. Do not read.** | nobody |

Geometry flag: `*(u64*)(S+0x108) & 0x1000` (bit 12) = "segmented / uses fade-node band index".

### 1.3 Index-buffer partition (per segment s ∈ {0,1,2})

```
prefix(s)      = segTris[0..s-1] summed          (only slots 0..2!)
startIndex(s)  = 3 * prefix(s)                   (in u16 indices)
indexCount(s)  = 3 * segTris[s]
active(s)      = segTris[s] != 0
```
IB format is R16_UINT (DrawTriShape passes DXGI_FORMAT 0x39), baseVertex always 0.
The IB pool-slice **byte offset is at IBwrapper+0x48** (passed to IASetIndexBuffer); same for the
VB wrapper (+0x48 → IASetVertexBuffers offset). Padding inside each block is zero-filled indices
(degenerate tris referencing vertex 0) — harmless, but present inside segTris counts.

### 1.4 Record partition (per segment) — the durable rule

```
W178   = *(void**)(S+0x178)                // table wrapper
T      = *(uint16_t**)(W178+0x8)           // CPU copy, RETAINED (owned: *(u8*)(W178+0x4E)==1)
bytes  = *(u32*)(W178+0x34)                // = roundup4(2 * numTriangles/GS)
GS     = *(u32*)(S+0x194)
nGroups= numTriangles / GS                 // exact division, see invariants

firstGroup(s) = prefix(s) / GS
numGroups(s)  = segTris(s) / GS
recordStart(s)= T[firstGroup(s)]
recordCount(s)= T[firstGroup(s)+numGroups(s)-1] - T[firstGroup(s)] + 1
```

Record ordering (proven from the bake worker): records are allocated **segment-major**:
`for band in 0..2: for model-entry in extra-data order: for placement: emit one record`,
and a model emits records for a band **only if it has triangles in that band**. Consequences:
- hedge case: 1 model active in 3 bands × 13 placements = 3 equal blocks of 13, transforms repeated per block ✓
- road case: model A (9 placements) active in one band + model B (4) in another = 9+4 unequal ✓
- `recordStart(s+1) == recordStart(s) + recordCount(s)` across ascending active bands.

### 1.5 Record contents (0x50 = 20 floats each)

| floats | content |
|---|---|
| f[0..2], f[4..6], f[8..10] | rotation rows (f3/f7/f11 = source pad, ignore) |
| f[12..14] | translation, **shape-local** (bake subtracts the merged bound center; the shape's local translate at S+0x60/0x64/0x68 is set to that center) |
| f[15] | scale |
| f[16] | 1.0f (or per-instance secondary scale if extra-data flag set) |
| f[17..19] | **UNINITIALIZED heap garbage — never written. Do not interpret.** |

CPU copy is NOT retained for this buffer. To get record contents:
- keep using the existing t7/t8 DrawCapture (bind-time capture), or
- read back `ID3D11Buffer* = *(void**)(W170+0x0)` via CopyResource→staging
  (W170 = `*(void**)(S+0x170)`; buffer/SRV filled asynchronously — see invariants), or
- validation-grade alternative: the parent **BSFadeNode** keeps a per-record **bounding sphere
  array** at `fadeNode+0x180` (0x10-byte entries {float3 center, float radius}, one per record,
  same order as records; count reserved == recordCount). Spheres are copied from source data
  *without* the center rebase — verify the space live before relying on absolute values.

Record wrapper (created by 0x14184F2E0) layout:
`+0x00 ID3D11Buffer*` (async), `+0x08 ID3D11ShaderResourceView*` (async), `+0x28 HANDLE* uploadEvent`,
`+0x30 u32 pendingUploads`, `+0x34 u32 elementCount (= recordCount)`, `+0x38 u32 typeTag == 2`.

Table wrapper (created by 0x14184EF10) layout:
`+0x00 ID3D11Buffer*`, `+0x08 void* cpuData (retained)`, `+0x18 SRV*`, `+0x28 HANDLE* event`,
`+0x30 u32 capacityBytes`, `+0x34 u32 usedBytes`, `+0x44 u32 pending`, `+0x4E u8 ownsCpuData`.

### 1.6 Current band / fade state (optional, for parity with engine)

`fadeNode = *(void**)(*(void**)(S+0x138) + 0x48)` (BSShaderProperty+0x48 → BSFadeNode*, also the
shape's ancestor). Bytes:
- `+0x11E` u8 = current band count k ∈ {1,2,3} (distance-driven; 3 = all)
- `+0x11D` u8 = fade state {0=restart-fade, 1=fading, 2=stable, 4=band-count-grew}
- `+0x11F` u8 = previous k
- `+0x13C` f32 = fade alpha of the transitioning band
- `+0x108` bit 31 = "band changed, rebuild passes"

The engine draws range `[0, prefix(k))` opaque (pass byte4E = k, bit7 clear) plus one alpha pass
for band k alone (byte4E = k|0x80, pass flag 0x1000000) while fading.

### 1.7 Validation invariants (gate before trusting data)

1. vftable == base+0x2698490; ucType(+0x158) == 5.
2. GS = +0x194 is a power of two, 16 ≤ GS ≤ 128.
3. `numTriangles % GS == 0`; each `segTris[s] % GS == 0`; `Σ segTris[0..2] == numTriangles`.
   **Never read +0x1AC.**
4. Table wrapper: `*(u8*)(W178+0x4E)==1`, `*(u32*)(W178+0x34) == roundup4(2*numTriangles/GS)`,
   CPU ptr non-null and heap-like.
5. Table content: `T[0]==0`; non-decreasing; steps ∈ {0,+1};
   `max(T)+1 == *(u32*)(W170+0x34)` (record count); `*(u32*)(W170+0x38)==2`.
6. Per-band record ranges contiguous and consecutive (§1.4). Any violation ⇒ reject shape.
7. If reading GPU buffers: wait until `*(u32*)(W170+0x30)==0` / `*(u32*)(W178+0x44)==0`
   (pending async upload counters), or WaitForSingleObject on `**(HANDLE**)(W+0x28)`.

---

## 2. Draw-time control flow (how the engine itself consumes the partition)

1. **Band selection** `FUN_142175F70(fadeNode, dist)` (0x142175F70): writes fadeNode+0x11E
   (1/2/3 by distance thresholds at 0x143E47364/370/394/3A0 …, log-scaled by cluster radius),
   +0x11D state machine, +0x13C alpha; sets flags bit31 to force pass rebuild.
2. **Pass building** `FUN_142172540` (BSLightingShaderProperty::GetRenderPasses-like):
   if geom flag bit12: pass→byte4E = fadeNode+0x11E (prefix draw); adds an extra pass with
   byte4E = (fadeNode+0x11E)|0x80 and flag 0x1000000 while +0x11D != 2 (the fading band).
   Merge shapes force technique bits `|= 0x18000000`.
3. **Submit gate** — vftable slot 57 override 0x1417E8B20: reads fadeNode via prop+0x48,
   skips submission when the selected range is empty
   (calls 0x1417E8950 with k and k|0x80).
4. **Segment query** — vftable slot 65 (thunk 0x1417E8E00 → 0x1417E8950):
   `arg&0x80 ? segTris[arg&0x7F] : Σ segTris[0..arg-1]` (triangles).
   Non-virtual sibling 0x1417E88D0: `arg&0x80 ? 3*Σ segTris[0..(arg&0x7F)-1] : 0` (start index).
5. **Draw dispatch** `FUN_142217570`, switch on ucType(+0x158), case 5:
   `geom->GetAsBSMergeInstancedTriShape()` (vtable slot 63, offset 0x1F8, returns `this`) →
   `count = 0x1417E8950(S, pass+0x4E)`, `start = 0x1417E88D0(S, pass+0x4E)` →
   `FUN_141818890(renderer, S->rendererData, start, count)`.
6. **Draw primitive** `FUN_141818890` = Renderer::DrawTriShape:
   `IASetIndexBuffer(ib, R16_UINT, IBwrap+0x48)`, `IASetVertexBuffers(0,1,&vb,&stride,&VBwrap+0x48)`,
   **`DrawIndexed(count*3, start, 0)`** — context vtable +0x60. NOT instanced.
7. **SRV/constant setup** `FUN_1421FDA30` (lighting-shader geometry setup):
   ```
   t4 = SRV of rendererData->VB     (binder 0x14181A2D0)
   t6 = SRV of rendererData->IB     (binder 0x14181A310)
   t7 = SRV of S+0x178 table        (binder 0x14181A350, srv at wrapper+0x18)
   t8 = SRV of S+0x170 records      (binder 0x14181AB40, srv at wrapper+0x8)
   ```
   plus uploads S+0x180..+0x19C into two VS cbuffer registers (register indices from the shader
   descriptor at DAT_143E5AE58 +0x7E/+0x7F) and stores **startIndex (0x1417E88D0 result) into the
   3rd dword of the second register** — the VS reconstructs `group = (startIndex + ordinal)/(3*GS)`
   and fetches its record from t8[t7[group]]. (Exact VS bytecode not traced; not needed for the
   partition.) All binders are `ctx->VSSetShaderResources(slot,1,&srv)` (context vtable +0xC8).

Same t7/t8 binding pattern exists in the other shader setups that call GetAsMerge:
0x14221DC8F-region, 0x14223A6C0-region, 0x142228B50-region (depth/utility/shadow variants) —
this is why binds fire "regardless of visibility" (shadow/depth passes) while color DrawIndexed
only fires when on screen.

---

## 3. Evidence chain (function VAs, what each proves)

| VA | Identity | Proves |
|---|---|---|
| 0x142698490 | BSMergeInstancedTriShape::vftable (66 slots) | overrides at slots 0,26,27,30,57,63,65 |
| 0x14267E948 | BSTriShape::vftable | baseline for the diff |
| 0x1417E8440 | slot 26 = CreateClone | **sizeof == 0x1B0**; +0x170/+0x178 deep-copied under renderer lock (AddRef 0x14181B230 / 0x14181A400) |
| 0x1417E8650 / 0x1417E8720 / 0x1417E8760 | ctors | only +0x170/+0x178 zeroed beyond BSTriShape ⇒ +0x180..+0x1AC uninitialized until bake |
| 0x1417E8860 | dtor head | releases +0x170 (0x14181B240) and +0x178 (0x14181A410), tail-jmp BSTriShape dtor 0x1416DA130 |
| 0x1417E8950 | slot 65 impl | prefix-sum/count semantics over `+0x1A0[i]`, bit 0x80 selector |
| 0x1417E88D0 | non-virtual startIndex helper | `3*prefix` with bit-7 gate; callers 0x1421FDD5F, 0x14221790D, 0x14221F961, 0x14223AC70 |
| 0x1417E8B20 | slot 57 submit gate | fadeNode = prop(+0x138)→+0x48; bytes +0x11D/+0x11E |
| 0x1417E8AA0 | SetRecordBuffer | `+0x170 = CreateStructuredBuffer(0x50, n, data)` |
| 0x1417E8AF0 | SetGroupTable | `+0x178 = CreateDataBuffer(bytes, data)` |
| 0x1417E89E0 | SetLayoutInfo | +0x180..+0x198 field meanings incl. GS at +0x194 |
| 0x14048F0A0 (call site 0x14048F250 / 0x14048F28E) | merge-shape creation from source geom + extra data | parent node = **BSFadeNode** (ctor 0x142174DC0, vft 0x1428FA3E8, 0x1C0) or **BSLeafAnimNode** (0x142177DF0, vft 0x1428FA690, 0x1E0, when prop flags bit 61); gates DAT_142F27BE0/DAT_142F27BF8; propagates geom flag bit 12 |
| 0x1421E27D0 | BSPackedCombined\*GeomDataExtra bake | **`while(seg<3)` writes +0x1A0/1A4/1A8 only**; +0x160=Σ padded; GS derivation (pow2≥16, forced 0x20 when extraData+0x9C>0x800); record buf = n×0x50; table = 2 bytes per group; creates rendererData via 0x141818550 |
| 0x1421E5250 | bake worker (called per (band, model-entry)) | segment-major record order; per-placement block: memcpy seg indices + vertexBase fixup + zero padding to GS; **`T[g] = recordCounter` per group, counter++ per placement**; record float layout incl. translation-rebase at +0x30, scale +0x3C, f[16]=1.0, f[17..19] never written; spheres appended to fadeNode+0x180 array |
| 0x142172540 | pass builder | byte4E assignment (k and k\|0x80), flag bit12 gate, fade alpha +0x13C, technique bits 0x18000000 |
| 0x142175F70 | band selector | +0x11E ∈ {1,2,3}; +0x11D states; +0x11F prev; flags bit31 |
| 0x142217570 | draw dispatch | ucType==5 case; count/start calls; case 3 (plain trishape) uses (0, numTris) confirming units |
| 0x141818890 | Renderer::DrawTriShape | DrawIndexed(count*3, start, 0); R16 IB; **wrapper+0x48 = pool byte offset** |
| 0x1421FDA30 | lighting geometry setup | t4/t6/t7/t8 slot constants; +0x180-block cbuffer upload; startIndex → cbuffer |
| 0x14184F2E0 | async CreateStructuredBuffer | record wrapper layout; CPU copy is upload-command-temp only |
| 0x14184EF10 | async CreateDataBuffer | **table wrapper retains CPU copy at +0x8** (owned flag +0x4E=1) |
| 0x14181A350 / 0x14181AB40 | SRV binders | VSSetShaderResources via ctx vtable +0xC8; srv at wrapper+0x18 / +0x8 |

RTTI anchors (this build): merge TD 0x14309D9F8, BSTriShape TD 0x142FB71B8,
BSFadeNode TD 0x142F9EDD0, BSLeafAnimNode TD 0x1430D0D70,
BSPackedCombinedGeomDataExtra vft 0x14290B6C8, BSPackedCombinedSharedGeomDataExtra vft 0x14290B9A8,
BSCombinedNode vft 0x14290E818, BSCombinedTriShape vft 0x14269B438.

---

## 4. Disproved / corrected (do not retry)

1. **Take-6 “descriptors at +0x1D0”**: the object is 0x1B0 bytes. +0x1D0 reads past the
   allocation — pure heap noise. Dead end forever.
2. **4-dword segment table**: there are exactly **3** slots (+0x1A0..+0x1A8); the bake loop is
   literally `while (seg < 3)` and slot 3 (+0x1AC) is never touched by ctor, bake, clone, or
   draw. The observed “sometimes 4 slots sum to numTriangles” must have been coincidence of heap
   junk; treat any dependence on slot 3 as a bug. (Take 12.1's 3-dword fix is decompiler-confirmed.)
3. **“Equal contiguous record blocks (recordCount/segments)”**: false in general. Real rule =
   segment-major × model-entry × placement, records emitted only for (model, band) pairs with
   triangles (§1.4). 9+4 and 13×3 both fall out of this rule.
4. **Per-segment instancing draws**: the engine never issues DrawIndexedInstanced for these, and
   no {instanceStart, instanceCount} exists anywhere. Record selection is VS-side via t7 table.
5. **Record buffer CPU mirror on the shape**: does not exist. Only the u16 table (+0x178) keeps a
   CPU copy. (The 80-byte records exist CPU-side only transiently inside the bake, freed
   immediately, and inside the async-upload command, freed after upload.)
6. **Vertex-desc caveat**: the bake rewrites the vertex desc — every attribute offset −8 and
   stride −8 vs. the source packed data (block at 0x1421E27D0 head). Read stride/offsets from
   S+0x190/+0x180..0x18C, never assume 32.
7. **“vftable slot 2 fires 2×/frame ⇒ render virtual”**: slot 2 is `GetRTTI` (NiRTTI queries).
   The render-relevant merge virtuals are slot 57 (submit gate) and slot 65 (segment query);
   slot 63 is GetAsBSMergeInstancedTriShape (returns `this`).
8. **f[17..19] of records**: uninitialized heap garbage by construction — not bounding spheres.
   The real per-record spheres live in the BSFadeNode array at +0x180 (0x10 stride).

---

## 5. Suggested live verification (for the main agent)

1. Pick a live merge shape S; check invariants §1.7 — especially `T` monotone and
   `max(T)+1 == *(u32*)(W170+0x34)`.
2. Cross-check take-12.1 captures: recompute per-segment record ranges from `T` and compare with
   the record-anchored validation results (road 9+4 cluster should reproduce exactly).
3. Verify `*(u32*)(S+0x194)` ∈ {16,32} in the wild and `numTriangles % GS == 0`.
4. Read `fadeNode+0x11E` while walking toward/away from a cluster — expect 1→2→3 transitions and
   +0x13C sweeping 0→1 (this also explains any “geometry pops in bands” behavior).
5. Replace DrawCapture chunk starvation: for never-on-screen clusters, extract geometry directly
   from IB/VB CPU pointers (pool wrapper +0x8) using §1.3 ranges, records via staging readback of
   `*(void**)(W170+0x0)` — compare against a DrawCapture-upgraded cluster for bit-exactness.

---

## 6. Draw-state culling investigation (2026-07-07)

**Question:** Do BSMergeInstancedTriShape draws use a rasterizer state with reversed
winding/culling vs plain BSTriShape draws? Are the baked merged index buffers wound opposite to
ordinary mesh data?

### 6.0 VERDICT

**Merge draws use IDENTICAL culling and winding to plain BSTriShape draws. The baked merged index
buffers are wound the SAME as source mesh data — they are NOT reversed.** No mechanism anywhere in
the merge pass/dispatch/bake path flips triangle winding, front-face convention, or cull mode
relative to plain trishapes. If merged geometry looks wrong, culling/winding is not the cause.

Four independent lines of decompiled evidence, below.

### 6.1 The bake copies source indices verbatim — no winding flip (decisive for the IB question)

Bake worker `FUN_1421E5250` copies each placement's index block with a plain **`memcpy`** and then
only adds the per-placement vertex base to each u16 index — no reversal, no determinant test, no
negative-scale handling:
```c
// _Size_00 = iVar4 * 6  == triCount * 3 indices * 2 bytes
memcpy(_Dst, _Src, _Size_00);                 // straight copy, winding order preserved
...
do { psVar1 = (short*)(dst - 2 + lVar15);
     *psVar1 = *psVar1 + local_res8[0];        // += vertexBase only; order untouched
} while (--uVar17);
```
Per-instance transforms are copied into the 0x50 record **verbatim** (rows, translation, scale,
f16=1.0) with no negation or handedness fix-up. Therefore the baked IB winding == source model
winding, and mirrored/negative-scale instances (if any) are NOT compensated at bake time — merged
geometry behaves exactly as the equivalent un-merged instance would.

### 6.2 Draw dispatch is shape-type-agnostic for raster state

`FUN_142217570` switch on ucType(+0x158): case 3 (plain) → `DrawTriShape(rd, 0, numTris)`; case 5
(merge) → `DrawTriShape(rd, 3*prefix, segTris[k])`. Both land in the SAME
`Renderer::DrawTriShape FUN_141818890`, which only sets IB (`IASetIndexBuffer`, R16, ctx+0x98),
VB (`IASetVertexBuffers`, ctx+0x90), primitive topology, and issues `DrawIndexed` (ctx+0x60).
**DrawTriShape touches no rasterizer/cull/winding state.** The only case-5-vs-case-3 difference is
the {startIndex, indexCount} range — never a state change.

### 6.3 Where cull/winding actually comes from: the state flush + rasterizer-state table

Rasterizer state is applied by the pre-draw flush `FUN_141823a40` (called from DrawTriShape and the
batch path). It lazily applies cached D3D11 states from a per-thread render-state struct
(`state = TLS[0xb20] ? : DAT_1438caa98`) driven by dirty bits at `state+0x1b70`:

| dirty bit | ctx vtbl | D3D11 call | index source (state offsets) |
|---|---|---|---|
| 0x2  | +0x160 | RSSetViewports | +0x1c00 (viewport, 6 floats) |
| 0x4  | +0x120 | OMSetDepthStencilState | `DAT_1438caad0[ (+0x1c18)*0x27 + (+0x1c1c) ]` (273 states) or `DAT_1438cb360[(+0x1c20)]` |
| **0x8**  | **+0x158** | **RSSetState** | **`DAT_1438cb3e0[ ((+0x1c28·3 + +0x1c2c)·9 + +0x1c30)·2 + +0x1c34 ]`** |
| 0x10 | +0x118 | OMSetBlendState | `DAT_1438cb740[ ((+0x1c3c + +0x1c38·2)·0xd + +0x1c40) ]` (182 states) |

The **rasterizer-state table `DAT_1438cb3e0`** holds **108 `ID3D11RasterizerState*`** (nested-loop
dimensions **2 × 3 × 9 × 2**, confirmed by the release loop in `FUN_141855fa0`). The index
decomposes into four per-frame variable sub-fields:

| state offset | range | meaning |
|---|---|---|
| +0x1c28 | {0,1} | fill-mode / clip toggle (2-value) |
| **+0x1c2c** | **{0,1,2}** | **CULL MODE (NONE/FRONT/BACK) — the only 3-value dimension** |
| +0x1c30 | {0..8} | depth-bias preset (9-value; shadow/decal) |
| +0x1c34 | {0,1} | scissor/AA toggle (2-value) |

Crucially, **FrontCounterClockwise (winding order) is NOT one of the four variable dimensions** —
it is a compile-time constant baked identically into every one of the 108 rasterizer states. There
is no per-draw winding-flip knob. Cull mode (+0x1c2c) is the only per-draw raster variable that
could differ, and it carries 3 states = D3D CullMode {NONE, FRONT, BACK}, set from the material.

The alternate immediate/batch draw path `FUN_1418284a0` confirms the exact same index math and shows
the cull field is a **per-render-item ushort at item+0x1c** (and depth-bias at item+0x1e):
```c
ctx->RSSetState( DAT_1438cb3e0[ ctx_c34(+0xc4) +
   (( ctx_c28(+0xb8)*3 + item->cull(+0x1c) )*9 + item->bias(+0x1e))*2 ] );  // ctx+0x158
```

### 6.4 The merge technique bit 0x18000000 is a SHADER-permutation bit, not a raster bit

Pass builder `FUN_142172540`: for a merge shape (`GetAsMerge` via vtbl+0x1F8 ≠ 0) it OR-s
`0x18000000` into the **technique word** `uVar19`, then stores it to `pass+0x48`
(`*(uint*)(plVar15+9) = uVar19`) and resolves the shader from it via
`FUN_142200170(DAT_143e4bbd8, pass)` (two-sided family: `FUN_14223a6c0`). That technique word drives
**VS/PS permutation selection** (the merge VS reads t7/t8 to fetch per-instance transforms) — it is
consumed by the shader-technique hashmap `FUN_142229fd0`, and never reaches the rasterizer index
computation in §6.3. Pass byte +0x4E is the band index (draw range), also unrelated to cull.
Geometry flag bit 12 (shape+0x108) gates the band/fade path (byte4E assignment), not cull.

### 6.5 Generic pass setup forces the cull sub-field identically for all shapes

`FUN_142241540` (pass setup, runs before the geometry dispatch regardless of ucType) writes the
rasterizer cull sub-field the same way for every shape:
```c
if (state->0x1c2c != 0) { state->0x1b70 |= 8; state->0x1c2c = 0; }   // cull field forced, dirty bit 8 (RSSetState)
if (state->0x1c20 != 1) { state->0x1b70 |= 4; state->0x1c20 = 1; }   // depth-stencil select
```
Nothing here (or anywhere traced) keys a rasterizer sub-field on `ucType==5` or the merge flag.
Two-sided → CullMode NONE is a property/material decision: a merge shape carries a standard
`BSLightingShaderProperty` (spProperties[1] at shape+0x138), so its two-sided handling maps to the
same cull index as any BSTriShape. (Note: pyghidra offset-scans for the specific two-sided→cull
setter VA were defeated by struct-offset aliasing; the mapping is confirmed structurally — cull is
the single 3-value raster dimension, set per-item/material, shared by both shape classes.)

### 6.6 Evidence chain (this section)

| VA | Identity | Proves |
|---|---|---|
| 0x1421E5250 | bake worker | index block is `memcpy` + vertex-base add; NO winding reversal, NO negative-scale/determinant handling |
| 0x142217570 | draw dispatch | case 3 & case 5 both → DrawTriShape; only the index range differs |
| 0x141818890 | Renderer::DrawTriShape | sets IB/VB/topology + DrawIndexed only; no raster state |
| 0x141823a40 | pre-draw state flush | RSSetState (ctx+0x158, dirty 0x8) indexes DAT_1438cb3e0[108]; winding not a variable dim |
| 0x1418284a0 | batch/immediate draw path | same raster index math; cull=item+0x1c (3-val), bias=item+0x1e (9-val) |
| 0x141855fa0 | render-state table release | rasterizer table dims 2×3×9×2=108; DS=7×39=273; blend=7×2×13=182 |
| 0x141855090 | render-state table create (DS) | CreateDepthStencilState via devwrap DAT_1438caaa0+0x48 vtbl+0xa8 → DAT_1438caad0 |
| 0x142172540 | pass builder | 0x18000000 OR-ed into technique word (pass+0x48), routed to shader resolve — not raster |
| 0x142229fd0 | technique hashmap | consumes technique word to bind VS/PS permutation (where 0x18000000 lands) |
| 0x142241540 | pass setup | forces cull sub-field state+0x1c2c=0 identically for all shape types |

Globals: `DAT_1438cb3e0` = ID3D11RasterizerState*[108] (raster table); `DAT_1438caad0` =
ID3D11DepthStencilState*[273]; `DAT_1438cb740` = ID3D11BlendState*[182]; `DAT_1438caa98` = default
render-state struct; `DAT_1438caaa0` = D3D device wrapper.

### 6.7 Suggested live verification (optional; not required for the verdict)

If the main agent wants a live sanity check: hook `ID3D11DeviceContext::RSSetState` and log the
bound `ID3D11RasterizerState*` immediately before a known merge `DrawIndexed` vs a neighboring plain
static `DrawIndexed` in the same opaque pass — they should be the **same pointer** (same cull +
same FrontCounterClockwise). Alternatively `GetDesc()` both and compare `CullMode` /
`FrontCounterClockwise`. Expectation: identical. This would only ever differ if the two shapes carry
different two-sided material flags, which is orthogonal to the merge system.

---

## 7. Merge VS record math (2026-07-07)

**Question:** exactly how the merge VS transforms vertices by the 80-byte t8 records —
multiply order/orientation (`v·R` row-vector vs `R·v` column-vector), where scale/translation
apply, and how t7 is indexed.

### 7.0 VERDICT (all four deliverables, decompiled from the actual DXBC)

1. **`vertex.x multiplies dwords {0,1,2}` — row-vector `worldPos_local = v·R`.** The stored
   triples `f[0..2]`, `f[4..6]`, `f[8..10]` are the **rows of R** in the row-vector convention
   (`out.j = Σ_i v_i·R[i][j]`), i.e. HLSL `mul(position, (float3x3)record)`. The shader does
   **NOT** dp3 the stored rows against the vertex; it transposes (gathers one element from each
   stored row into a column) and dp3's the vertex against those columns `[f0,f4,f8]`,
   `[f1,f5,f9]`, `[f2,f6,f10]`. So `vertex.x` is distributed across the output as `{f0,f1,f2}`.
2. **`group = (SV_VertexID + startIndex) / (3·GS)`**, exactly as predicted. `startIndex = cb2[7].z`
   (3rd dword of the 2nd cbuffer register), `GS = cb2[7].y`. `t7` is a **raw ByteAddressBuffer of
   packed u16**: the shader byte-addresses `2·(group & ~1)`, loads a dword, and selects the low or
   high u16 by `group&1` — matches the live-proven raw packed-u16 view. `recordIndex = T[group]`.
3. **`localPos = (v·R)·s + t`** — scale `f[15]` multiplies the **rotated** vector, translation
   `f[12..14]` is added **after** (single `mad r, f15, (v·R), (f12,f13,f14)`). Rotate → uniform
   scale → translate.
4. **Yes — the record result then feeds the identical world→view→proj chain as a plain
   trishape.** After the record maps model→cluster-local, `localPos` (w=1) is multiplied by the
   shape's **World** matrix `cb2[0..3]` (translation made camera-relative via `−cb12[35]`), then by
   **ViewProj** `cb12[8..11]`. The record is purely an extra *model→cluster-local* step composed
   *before* the standard world transform (World's translation is the merged-bound center that the
   bake rebased the record translations against — §1.5).

### 7.1 Where the blob came from

- Package `Data\Fallout4 - Shaders.ba2` (BA2 **GNRL** v8, single entry
  `ShadersFX\Shaders011.fxp`, 12,844,936 B uncompressed).
- FXP holds **3939 DXBC blobs** (1339 VS, 2357 PS, 69 HS/69 DS/105 CS). Each blob is preceded by a
  `0x11223344` marker + `u32 blobSize` + `u32 shaderKey` (top byte `0xA0` = Lighting class) + a
  0xFF-padded per-shader constant-remap descriptor. **RDEF is stripped** (only ISGN/OSGN/SHEX/PCSG
  chunks survive) — so shaders were identified by parsing SHEX `dcl_resource_*` opcodes, not names.
- **Merge Lighting VS fingerprint** (the only VS in the package doing per-vertex record indexing):
  `dcl_resource_structured t8, 80` (80-byte records = `0x50`) + `dcl_resource_raw t5,t6,t7`
  (VB / IB / u16 group→record table) + `SV_VertexID`. **40** such VS exist (the Lighting merge
  permutations). A parallel family (**38** VS) uses the same algorithm at shifted slots
  (VB=t8, IB=t9, table=t10, records=t11) — a different BSShader class's merge variant, identical math.
- Disassembled via `D3DDisassemble` (system `d3dcompiler_47.dll`). Representative blobs:
  file-offset **0x38C358** (2684 B, depth-ish 1-texcoord variant) and **0x38AE60** (3824 B).
  The technique-word→blob mapping (runtime `|0x18000000`) is resolved by the engine hashmap
  `FUN_142229FD0` keyed on the file `shaderKey`; that key is **not** a literal `0x18000000` field in
  the file, so the merge VS were confirmed *structurally* (t7/t8 + record fetch) — which exactly
  matches the engine setup `FUN_1421FDA30` and is independently corroborated by cb2[7].z=startIndex
  / cb2[7].y=GS below. This is airtight; the file-side technique key was not further decoded.

### 7.2 The exact record-fetch + transform (disasm, blob 0x38C358)

```asm
; ---- global linear index + manual IB fetch (t6 = raw IB, u16 unpack) ----
iadd r0.x, v0.x, cb2[7].z                 ; idx = SV_VertexID + startIndex   (cb2[7].z = startIndex)
ishl r0.y, r0.x, l(1)                      ; idx*2  (byte addr into R16 IB)
and  r0.z, r0.x, l(1)                      ; parity = idx & 1
imad r0.y, l(-2), r0.z, r0.y               ; even-align: 2*(idx & ~1)
ld_raw_indexable(raw_buffer) r0.y, r0.y, t6.xxxx
ushr r0.w, r0.y, l(16)                      ; hi u16
and  r0.y, r0.y, l(0x0000ffff)             ; lo u16
imul null, r0.w, r0.z, r0.w
iadd r0.z, -r0.z, l(1)
imad r0.y, r0.y, r0.z, r0.w                 ; vertexIndex = parity?hi:lo
; ---- manual VB position fetch (t5 = raw VB, 3x f16 position) ----
imul null, r0.y, r0.y, cb2[7].x            ; byteoff = vertexIndex * stride   (cb2[7].x = stride)
ld_raw_indexable(raw_buffer) r0.yz, r0.y, t5.xxyx
ushr r0.w, r0.y, l(16)
f16tof32 r1.xyz, r0.ywzy                    ; r1 = model-space position (px,py,pz)
; ---- group -> record index (t7 = raw u16 group->record table) ----
imul null, r0.y, cb2[7].y, l(3)            ; 3*GS   (cb2[7].y = groupSize GS)
udiv r0.x, null, r0.x, r0.y                ; group = (SV_VertexID+startIndex) / (3*GS)
ishl r0.y, r0.x, l(1)                       ; group*2 (byte addr, u16 table)
and  r0.x, r0.x, l(1)
imad r0.y, l(-2), r0.x, r0.y                ; 2*(group & ~1)
ld_raw_indexable(raw_buffer) r0.y, r0.y, t7.xxxx
ushr r0.z, r0.y, l(16)
and  r0.y, r0.y, l(0x0000ffff)
imul null, r0.z, r0.x, r0.z
iadd r0.x, -r0.x, l(1)
imad r0.x, r0.y, r0.x, r0.z                 ; recordIndex = T[group]  (u16)
; ---- load the 80-byte record (t8, stride 80) and TRANSPOSE the rotation ----
ld_structured_indexable(stride=80) r0.yzw, r0.x, l(0),  t8.xyxz   ; r0.y=f1  r0.z=f0  r0.w=f2
mov r2.x, r0.z                                                     ; r2.x = f0
ld_structured_indexable(stride=80) r3.xyz, r0.x, l(16), t8.xzyx   ; r3.x=f4  r3.y=f6  r3.z=f5
mov r2.y, r3.x                                                     ; r2.y = f4
ld_structured_indexable(stride=80) r4.xyz, r0.x, l(32), t8.xyzx   ; r4.x=f8  r4.y=f9  r4.z=f10
ld_structured_indexable(stride=80) r5.xyzw, r0.x, l(48), t8.xyzw  ; r5 = f12,f13,f14,f15
mov r2.z, r4.x                                                     ; r2.z = f8   -> r2 = [f0,f4,f8]
dp3 r2.x, r1.xyzx, r2.xyzx                  ; out.x = px*f0 + py*f4 + pz*f8
mov r3.x, r0.w                              ; r3.x = f2
mov r0.z, r3.z                              ; r0.z = f5
mov r0.w, r4.y                              ; r0.w = f9   -> r0.yzw = [f1,f5,f9]
mov r3.z, r4.z                              ; r3.z = f10  -> r3 = [f2,f6,f10]
dp3 r2.z, r1.xyzx, r3.xyzx                  ; out.z = px*f2 + py*f6 + pz*f10
dp3 r2.y, r1.xyzx, r0.yzwy                  ; out.y = px*f1 + py*f5 + pz*f9
; ---- scale then translate: localPos = (v·R)*f15 + (f12,f13,f14) ----
mad r0.xyz, r5.wwww, r2.xyzx, r5.xyzx       ; f15 = scale, (f12,f13,f14) = translation
mov r0.w, l(1.000000)                        ; homogeneous -> r0 = [localPos, 1]
; ---- then World (cb2[0..3], camera-relative) * localPos, then ViewProj (cb12[8..11]) ----
```

**Reading of the three dp3s:** `out.x = px·f0 + py·f4 + pz·f8`, `out.y = px·f1 + py·f5 + pz·f9`,
`out.z = px·f2 + py·f6 + pz·f10`. Therefore `vertex.x (px)` is multiplied by `{f0,f1,f2}` (one per
output lane) ⇒ **stored triple 0 = R's row 0 = row-vector `v·R`.** (Sanity check on the unambiguous
identity-swizzle load `l(48) t8.xyzw` → `r5=[f12,f13,f14,f15]` with `f15` used as scale validates
the byte-offset/swizzle decoding: dword N is at byte offset 4·N.)

Second blob **0x38AE60** reproduces this instruction-for-instruction (same
`(SV_VertexID+startIndex)/(3·GS)`, same t7 u16 unpack, same `t8` offsets `l(0)/l(16)/l(32)/l(48)`
with swizzles `xyxz/xzyx/xyzx/xyzw`, same transpose-gather + dp3, same `mad(f15, v·R, f12..14)`),
and additionally shows per-attribute VB offsets sourced from `cb2[6]`
(`imad r2.xyz, vertexIndex, stride, cb2[6].xzw`) — i.e. cb2[6] carries the S+0x180..+0x18C attribute
offsets, cb2[7] carries {stride, GS, startIndex} = S+0x190/+0x194 + draw startIndex. This matches
§2.7 (two VS cbuffer registers from descriptor +0x7E/+0x7F = registers 6 & 7; startIndex written to
the 3rd dword of the second register = cb2[7].z).

### 7.3 Cbuffer register map (merge Lighting VS)

| Register | Contents | Engine source |
|---|---|---|
| cb2[6].x / .z / .w | vertex-attribute byte offsets | S+0x180..+0x18C |
| cb2[7].x | GPU vertex stride | S+0x190 |
| cb2[7].y | groupSize GS | S+0x194 |
| cb2[7].z | draw startIndex (3rd dword of 2nd reg) | `0x1417E88D0` result, written by `FUN_1421FDA30` |
| cb2[0..3] | shape World matrix (translation camera-relative via −cb12[35]) | standard |
| cb12[8..11] | ViewProj rows | standard |
| t5 / t6 / t7 / t8 | raw VB / raw IB / raw u16 group→record table / structured records (stride 80) | `FUN_1421FDA30` t4/t6/t7/t8 binders (VB slot is t5 here, not t4) |

### 7.4 Consequences for the plugin

- Record application order is unambiguous: `clusterLocal = mul(modelPos, (float3x3)R)*scale + trans`,
  with `R` rows = record `f[0..2] / f[4..6] / f[8..10]`, `scale=f[15]`, `trans=f[12..14]`. To
  reproduce a merged vertex on the CPU (e.g. for baking geometry for the path tracer), use
  **row-vector** multiply `v·R` (equivalently `R^T · v`), **not** `R · v`. Getting this backwards
  transposes the per-instance rotation and would mirror/rotate instances incorrectly.
- The rotation is applied as a pure 3×3 (rows f0-2/f4-6/f8-10); `f3/f7/f11` (padding) are never
  read by the VS — consistent with §1.5.
- t7 indexing is per-vertex `(SV_VertexID + startIndex)/(3·GS)`; since the draw is a plain
  DrawIndexed with `startIndex = 3·prefix(band)` (§1.3), the VS recovers the same group→record
  mapping the CPU-side rule in §1.4 derives from the retained u16 table — so CPU extraction and the
  GPU path are provably consistent.
