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
