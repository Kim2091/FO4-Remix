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

namespace Resolvers {
namespace Lighting {

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
            default: return "unknown";
        }
    }
}

bool TryResolveStatic(SemanticCapture::DrawableState& state,
                      uint64_t hash,
                      ID3D11Device* device) {
    if (state.submittedToRemix) return true;

    // Mark in-flight immediately so an SEH catch on an early-step crash
    // still reports the right hash.
    ResolverTrace::g_lastHash.store(hash, std::memory_order_relaxed);
    ResolverTrace::g_lastStep.store(Trace::kEntered, std::memory_order_relaxed);

    // state.geometry is NiPointer<NiAVObject>; the implicit operator T*()
    // hands us a raw NiAVObject* that's guaranteed alive (we hold a refcount
    // via the NiPointer). We use Bethesda's RTTI helper to filter non-
    // BSTriShape geometry (particle systems, etc.) that also hits this hook.
    NiAVObject* obj = state.geometry;
    if (!obj) return false;

    BSTriShape* tri = obj->GetAsBSTriShape();
    if (!tri) return false;  // not a BSTriShape (particle system, segmented shape, ...)

    // 1B scope: skip skinned. Skinning regression accepted; later phase revives.
    if (tri->vertexDesc & BSGeometry::kFlag_Skinned) {
        ResolverTrace::g_lastStep.store(Trace::kSkinSkipped, std::memory_order_relaxed);
        return false;
    }

    ResolverTrace::g_lastStep.store(Trace::kCastOK, std::memory_order_relaxed);

    // ---- Parse vertex / index data ----
    ResolverTrace::g_lastStep.store(Trace::kParseStart, std::memory_order_relaxed);
    ParsedGeometry parsed;
    if (!BsExtraction::ParseShapeGeometry(tri, parsed, /*logRejections=*/g_config.logRejections)) {
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

    std::vector<ExtractedTexture> newTextures;
    mesh.diffuseTextureHash = BsExtraction::ExtractMaterialTexture(
        mat->spDiffuseTexture, "diffuse", device, newTextures);
    mesh.normalTextureHash = BsExtraction::ExtractMaterialTexture(
        mat->spNormalTexture, "normal", device, newTextures, TexturePostProcess::Octahedral);
    mesh.roughnessTextureHash = BsExtraction::ExtractMaterialTexture(
        mat->spSmoothnessSpecMaskTexture, "roughness", device, newTextures, TexturePostProcess::InvertRGB);
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
    _MESSAGE("FO4RemixPlugin: [Resolver] submitted hash=0x%llX", (unsigned long long)hash);
    for (const auto& t : newTextures) {
        state.textureHashes.insert(t.hash);
    }

    // Reset trace so we can tell when we're between resolver calls.
    ResolverTrace::g_lastStep.store(Trace::kIdle, std::memory_order_relaxed);
    return true;
}

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

}  // namespace Lighting
}  // namespace Resolvers
