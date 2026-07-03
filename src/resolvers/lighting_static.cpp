#include "lighting_static.h"

#include "../bs_extraction.h"
#include "../semantic_capture.h"
#include "../remix_renderer.h"
#include "../config.h"
#include "../fo4_diagnostics.h"
#include "f4se/NiObjects.h"
#include "f4se/BSGeometry.h"
#include "f4se/NiMaterials.h"
#include "f4se/PluginAPI.h"  // _MESSAGE

#include <atomic>
#include <vector>
#include <cmath>
#include <cstring>

namespace Resolvers {

// In-flight resolver state. Updated at each gate inside TryResolveStatic
// AND inside RemixRenderer::SubmitDrawable (via Trace::SetStep). Read by
// the SEH handler in semantic_capture.cpp's Tick when an access violation
// is caught -- tells us exactly which drawable + step crashed.
//
// The Step enum lives in lighting_static.h so other TUs (remix_renderer,
// semantic_capture) can name the inside-SubmitDrawable / gate constants
// when calling Trace::SetStep.
namespace ResolverTrace {
    std::atomic<int>      g_lastStep{Trace::kIdle};
    std::atomic<uint64_t> g_lastHash{0};

    const char* StepName(int s) {
        switch (s) {
            case Trace::kIdle:                       return "idle";
            case Trace::kEntered:                    return "entered";
            case Trace::kCastOK:                     return "cast_ok";
            case Trace::kSkinSkipped:                return "skin_skipped";
            case Trace::kParseStart:                 return "parse_start";
            case Trace::kParseOK:                    return "parse_ok";
            case Trace::kExtentRejected:             return "extent_rejected";
            case Trace::kBuildMeshOK:                return "build_mesh_ok";
            case Trace::kMaterialFetched:            return "material_fetched";
            case Trace::kLandscapeSkipped:           return "landscape_skipped";
            case Trace::kTexturesExtracted:          return "textures_extracted";
            case Trace::kSubmitStart:                return "submit_start";
            case Trace::kSubmitOK:                   return "submit_ok";
            case Trace::kSubmitFailed:               return "submit_failed";
            case Trace::kSubmit_BeforeTextureCreate:  return "submit_before_texture_create";
            case Trace::kSubmit_AfterTextureCreate:   return "submit_after_texture_create";
            case Trace::kSubmit_BeforeMaterialCreate: return "submit_before_material_create";
            case Trace::kSubmit_AfterMaterialCreate:  return "submit_after_material_create";
            case Trace::kSubmit_BeforeMeshCreate:     return "submit_before_mesh_create";
            case Trace::kSubmit_AfterMeshCreate:      return "submit_after_mesh_create";
            case Trace::kSubmit_GateInputEmpty:       return "submit_gate_input_empty";
            case Trace::kSubmit_GateVram:             return "submit_gate_vram";
            case Trace::kSubmit_GateBudget:           return "submit_gate_budget";
            case Trace::kLODSkipped:                  return "lod_skipped";
            case Trace::kTopFadeNodeSkipped:          return "topfadenode_skipped";
            case Trace::kWorldLODChunkSkipped:        return "world_lod_chunk_skipped";
            default: return "unknown";
        }
    }
}

namespace Lighting {

bool TryResolveStatic(SemanticCapture::DrawableState& state,
                      uint64_t hash,
                      ID3D11Device* device) {
    if (state.submittedToRemix) return true;

    // Mark in-flight immediately so an SEH catch on an early-step crash
    // still reports the right hash.
    ResolverTrace::g_lastHash.store(hash, std::memory_order_relaxed);
    ResolverTrace::g_lastStep.store(Trace::kEntered, std::memory_order_relaxed);

    // state.geometry is void* (set by the hot-path detour). Cast through
    // NiAVObject* so we can use Bethesda's RTTI helper to filter non-BSTriShape
    // geometry (particle systems, etc.) that might also hit BSLightingShaderProperty.
    NiAVObject* obj = static_cast<NiAVObject*>(state.geometry);
    if (!obj) return false;

    BSTriShape* tri = obj->GetAsBSTriShape();
    if (!tri) return false;  // not a BSTriShape (particle system, segmented shape, ...)

    // 1B scope: skip skinned. Skinning regression accepted; later phase revives.
    if (tri->vertexDesc & BSGeometry::kFlag_Skinned) {
        ResolverTrace::g_lastStep.store(Trace::kSkinSkipped, std::memory_order_relaxed);
        return false;
    }

    // Drop engine LOD chunks. NiAVObject::kFlagIsMeshLOD (1<<12) is set at NIF
    // load time on level-of-detail meshes; the engine fires GetRenderPasses
    // for them concurrently with full-detail counterparts during streaming and
    // at distance, producing visible LOD-over-full-detail overlap. Diagnostic
    // run on 2026-04-28 measured 25-45% of all active drawables flagged as
    // LOD across multiple cells. Filtering at submit means these never become
    // Remix mesh handles. Visible cost: cells outside uGridsToLoad render no
    // distant geometry (path tracer handles atmospheric distance).
    //
    // 2026-04-29: DISABLED. The pendingByGate breakdown showed lod=2016 of
    // pending drawables near the player at Concord -- ~40% of all rejections.
    // Visual symptom: HQ structural meshes (walls/floors/roofs) absent while
    // clutter/debris/doors are submitted normally. Hypothesis under test:
    // kFlagIsMeshLOD on FO4 means "geometry participates in the LOD system,"
    // not "geometry IS a low-detail variant" -- so the engine sets it on HQ
    // statics that have LOD counterparts, and our blanket filter dropped them.
    // The chunk-spatial filter (in OnFrame) handles the worldspace-LOD-overlap
    // case this filter was originally added for. If overlap returns visibly,
    // we'll add a smarter discriminator (e.g., require chunk parent metadata).
    // if (state.initialFlags & (1ULL << 12)) {
    //     ResolverTrace::g_lastStep.store(Trace::kLODSkipped, std::memory_order_relaxed);
    //     return false;
    // }

    // NOTE (2026-04-28): two additional filters were tried and reverted.
    //   1. Filter on parent1 kFlagTopFadeNode (bit 14) -- killed buildings,
    //      because TopFadeNode is the engine's general fade-management for
    //      most static refs, not specifically LOD-swap groups.
    //   2. Filter on parent name "chunk" / "obj" -- killed worldspace LOD
    //      chunks (BTR/BTO) which are the WANTED distant rendering, not
    //      the unwanted close-up overlap.
    // The up-close low-quality+low-quality-texture overlap user reports
    // remains unidentified. Hypothesis space narrowed but not resolved.

    ResolverTrace::g_lastStep.store(Trace::kCastOK, std::memory_order_relaxed);

    // Shader-property flag word, read BEFORE the parse because the vertex-
    // color gate below needs it. BSLightingShaderProperty::flags (UInt64 at
    // +0x30) packs shader-flags-1 in the low 32 bits, shader-flags-2 high.
    uint64_t propFlagsEarly = 0;
    if (state.property) {
        propFlagsEarly = *reinterpret_cast<uint64_t*>(
            reinterpret_cast<uintptr_t>(state.property) + 0x30);
    }

    // Vertex-color gate. FO4 meshes routinely CARRY a vertex-color stream
    // that the vanilla shader only multiplies in when SLSF2_Vertex_Colors
    // (flags2 bit 5 -> merged bit 37; layout anchored by kTwoSided = bit 36
    // and kDecal = bit 26, both independently confirmed) is set -- the data
    // is otherwise a shader-specific mask (AO paint, blend weights), often
    // near-black. Baking it unconditionally rendered whole objects black
    // (power-armor stands, chain-link fences, workstations) while their
    // textures loaded fine: the log's WorkstationChemistry propFlags
    // 0x8180400281 has bit 37 CLEAR, yet its painted colors were applied.
    constexpr uint64_t kFlag_VertexColors = 1ULL << 37;
    const bool applyVertexColors = (propFlagsEarly & kFlag_VertexColors) != 0;

    // ---- Parse vertex / index data ----
    ResolverTrace::g_lastStep.store(Trace::kParseStart, std::memory_order_relaxed);
    ParsedGeometry parsed;
    if (!BsExtraction::ParseShapeGeometry(tri, parsed, /*logRejections=*/g_config.logRejections,
                                          applyVertexColors)) {
        return false;
    }

    // Reject shapes with garbage extents (defensive guard against malformed input).
    constexpr float kMaxExtent = 1.0e6f;
    for (const auto& v : parsed.vertices) {
        if (std::abs(v.position[0]) > kMaxExtent ||
            std::abs(v.position[1]) > kMaxExtent ||
            std::abs(v.position[2]) > kMaxExtent) {
            ResolverTrace::g_lastStep.store(Trace::kExtentRejected, std::memory_order_relaxed);
            return false;
        }
    }

    ResolverTrace::g_lastStep.store(Trace::kParseOK, std::memory_order_relaxed);

    // ---- Build mesh ----
    ExtractedMesh mesh{};
    mesh.hash = hash;
    mesh.vertices = std::move(parsed.vertices);
    mesh.indices  = std::move(parsed.indices);
    SemanticCapture::BuildRemixTransform(tri->m_worldTransform, mesh.worldTransform);
    BsExtraction::ExtractAlphaState(tri, mesh);

    // Decal tag. The decal bit is in flags1: bit 26, mask 0x04000000.
    // Confirmed for FO4 1.10.980 by static analysis of the BGSM flag-applier
    // at VA 0x142163480 (called from BSLightingShaderProperty::SetMaterial at
    // 0x142162D7C): it passes bit index 0x1A to SetFlag (RVA 0x02161950) for
    // the decal path, gated on the same source byte that drives bit 27
    // (Dynamic_Decal, 0x08000000). Matches Skyrim's SLSF1_Decal layout.
    // F4SE's published kShaderFlags_* enum at NiProperties.h:125-129 omits
    // this flag. (propFlagsEarly is read above the parse, before the
    // vertex-color gate.)
    {
        constexpr uint64_t kSLSF1_Decal = 0x0000000004000000ULL;
        // Two-sided: bit 36 of the merged 64-bit flag word (flags2 bit 4).
        // CommonLibF4 BSShaderProperty::EShaderPropertyFlag kTwoSided = 1<<36;
        // layout cross-checked against kOwnEmit (22) and kDecal (26), both of
        // which match our independently confirmed bits.
        constexpr uint64_t kFlag_TwoSided = 0x0000001000000000ULL;
        if (propFlagsEarly & kSLSF1_Decal) {
            mesh.isDecal = true;
        }
        if (propFlagsEarly & kFlag_TwoSided) {
            mesh.isTwoSided = true;
        }
    }

    // Detect FO4 worldspace LOD chunk and tag the mesh so OnFrame can apply
    // a spatial filter (skip-when-player-is-inside-coverage). Two patterns
    // identified by the 2026-04-28 parent-chain diagnostic:
    //   parent1.name == "chunk", parent2.name in {"4","8","16","32"} -> terrain LOD
    //     chunkExtent = level * 4096 (one cell == 4096 Beth units)
    //   parent2.name == "obj"                                        -> object LOD
    //     chunkExtent = 16384 (level-4 equivalent; refine if logs show otherwise)
    {
        const char* p1Name = nullptr;
        const char* p2Name = nullptr;
        if (state.parent1) {
            p1Name = static_cast<NiAVObject*>(state.parent1)->m_name.c_str();
        }
        if (state.parent2) {
            p2Name = static_cast<NiAVObject*>(state.parent2)->m_name.c_str();
        }
        float lodLevel = 0.0f;
        if (p1Name && p2Name && std::strcmp(p1Name, "chunk") == 0) {
            // parent2.name is a digit string identifying LOD level.
            if      (std::strcmp(p2Name, "4")  == 0) lodLevel = 4.0f;
            else if (std::strcmp(p2Name, "8")  == 0) lodLevel = 8.0f;
            else if (std::strcmp(p2Name, "16") == 0) lodLevel = 16.0f;
            else if (std::strcmp(p2Name, "32") == 0) lodLevel = 32.0f;
        } else if (p2Name && std::strcmp(p2Name, "obj") == 0) {
            lodLevel = 4.0f;  // assumed; refine with diagnostic if wrong
        }
        if (lodLevel > 0.0f) {
            mesh.isLODChunk   = true;
            mesh.chunkOriginX = tri->m_worldTransform.pos.x;
            mesh.chunkOriginY = tri->m_worldTransform.pos.y;
            mesh.chunkExtent  = lodLevel * 4096.0f;
            // Mirror onto the DrawableState so Tick can maintain the
            // chunk-key index that feeds OnFrame's stale-chunk filter.
            state.isLODChunk  = true;
        }
    }

    ResolverTrace::g_lastStep.store(Trace::kBuildMeshOK, std::memory_order_relaxed);

    // ---- Material + textures ----
    auto* mat = BsExtraction::GetLightingMaterial(tri);
    if (!mat) return false;

    // 1B scope: skip landscape (terrain regression accepted; Phase 5 revives).
    if (mat->GetType() == BSLightingShaderMaterialBase::kType_Landscape) {
        ResolverTrace::g_lastStep.store(Trace::kLandscapeSkipped, std::memory_order_relaxed);
        return false;
    }

    ResolverTrace::g_lastStep.store(Trace::kMaterialFetched, std::memory_order_relaxed);

    // ---- Metal conversion, take 2 (spec-gloss -> metal-rough, 2026-07-02) ----
    // FO4's "shiny metal" pathway is BSLightingShaderMaterialEnvmap: near-
    // black diffuse + kSpecularColor*fSpecularColorScale + cubemap*
    // fEnvmapScale. The path tracer replicates none of that, so untreated
    // envmap materials render as black dielectric voids. Session 2026-07-02
    // log: every user-reported black class (PArig02 power-armor stands,
    // PicketFenceB LODs, street lamps, workstations) is matType==kType_Envmap
    // with the SLSF1_Environment_Mapping propFlags bit set, while fine-
    // rendering controls (picture frames, TVs, vans) are kType_Default.
    //
    // Take 1 (506e5e7, reverted in bb2a02f) targeted this class but failed
    // three ways, each fixed here:
    //   - Gated on fEnvmapScale > 0.01, which skipped the very objects it
    //     was built for (the PA stand reads envScale ~0). Classify on
    //     GetType() alone; log envScale for diagnostics only.
    //   - metallic = 0.9 * envScale: authored-low scales (ShotgunReceiver
    //     0.20) came out nearly dielectric. envScale is a reflection-
    //     intensity knob, not a metalness signal. Modulate by fSmoothness
    //     instead -- wetness-style envmaps (tree bark is kType_Envmap!)
    //     author low smoothness, real metals author high -- with a 0.2
    //     participation floor so nothing classified is fully dielectric.
    //   - Albedo lift BLENDED toward the spec tint, washing white-spec
    //     weapons toward white/sepia. Replaced by a hue-preserving
    //     luminance floor on the diffuse (dark pixels scaled up keeping
    //     their hue, bright pixels untouched) -- see AlbedoLumFloor_Apply.
    uint8_t albedoLumFloor = 0;
    if (g_config.metalConversionEnabled &&
        mat->GetType() == BSLightingShaderMaterialBase::kType_Envmap) {
        float smooth = mat->fSmoothness;
        if (smooth < 0.0f) smooth = 0.0f;
        if (smooth > 1.0f) smooth = 1.0f;
        mesh.metallicConstant = g_config.metalMetallic * (0.2f + 0.8f * smooth);

        float rough = 1.0f - smooth;
        if (rough < g_config.metalMinRoughness) rough = g_config.metalMinRoughness;
        if (rough > 0.95f) rough = 0.95f;
        mesh.roughnessConstantOverride = rough;

        float floorF = g_config.metalAlbedoLumFloor;
        if (floorF < 0.0f) floorF = 0.0f;
        if (floorF > 1.0f) floorF = 1.0f;
        albedoLumFloor = (uint8_t)(floorF * 255.0f + 0.5f);

        static std::atomic<int> sMetalLogs{0};
        const int mn = sMetalLogs.fetch_add(1, std::memory_order_relaxed);
        if (mn < 40) {
            const float envScale =
                static_cast<BSLightingShaderMaterialEnvmap*>(mat)->fEnvmapScale;
            _MESSAGE("FO4RemixPlugin: [Metal] #%d shape=\"%s\" envScale=%.2f smooth=%.2f "
                     "spec=(%.2f,%.2f,%.2f)x%.2f -> metallic=%.2f rough=%.2f lumFloor=%u",
                     mn, tri->m_name.c_str() ? tri->m_name.c_str() : "",
                     envScale, mat->fSmoothness,
                     mat->kSpecularColor.r, mat->kSpecularColor.g, mat->kSpecularColor.b,
                     mat->fSpecularColorScale, mesh.metallicConstant, rough,
                     (unsigned)albedoLumFloor);
        }
    }

    std::vector<ExtractedTexture> newTextures;
    // For alpha-tested or alpha-blended geometry: synthesize alpha from RGB
    // luminance if the diffuse is BC1 (no alpha channel). BGS LOD foliage
    // atlases are stored as BC1; vanilla DX11's rasterizer hides the lack of
    // alpha (cutout regions render as dark blobs), but Remix's path tracer
    // applies our smoothness-derived roughness map at those pixels and
    // produces mirror-reflective rectangles. The synthesized alpha gives the
    // path tracer a real channel to test against.
    //
    // Decals additionally force synthesis on BC3 diffuses: BGS sometimes
    // packs non-cutout data in BC3.a so the authored alpha doesn't behave as
    // a clean mask. Overriding it with luminance gives a usable cutout for
    // the path tracer.
    // Reverted (2026-05-02): forcing luminance synthesis on BC3 decals turned
    // out to destroy perfectly-good authored alpha. Verified by extracting
    // and decoding DecalDebrise05_d.DDS: BC3 alpha is a proper cutout mask
    // (10% near-zero, 23% near-255, full spread across buckets, 17% pass the
    // alphaTest=241 threshold). Bethesda's authored alpha is clean for these
    // BC3 decals; synthesis would replace it with worse luminance-derived
    // values. Use the BC1-only synthesis: BC1 inputs (no alpha at all) get
    // the synthesized cutout, BC3/BC7 inputs preserve their authored alpha.
    const TexturePostProcess diffusePostProcess =
        (mesh.alphaTestEnabled || mesh.alphaBlendEnabled)
            ? TexturePostProcess::DiffuseAlphaFromLuminance
            : TexturePostProcess::None;
    mesh.diffuseTextureHash = BsExtraction::ExtractMaterialTexture(
        mat->spDiffuseTexture, "diffuse", device, newTextures, diffusePostProcess,
        /*minRoughness=*/0, albedoLumFloor);
    mesh.normalTextureHash = BsExtraction::ExtractMaterialTexture(
        mat->spNormalTexture, "normal", device, newTextures, TexturePostProcess::Octahedral);
    // Smoothness/spec-mask (_s.dds) extraction REMOVED (2026-07-02). FO4's
    // packed spec maps translate too inconsistently to naive roughness:
    // "very smooth" authoring became roughness~0 mirrors (decals needed a
    // clamp band-aid; metal fences/racks read as black voids reflecting a
    // dark environment), and per-asset channel packing varies. Dropping the
    // slot leaves mesh.roughnessTextureHash == 0, so SubmitDrawable builds
    // materials with roughnessConstant=0.8 -- and saves a GPU readback +
    // BC decompress + invert per material. Revisit only as part of a real
    // spec-gloss -> metal-rough conversion (spec color/envmap -> metallic).
    BsExtraction::ExtractEmissiveData(tri, mat, device, newTextures,
                                      mesh.emissiveTextureHash,
                                      mesh.emissiveColorR, mesh.emissiveColorG, mesh.emissiveColorB,
                                      mesh.emissiveIntensity);

    // No diffuse -> can't render lit; retry next frame in case texture resolves later.
    if (mesh.diffuseTextureHash == 0) return false;

    ResolverTrace::g_lastStep.store(Trace::kTexturesExtracted, std::memory_order_relaxed);

    // ---- Submit to Remix ----
    ResolverTrace::g_lastStep.store(Trace::kSubmitStart, std::memory_order_relaxed);
    auto status = RemixRenderer::SubmitDrawable(hash, mesh, newTextures);
    if (status != RemixRenderer::SubmitStatus::kSubmitted) {
        ResolverTrace::g_lastStep.store(Trace::kSubmitFailed, std::memory_order_relaxed);
        return false;
    }
    ResolverTrace::g_lastStep.store(Trace::kSubmitOK, std::memory_order_relaxed);

    // Update DrawableState to mark submission and track refcount targets.
    state.submittedToRemix = true;
    state.meshHash = hash;
    // Note: state.materialHash is left at 0 here. SubmitDrawable's hash
    // computation is internal to remix_renderer; if you want symmetric
    // tracking, expose a helper or have SubmitDrawable take an out-param.
    // For 1B, ReleaseDrawable looks up by `hash` and finds the materialHash
    // via g_drawables, so leaving state.materialHash at 0 is fine -- the
    // refcount cleanup goes through g_drawables anyway.
    // LOD-overlap diagnostic (2026-04-28): include geometry name, the
    // static IsMeshLOD bit (from initialFlags captured on first-seen),
    // the live flag word at submit time, technique flag from the hook
    // arg, world position, and the parent NiNode chain (2 levels up,
    // captured in the detour). Parent reads are guarded by null checks;
    // the resolver is wrapped in SEH at the caller, so a stale parent
    // pointer that survived freshness gating gets caught upstream.
    const char* meshName = obj->m_name.c_str();
    const bool  isLOD = ((state.initialFlags >> 12) & 1ULL) != 0;

    const char* p1Name = "";
    uint64_t    p1Flags = 0;
    const char* p2Name = "";
    uint64_t    p2Flags = 0;
    if (state.parent1) {
        auto* pn = static_cast<NiAVObject*>(state.parent1);
        const char* n = pn->m_name.c_str();
        p1Name = n ? n : "";
        p1Flags = pn->flags;
    }
    if (state.parent2) {
        auto* pn = static_cast<NiAVObject*>(state.parent2);
        const char* n = pn->m_name.c_str();
        p2Name = n ? n : "";
        p2Flags = pn->flags;
    }

    // Alpha-test diagnostic (2026-04-29): emit material type, BSShaderProperty
    // shader-flags, and the contents of geo->effectState (the NiAlphaProperty
    // slot at offset 0x130 on BSGeometry per f4se BSGeometry.h). For foliage
    // drawables that render as solid alpha cards, we want to know which
    // alpha-test signal source the engine is using -- NiAlphaProperty (geo
    // level), BSLightingShaderProperty::flags (shader level), or BSLighting-
    // ShaderMaterialBase fields (material level, requires GetType discriminator).
    const uint32_t matType = mat ? mat->GetType() : 0xFFFFFFFFu;
    uint64_t       propFlags = 0;
    if (state.property) {
        propFlags = *reinterpret_cast<uint64_t*>(
            reinterpret_cast<uintptr_t>(state.property) + 0x30);
    }
    void*    effectState     = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(tri) + 0x130);
    uint16_t alphaFlags      = 0;
    uint8_t  alphaThreshold  = 0;
    if (effectState) {
        alphaFlags     = *reinterpret_cast<uint16_t*>(
            reinterpret_cast<uintptr_t>(effectState) + 0x28);
        alphaThreshold = *reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(effectState) + 0x2A);
    }

    // Rotation+scale dump (PROBE 2026-05-03): roads/statics rendering flat
    // when bUsePreCombines=0; need to see whether m_worldTransform.rot
    // arrives as identity (rotation lost upstream) or with the slope intact
    // (then BuildRemixTransform / Remix submission is the leak).
    const auto& rot = tri->m_worldTransform.rot;
    const float scale = tri->m_worldTransform.scale;
    // Leaf RTTI class name (PROBE 2026-05-03): identify whether road/static
    // sub-meshes are plain BSTriShape vs BSMergeInstancedTriShape vs another
    // subclass with per-instance transform attributes we don't handle.
    char leafClass[64] = "";
    SemanticCapture::GetLeafClassName(reinterpret_cast<void*>(tri),
                                      leafClass, sizeof(leafClass));
    _MESSAGE("FO4RemixPlugin: [Resolver] submitted hash=0x%llX name=\"%s\" "
             "leafClass=\"%s\" "
             "isLOD=%d isDecal=%d flags=0x%016llX tech=0x%08X pos=(%.1f,%.1f,%.1f) "
             "p1=\"%s\"(0x%016llX) p2=\"%s\"(0x%016llX) "
             "matType=%u propFlags=0x%016llX effectState=%p "
             "alphaFlags=0x%04X alphaThreshold=%u alphaTestEnabled=%d "
             "alphaBlendEnabled=%d srcFactor=%u dstFactor=%u "
             "rot=[%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f] scale=%.3f",
             (unsigned long long)hash,
             meshName ? meshName : "(null)",
             leafClass[0] ? leafClass : "(unknown)",
             isLOD ? 1 : 0,
             mesh.isDecal ? 1 : 0,
             (unsigned long long)state.lastFlags,
             state.lastTechniqueFlags,
             tri->m_worldTransform.pos.x,
             tri->m_worldTransform.pos.y,
             tri->m_worldTransform.pos.z,
             p1Name, (unsigned long long)p1Flags,
             p2Name, (unsigned long long)p2Flags,
             matType, (unsigned long long)propFlags, effectState,
             alphaFlags, alphaThreshold, mesh.alphaTestEnabled ? 1 : 0,
             mesh.alphaBlendEnabled ? 1 : 0,
             mesh.srcColorBlendFactor, mesh.dstColorBlendFactor,
             rot.data[0][0], rot.data[0][1], rot.data[0][2],
             rot.data[1][0], rot.data[1][1], rot.data[1][2],
             rot.data[2][0], rot.data[2][1], rot.data[2][2],
             scale);
    for (const auto& t : newTextures) {
        state.textureHashes.insert(t.hash);
    }

    // Reset trace so we can tell when we're between resolver calls.
    ResolverTrace::g_lastStep.store(Trace::kIdle, std::memory_order_relaxed);
    return true;
}

}  // namespace Lighting

// ---- Trace accessors (for SEH handler in semantic_capture.cpp) ----
namespace Trace {
    int LastStep() {
        return ResolverTrace::g_lastStep.load(std::memory_order_relaxed);
    }
    uint64_t LastHash() {
        return ResolverTrace::g_lastHash.load(std::memory_order_relaxed);
    }
    const char* StepName(int s) {
        return ResolverTrace::StepName(s);
    }
    void SetStep(int s) {
        ResolverTrace::g_lastStep.store(s, std::memory_order_relaxed);
    }
}

}  // namespace Resolvers
