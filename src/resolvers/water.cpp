#include "water.h"

#include "../bs_extraction.h"
#include "../semantic_capture.h"
#include "../remix_renderer.h"
#include "../config.h"
#include "../fo4_diagnostics.h"
#include "f4se/NiObjects.h"
#include "f4se/BSGeometry.h"
#include "f4se/NiMaterials.h"  // BSWaterShaderMaterial
#include "f4se/PluginAPI.h"  // _MESSAGE
#include "lighting_static.h"  // for Resolvers::Trace

#include <atomic>
#include <cmath>
#include <vector>

namespace Resolvers {
namespace Water {

// Sentinel hash for the synthetic 1x1 RGBA8 blue diffuse texture. Stable
// across runs so toolkit USD replacement targets a known hash. All water
// drawables share this same texture handle (refcounted in g_textureHandles).
constexpr uint64_t kSyntheticDiffuseHash = 0xFA11FA11FA11FA11ULL;

bool TryResolve(SemanticCapture::DrawableState& state,
                uint64_t hash,
                ID3D11Device* device) {
    static std::atomic<uint64_t> sDispatchCount{0};
    const uint64_t n = sDispatchCount.fetch_add(1, std::memory_order_relaxed);
    if (n < 10) {
        _MESSAGE("FO4RemixPlugin: [ResolverWater] DISPATCH #%llu hash=0x%llX geo=%p prop=%p submitted=%d",
                 (unsigned long long)n, (unsigned long long)hash,
                 state.geometry, state.property, state.submittedToRemix ? 1 : 0);
    }
    if (state.submittedToRemix) return true;

    Resolvers::Trace::SetStep(Resolvers::Trace::kEntered);

    NiAVObject* obj = static_cast<NiAVObject*>(state.geometry);
    if (!obj) return false;

    BSTriShape* tri = obj->GetAsBSTriShape();
    if (!tri) return false;

    // Skip skinned (out of scope; lighting resolver same).
    if (tri->vertexDesc & BSGeometry::kFlag_Skinned) {
        Resolvers::Trace::SetStep(Resolvers::Trace::kSkinSkipped);
        return false;
    }

    Resolvers::Trace::SetStep(Resolvers::Trace::kCastOK);

    // Parse vertex / index data (shader-agnostic).
    Resolvers::Trace::SetStep(Resolvers::Trace::kParseStart);
    ParsedGeometry parsed;
    if (!BsExtraction::ParseShapeGeometry(tri, parsed,
                                          /*logRejections=*/g_config.logRejections)) {
        return false;
    }

    constexpr float kMaxExtent = 1.0e6f;
    for (const auto& v : parsed.vertices) {
        if (std::abs(v.position[0]) > kMaxExtent ||
            std::abs(v.position[1]) > kMaxExtent ||
            std::abs(v.position[2]) > kMaxExtent) {
            Resolvers::Trace::SetStep(Resolvers::Trace::kExtentRejected);
            return false;
        }
    }

    Resolvers::Trace::SetStep(Resolvers::Trace::kParseOK);

    // Build mesh -- plain opaque, synthetic blue diffuse.
    ExtractedMesh mesh{};
    mesh.hash = hash;
    mesh.vertices = std::move(parsed.vertices);
    mesh.indices  = std::move(parsed.indices);
    SemanticCapture::BuildRemixTransform(tri->m_worldTransform, mesh.worldTransform);

    // Force opaque. BSWaterShaderProperty drawables don't carry NiAlphaProperty,
    // so the defaults from BsExtraction::ExtractAlphaState would already give
    // alphaTestEnabled=false / alphaBlendEnabled=false -- but we set them
    // explicitly here for clarity. dxvk-remix's path-tracer pipeline cannot
    // consume per-instance alpha-blend state (see spec's "What we tried and
    // reverted" section); plain opaque is the only viable mode.
    mesh.alphaTestEnabled  = false;
    mesh.alphaBlendEnabled = false;

    Resolvers::Trace::SetStep(Resolvers::Trace::kBuildMeshOK);

    // Pull spNormalMap01 from the water material into the actual normal slot
    // so the path tracer reads ripple bump as surface perturbation (Fresnel
    // glints, refraction angle variation) instead of as flat diffuse color.
    // Hash is content-derived inside ExtractMaterialTexture (deterministic
    // over the .dds bytes), so it stays stable across game restarts --
    // toolkit USD replacements keyed by this hash keep working session-to-
    // session.
    //
    // Post-process: Octahedral. dxvk-remix's translucent and opaque material
    // shaders both unconditionally octahedral-decode normalSample.xy via
    // unsignedOctahedralToHemisphereDirection (see translucent_surface_
    // material_interaction.slangh). Submitting raw tangent-space normals
    // gives the BRDF an arbitrary direction that happens to look "wave-ish"
    // at oblique angles but is wrong everywhere else -- and it blocks the
    // animated-water path because the dual-layer normal blend (normalBlendRNM)
    // expects properly decoded hemisphere normals. ConvertNormalToOctahedral
    // works on standard tangent-space DDS input (BC5 or RGB) and emits the
    // RG-only octahedral encoding the shader expects.
    //
    // Diffuse always gets the synthetic 1x1 blue. Path-tracer translucent
    // BRDF (useDiffuseLayer=0 in SubmitDrawable below) won't sample it, but
    // SubmitDrawable's diffuse-loaded gate requires a non-zero hash; the
    // synth keeps that gate passing without leaking color into the water
    // surface. The g_textureHandles cache dedupes the synth across all
    // water drawables, so pushing it every call is essentially free.
    std::vector<ExtractedTexture> newTextures;
    auto* waterMat = static_cast<BSWaterShaderMaterial*>(state.material);
    if (waterMat) {
        mesh.normalTextureHash = BsExtraction::ExtractMaterialTexture(
            waterMat->spNormalMap01, "water_normal", device, newTextures,
            TexturePostProcess::Octahedral);
        mesh.isWater             = true;
        mesh.waterTransmittanceR = waterMat->kDeepColor.r;
        mesh.waterTransmittanceG = waterMat->kDeepColor.g;
        mesh.waterTransmittanceB = waterMat->kDeepColor.b;
    }
    {
        ExtractedTexture synth;
        synth.width      = 1;
        synth.height     = 1;
        synth.dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        synth.pixels     = { 64, 96, 160, 255 };  // RGBA(0.25, 0.38, 0.63, 1.0) -- solid blue
        synth.hash       = kSyntheticDiffuseHash;
        newTextures.push_back(synth);
        mesh.diffuseTextureHash = kSyntheticDiffuseHash;
    }

    Resolvers::Trace::SetStep(Resolvers::Trace::kTexturesExtracted);

    // Submit.
    Resolvers::Trace::SetStep(Resolvers::Trace::kSubmitStart);
    auto status = RemixRenderer::SubmitDrawable(hash, mesh, newTextures);
    if (status != RemixRenderer::SubmitStatus::kSubmitted) {
        // Rate-limited failure log: this used to fail silently, and a
        // persistently-failing water drawable re-parses its whole geometry
        // on every poll with nothing in the log to say why water is absent.
        static std::atomic<int> sFailLogs{0};
        const int n = sFailLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 12) {
            _MESSAGE("FO4RemixPlugin: [ResolverWater] SubmitDrawable FAILED #%d "
                     "hash=0x%llX name=\"%s\"",
                     n, (unsigned long long)hash,
                     obj->m_name.c_str() ? obj->m_name.c_str() : "(null)");
        }
        Resolvers::Trace::SetStep(Resolvers::Trace::kSubmitFailed);
        return false;
    }
    Resolvers::Trace::SetStep(Resolvers::Trace::kSubmitOK);

    state.submittedToRemix = true;
    state.meshHash = hash;
    for (const auto& t : newTextures) {
        state.textureHashes.insert(t.hash);
    }

    _MESSAGE("FO4RemixPlugin: [ResolverWater] submitted hash=0x%llX name=\"%s\" "
             "pos=(%.1f,%.1f,%.1f)",
             (unsigned long long)hash,
             obj->m_name.c_str() ? obj->m_name.c_str() : "(null)",
             tri->m_worldTransform.pos.x,
             tri->m_worldTransform.pos.y,
             tri->m_worldTransform.pos.z);

    Resolvers::Trace::SetStep(Resolvers::Trace::kIdle);
    return true;
}

}  // namespace Water
}  // namespace Resolvers
