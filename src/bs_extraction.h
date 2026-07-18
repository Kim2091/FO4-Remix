#pragma once

#include "remix/remix_c.h"
#include <vector>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <d3d11.h>

struct ExtractedTexture {
    uint64_t hash;
    uint32_t width;            // Mip 0 dimensions
    uint32_t height;
    DXGI_FORMAT dxgiFormat;
    uint32_t mipLevels = 1;    // Number of mip levels packed in `pixels`. >=1.
    std::vector<uint8_t> pixels;  // Tightly packed mip chain: mip0, mip1, ... mipN-1.
                                  // Each mip's dimensions are max(1, w>>i) and max(1, h>>i).
                                  // Format is uniform across all mips (post-decompression
                                  // and post-process this is RGBA8 for BC source textures).
};

// Texture supply list for SubmitDrawable (2026-07-14). Entries SHARE the CPU
// pixel cache's chains instead of copying them: a 2048^2 chain is ~22 MiB and
// the old value-vector paid that memcpy once per supplied slot per submit --
// the dominant per-submit cost after the parse (10-35ms big-merge submits).
// shared_ptr rather than a raw borrow because the cache can erase an entry
// while a supply list is alive in the same tick (superseded-resolution
// variant eviction fires from a sibling slot's decode landing mid-pass);
// shared ownership keeps the pixels valid through SubmitDrawable regardless.
// All producers/consumers are game-thread; refcounting is atomic anyway.
using TextureSupply = std::vector<std::shared_ptr<const ExtractedTexture>>;

struct ExtractedMesh {
    uint64_t hash = 0;
    std::vector<remixapi_HardcodedVertex> vertices;
    std::vector<uint32_t> indices;
    float worldTransform[3][4] = {}; // row-major 3x4 for remixapi_Transform
    uint64_t diffuseTextureHash = 0;    // 0 = no texture
    uint64_t normalTextureHash = 0;     // 0 = no texture
    uint64_t roughnessTextureHash = 0;  // 0 = no texture (FO4 smoothness/spec mask)
    uint64_t emissiveTextureHash = 0;   // 0 = no glow map (from BSLightingShaderMaterialGlowmap)
    float emissiveColorR = 0.0f;        // Emissive color R from BSLightingShaderProperty (0-1)
    float emissiveColorG = 0.0f;        // Emissive color G
    float emissiveColorB = 0.0f;        // Emissive color B
    float emissiveIntensity = 0.0f;     // fEmitColorScale from BSLightingShaderProperty
    bool alphaTestEnabled = false;  // true if NiAlphaProperty has alpha test
    int alphaTestType = 7;          // Remix/VkCompareOp value (7 = Always = no test)
    uint8_t alphaTestRef = 128;     // Alpha reference value (0-255)

    // Alpha blend state (NiAlphaProperty bits 0, 1-4, 5-8 -> VkBlendFactor).
    // When alphaBlendEnabled is true, Remix applies per-instance blend via
    // InstanceInfoBlendEXT chained at DrawInstance time. Material flips
    // opaqueExt.useDrawCallAlphaState=1 so the instance state wins over
    // the material-level alpha test defaults.
    bool alphaBlendEnabled = false;
    uint32_t srcColorBlendFactor = 1;  // VK_BLEND_FACTOR_ONE
    uint32_t dstColorBlendFactor = 0;  // VK_BLEND_FACTOR_ZERO

    // Decal tag (set by lighting_static resolver from BSLightingShaderProperty
    // shader-flag bit). When true, OnFrame ORs REMIXAPI_INSTANCE_CATEGORY_BIT_
    // DECAL_STATIC into the instance categoryFlags so dxvk-remix applies decal
    // depth-offset Z-fight prevention.
    bool isDecal = false;

    // 1st-person viewmodel tag (2026-07-18, set by lighting_static when the
    // geometry descends from PlayerCharacter::firstPersonSkeleton). The 1P
    // graph lives in a SYNTHETIC origin-local space (player-at-origin, world-
    // aligned axes; diag-proven: body z~3, Pip-Boy z~89, weapon z~100 while
    // the player stood at (-79683,90060,7827)); submitting its transforms
    // raw renders the arms at the map origin -- the "viewmodels invisible"
    // bug. OnFrame maps the space into the render world per frame by adding
    // delta = realCameraPos - camBoneSyntheticPos (SemanticCapture::
    // GetViewModelAnchor) to the instance transform (rigid parts) or the
    // bone translations (skinned parts), and hides these instances entirely
    // while the engine has the 1P root app-culled (3rd person, menus).
    bool isViewModel = false;

    // Two-sided tag (set by lighting_static resolver from BSShaderProperty
    // flag bit 36, kTwoSided per CommonLibF4; sanity-anchored by kOwnEmit=22
    // and kDecal=26 which match our confirmed bits). Drives per-instance
    // doubleSided at DrawInstance time so ray traversal keeps backface
    // culling on ordinary opaque geometry -- the previous hardcoded
    // doubleSided=1 paid any-hit/backface cost on every wall and rock.
    bool isTwoSided = false;

    // Worldspace LOD chunk metadata (2026-04-28). When isLODChunk is true,
    // OnFrame applies a spatial filter: skip drawing if the player's world
    // position is INSIDE the chunk's coverage area (the in-cell static refs
    // are already rendering that region with full detail). chunkOriginXY
    // is the chunk's pivot in raw Beth coords; chunkExtent is its side
    // length in Beth units. Set by the resolver from the parent NiNode chain
    // (parent1.name == "chunk" with parent2.name == "4|8|16|32" for terrain
    // LOD; parent2.name == "obj" for object LOD).
    bool  isLODChunk    = false;
    float chunkOriginX  = 0.0f;
    float chunkOriginY  = 0.0f;
    float chunkExtent   = 0.0f;

    // Water tag (2026-04-29). When true, SubmitDrawable builds the material
    // with MaterialInfoTranslucentEXT instead of MaterialInfoOpaqueEXT, so
    // the path tracer treats the surface as a refractive medium (Fresnel
    // reflections + transmittance through to whatever's below). Set only by
    // the water resolver; the renderer is otherwise shader-agnostic.
    // waterTransmittance{R,G,B} are pulled from BSWaterShaderMaterial's
    // kDeepColor at extraction time and become the transmittanceColor of
    // the translucent material -- this lets per-worldspace water tinting
    // (Far Harbor swamp green vs Sanctuary blue vs Glowing Sea sludge)
    // come through automatically from the .esm-authored color slots.
    bool  isWater              = false;
    float waterTransmittanceR  = 0.0f;
    float waterTransmittanceG  = 0.0f;
    float waterTransmittanceB  = 0.0f;

    // Metal conversion (2026-07-02, take 2). FO4 authors metal albedo near-
    // black and builds the metallic look from kSpecularColor/
    // fSpecularColorScale + an environment map -- a pipeline the path tracer
    // doesn't replicate, so untreated kType_Envmap materials (power-armor
    // stands, picket-fence LODs, street lamps) render as black dielectric
    // voids. The lighting resolver classifies kType_Envmap materials and
    // sets:
    //   metallicConstant          > 0 -> SubmitDrawable writes it into the
    //                                    opaque material (else default 0)
    //   roughnessConstantOverride >= 0 -> derived from the material's scalar
    //                                    fSmoothness (else default 0.8)
    // The diffuse itself gets a hue-preserving luminance floor at texture-
    // extraction time; see ExtractMaterialTexture's albedoLumFloor param.
    float metallicConstant          = 0.0f;
    float roughnessConstantOverride = -1.0f;

    // Skinned mesh (2026-07-08). When hasSkinning is true:
    //   - blendWeights/blendIndices are 4-per-vertex remixapi skinning
    //     streams (see ParsedGeometry); SubmitDrawable fills
    //     remixapi_MeshInfoSkinning and the runtime GPU-skins the mesh in
    //     OBJECT space at BLAS build (re-skinned when the per-instance bone
    //     set changes, keyed by boneHash).
    //   - worldTransform is the bare Beth->Remix mirror P: bone matrices
    //     (queued per frame by SkinnedMeshes::UpdateAndQueue) produce Beth
    //     WORLD coordinates, so the instance transform carries only the
    //     mirror -- which also makes the runtime's facing flip fire, same
    //     mechanism as the BatchedMirrorBase fix.
    //   - the mesh cache key is the drawable hash (no cross-drawable mesh
    //     sharing): each actor's BLAS must re-skin against its own bones.
    bool hasSkinning = false;
    uint32_t boneCount = 0;
    std::vector<float>    blendWeights;
    std::vector<uint32_t> blendIndices;
};

struct CellInfo {
    uintptr_t cellPtr;
    uint32_t formID;
};

// Placed light extracted from a cell's LIGH-based references. Revived
// 2026-07-07 (retired with the cell pipeline in phase 1B): with the
// Vault-111 walls fixed, sealed interiors lost the skybox light that had
// been leaking through the holes -- placed lights are the actual
// illumination source and must reach Remix as analytical lights.
struct ExtractedLight {
    uint64_t hash;          // stable ID from the REFR formID
    float position[3];      // world position (Beth X/Y already swapped)
    float radiance[3];      // HDR RGB radiance (pre config multipliers)
    float radius;           // FO4 falloff radius in game units
    bool isSpotLight;       // LIGH flags bit 0x100
    float spotDirection[3]; // spot direction (X/Y swapped)
    float spotFOV;          // full cone angle, degrees
    float spotSoftness;     // cone edge softness
};

// ---------------------------------------------------------------------------
// Texture post-processing modes (shared between bs_extraction and skinning)
// ---------------------------------------------------------------------------
// Texture post-processing modes applied during ExtractMaterialTexture.
//
// - None: pass-through, preserve source format (BC1/BC3/BC7 stay compressed,
//   RGBA8 stays uncompressed). Used for plain diffuse textures.
//
// - InvertRGB: smoothness->roughness conversion. RGB channels are inverted so
//   smoothness=1 (mirror) becomes roughness=0. Decompresses BC1/BC3/BC5 to
//   RGBA8 first; BC7/other passes through unchanged. Used for the
//   spSmoothnessSpecMaskTexture slot.
//
// - Octahedral: tangent-space normal -> hemispherical octahedral encoding.
//   Decompresses BC1/BC3/BC5 to RGBA8 first; BC7/other passes through.
//   Used for the spNormalTexture slot.
//
// - DiffuseAlphaFromLuminance: ONLY for BC1 source format -- decompress to
//   RGBA8 and synthesize alpha = max(R, G, B) so the cutout regions of an
//   atlas (near-black RGB) become transparent. Bethesda's LOD foliage
//   atlases (e.g. Commonwealth.Objects.DDS) are stored as BC1 with no
//   alpha channel; vanilla DX11's rasterizer hides this because cutout
//   regions render as dark blobs that blend into the distance, but Remix's
//   path tracer applies our converted-from-smoothness roughness map at
//   those pixels and produces mirror-reflective rectangles where vanilla
//   showed dark. Synthesizing alpha from luminance gives the path tracer a
//   real alpha channel to test against.
//   Non-BC1 inputs (BC3/BC7/RGBA8) are passed through unchanged because
//   they already have a usable alpha channel. Used for diffuse on
//   alpha-tested or alpha-blended geometry.
// (See full comment block above.)
//
// - DiffuseAlphaFromLuminanceForceBC3: like DiffuseAlphaFromLuminance but
//   also overrides BC3.a on BC3 inputs. Used for decal-tagged surfaces
//   specifically, where BGS sometimes packs non-cutout data in the BC3
//   alpha channel so the authored alpha doesn't behave as a clean mask.
// ForceRGBA8 (2026-07-06 black-merge experiment): decompress BC1/BC3 (incl.
// SRGB) to RGBA8 with authored alpha preserved and no other change. Used to
// discriminate "raw BC upload renders black on merge shapes" from
// material/batching causes.
enum class TexturePostProcess { None, InvertRGB, Octahedral, DiffuseAlphaFromLuminance, DiffuseAlphaFromLuminanceForceBC3, ForceRGBA8 };

// ---------------------------------------------------------------------------
// Common vertex/index extraction result -- shared between static and skinned paths
// ---------------------------------------------------------------------------
struct ParsedGeometry {
    std::vector<remixapi_HardcodedVertex> vertices;
    std::vector<uint32_t> indices;
    uint64_t vertexDesc;
    uint16_t vertexSize;
    uint8_t* vbData;        // raw vertex buffer pointer (for blend weight reading)
    bool isDynamic;

    // Skinning attributes (filled only when parseSkinning is requested AND
    // the shape's vertexDesc has kFlag_Skinned AND the skinning attribute
    // fits the stride). 4 entries per vertex, remixapi layout.
    //
    // VB layout (RenderDoc-confirmed 2026-07-08 + F4SE VertexDesc bitfield:
    // skinning attribute is desc nibble 7): 4x float16 weights followed by
    // 4x uint8 bone indices. The engine's vertex shader uses weights .xyz
    // and reconstructs the 4th as 1-(x+y+z) -- we replicate that instead of
    // trusting the 4th stored half.
    bool hasSkinning = false;
    std::vector<float>    blendWeights;   // numVertices * 4
    std::vector<uint32_t> blendIndices;   // numVertices * 4 (u8 widened)
};

// Forward declarations for F4SE types used in shared function signatures
struct BSTriShape;
struct BSGeometry;
struct BSLightingShaderMaterialBase;
struct NiTexture;

namespace BsExtraction {
    // Returns the player's current parent cell pointer, or 0 if unavailable.
    // Cheap enough to call every frame for cell-change detection.
    uintptr_t GetPlayerCellPtr();

    // Reads the player's world position (TESObjectREFR::pos at +0xD0).
    // Outputs are left unchanged (default 0) when player is unavailable.
    void GetPlayerPosition(float& outX, float& outY, float& outZ);

    // Returns PlayerCharacter::firstPersonSkeleton (+0xB78, STATIC_ASSERT-
    // anchored by playerEquipData/tints asserts in F4SE GameReferences.h),
    // or 0 when the player singleton is absent. The root NiNode of the
    // 1st-person scene graph (arms / weapon / Pip-Boy).
    uintptr_t GetPlayerFirstPersonRootPtr();

    // Lightweight check: player exists, parentCell loaded, cell has objects,
    // and player's 3D root node is present.  Cheap enough to call every frame.
    bool IsPlayerCellReady();

    // Returns all cells currently loaded by the engine (from DataHandler::cellList).
    std::vector<CellInfo> GetLoadedCells();

    // Extract all placed LIGH-reference lights from the given cell.
    // Game thread only (raw reads of the cell's object list).
    std::vector<ExtractedLight> ExtractCellLights(uintptr_t cellPtr);

    // Drop the internal texture cache (call on cell change if desired).
    void ClearTextureCache();

    // Join the async texture-conversion worker threads (see
    // ExtractMaterialTexture's decode pipeline). Queued jobs are dropped.
    // Call once at plugin shutdown; safe to call when no worker ever started.
    void StopTextureConversionWorkers();

    // Age-sweep completed async decodes nobody consumed (drawable evicted
    // mid-decode, resolution fold changed the hash). Called from the Tick
    // sweep cadence: enqueue-time sweeping alone leaves orphans pinned
    // (~22 MiB each, outside every budget) once a streaming burst ends.
    void SweepTextureQueues();

    // Diagnostic (2026-07-06 black-merge investigation): content statistics
    // of a cached extracted texture. Returns false on cache miss. meanRGBA is
    // computed from a stride sample of mip 0 when the cached format is
    // RGBA8 (post-decompression); left zeroed otherwise.
    bool GetCachedTextureStats(uint64_t hash, uint32_t* outW, uint32_t* outH,
                               uint32_t* outFmt, uint32_t outMeanRGBA[4]);

    // --- Shared helper functions (used by both bs_extraction.cpp and skinning.cpp) ---

    // Parse vertices and indices from a BSTriShape. Returns false if the shape
    // should be skipped (effect shader, missing data, NaN positions, bad indices).
    // applyVertexColors: whether the mesh's vertex-color stream (if present)
    // is baked into the output vertices. Callers should pass the shader
    // property's SLSF2_Vertex_Colors bit (merged flag bit 37) -- FO4 meshes
    // often carry painted vertex colors (AO/blend masks, frequently
    // near-black) that the vanilla shader IGNORES unless that flag is set;
    // baking them unconditionally rendered whole objects black. When false
    // (or when the mesh has no color stream), vertices get 0xFFFFFFFF.
    bool ParseShapeGeometry(BSTriShape* shape, ParsedGeometry& out, bool logRejections = true,
                            bool applyVertexColors = true, bool parseSkinning = false);

    // Get the BSLightingShaderMaterialBase from a shape, or nullptr
    BSLightingShaderMaterialBase* GetLightingMaterial(BSTriShape* shape);

    // Current resident WIDTH (px) of a lighting material's diffuse D3D
    // texture, SEH-guarded (0 on null/fault/non-Texture2D). FO4 streams
    // textures in progressively, so the resource behind a material is whatever
    // mip level is currently resident -- often reduced when the object first
    // appears at distance. The Tick's re-capture-on-approach poll compares this
    // live width against the resolution captured at submit
    // (DrawableState::submittedDiffuseWidth) and re-resolves the drawable when
    // the engine has since streamed a sharper mip in, so the full-resolution
    // texture reaches Remix instead of the locked-in blurry first capture.
    uint32_t GetMaterialDiffuseResidentWidth(void* material);

    // Sample a grayscale-to-palette lookup texture (spLookupTexture) at
    // (u,v) in [0,1] -- u the tonal ramp, v the FINAL engine palette row
    // (caller computes GrayscaleToPaletteScale - 1 + pow(vertexColor.x,
    // 1/2.2); see the palette branch in lighting_static.cpp). Returns
    // 0=ready (outRGB = 0xRRGGBB), 1=pending (async readback, retry next
    // tick like any not-yet-ready texture), 2=failed. The decoded LUT is
    // cached and shared with ExtractMaterialTexture's palette remap; a
    // ready (0) return guarantees the remap can find the decoded LUT.
    int SampleLookupColor(NiTexture* lut, ID3D11Device* device,
                          float u, float v, uint32_t& outRGB);

    // Generic texture extraction from any NiTexture slot.
    //
    // minRoughness: when non-zero AND postProcess == InvertRGB, clamps the
    // resulting RGB channels (the roughness output) to >= this value. Used
    // for decal surfaces, where Bethesda's smoothness map is often set to
    // "very smooth" which after InvertRGB becomes roughness near 0 (mirror)
    // -- vanilla DX11 hides this with specular highlights, but the path
    // tracer treats it as a literal mirror. Clamping to ~30% roughness
    // keeps glossy variation while preventing decals from being more
    // reflective than a polished surface should be.
    // albedoLumFloor: when > 0, every pixel whose Rec.709 luminance is below
    // this 8-bit floor is lifted AFTER the postProcess stage -- dark pixels
    // are scaled up multiplicatively (hue preserved, capped at 6x, with a
    // neutral fill for the near-black tail), bright pixels are untouched.
    // Alpha is untouched (cutouts survive). Used by the metal conversion:
    // FO4 metal diffuse is authored near-black and Remix's metal BRDF takes
    // F0 from albedo, so black albedo means a black metal regardless of the
    // metallic constant. Unlike the reverted lift-toward-spec-tint, this
    // never drags hue toward white spec (the weapon-washing failure of
    // 506e5e7). BC inputs are decompressed to RGBA8 first; BC7/other pass
    // through unchanged. The floor is folded into the texture cache hash so
    // metal and non-metal users of one source texture get distinct variants.
    // tintRGB: 0xRRGGBB multiplier applied per-pixel AFTER the postProcess
    // stage (0xFFFFFF = no-op). Used for FO4's tint-material classes
    // (BSLightingShaderMaterialSkinTint / HairTint): their diffuse maps are
    // authored unpigmented (skin) or grayscale (hair) and the vanilla shader
    // multiplies kTintColor in at runtime -- without it NPCs render pale
    // with gray hair. The caller passes the tint gamma-compensated (see the
    // resolver); folded into the texture cache hash so differently-tinted
    // users of one source texture get distinct variants. BC inputs are
    // decompressed to RGBA8 first; BC7/other pass through unchanged.
    // paletteLut/paletteRowV (2026-07-09 grayscale-to-palette remap): when
    // paletteLut is non-null and SampleLookupColor has already decoded it,
    // the diffuse RGB is REPLACED per-pixel with the engine's palette fetch
    //   rgb = LUT(u = pow(diffuseLinear.g, 1/2.2), v = paletteRowV)
    // exactly like the decompiled palette lighting PS (a flat tint cannot
    // reproduce the ramp's dark->bright hue shift, e.g. rust->paint on
    // vehicles). Alpha untouched; composes at the tint slot (after the
    // alpha stage, before the luminance floor); tintRGB then acts only as
    // the fallback for undecodable (BC7) diffuses. paletteRowV is the FINAL
    // engine V (caller applies scale/pow). LUT name + quantized row are
    // folded into the cache hash so meshes sharing one grayscale source but
    // different palette rows get distinct variants.
    // outPending (2026-07-09): when non-null, set true iff the texture is
    // still IN FLIGHT in the async pipeline (GPU readback or worker decode)
    // -- i.e. a retry will eventually return a hash. A 0 return with
    // *outPending == false means the texture genuinely can't be extracted
    // (missing resource, unsupported format, failed conversion). Callers
    // that submit materials should retry while pending instead of shipping
    // a material without the slot: since decode moved off-thread the four
    // slots of one drawable complete at DIFFERENT ticks, and hash-0-means-
    // absent submitted objects permanently missing their normal/roughness.
    //
    // supplyPixels (2026-07-09): false = "probe" mode -- fill hashes, start
    // any not-yet-started readback/decode, consume finished decodes into the
    // internal cache, but NEVER add pixel buffers to newTextures. True =
    // full behavior (pixels re-supplied whenever the Remix-side handle is
    // missing). The resolver probes all slots first and only runs a
    // supplying pass on the attempt that actually submits. Supplied entries
    // are shared_ptr views of the cache's chains (see TextureSupply) --
    // supplying costs a refcount, not a ~22MB memcpy.
    uint64_t ExtractMaterialTexture(NiTexture* tex, const char* slotName,
                                    ID3D11Device* device,
                                    TextureSupply& newTextures,
                                    TexturePostProcess postProcess = TexturePostProcess::None,
                                    uint8_t minRoughness = 0,
                                    uint8_t albedoLumFloor = 0,
                                    uint32_t tintRGB = 0xFFFFFFu,
                                    NiTexture* paletteLut = nullptr,
                                    float paletteRowV = 0.0f,
                                    bool* outPending = nullptr,
                                    bool supplyPixels = true);

    // Extract emissive data from a shape's shader property and material.
    // outPending / supplyPixels forward to the glow-map slot's
    // ExtractMaterialTexture call (see above).
    void ExtractEmissiveData(BSTriShape* shape, BSLightingShaderMaterialBase* lightingMat,
                             ID3D11Device* device, TextureSupply& newTextures,
                             uint64_t& outTexHash, float& outR, float& outG, float& outB, float& outIntensity,
                             bool* outPending = nullptr, bool supplyPixels = true);

    // Read alpha-test + alpha-blend state from the geometry's NiAlphaProperty
    // (effectState slot) and write it into mesh.alphaTest* / alphaBlend* fields.
    // Defaults: alphaTestEnabled=false, alphaTestType=7 (Always), alphaTestRef=128,
    // alphaBlendEnabled=false. Geo may be any BSGeometry-derived shape.
    void ExtractAlphaState(BSGeometry* geo, ExtractedMesh& mesh);
}
