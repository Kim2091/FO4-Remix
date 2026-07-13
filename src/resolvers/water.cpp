#include "water.h"

#include "../bs_extraction.h"
#include "../semantic_capture.h"
#include "../remix_renderer.h"
#include "../config.h"
#include "../fo4_diagnostics.h"
#include "f4se/NiObjects.h"
#include "f4se/BSGeometry.h"
#include "f4se/NiMaterials.h"  // BSWaterShaderMaterial
#include "f4se/NiProperties.h" // BSShaderProperty (live material re-fetch)
#include "f4se/PluginAPI.h"  // _MESSAGE
#include "lighting_static.h"  // for Resolvers::Trace

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
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

    // Re-fetch the water material from the LIVE shader property on every
    // attempt instead of trusting the detour-captured state.material: the
    // engine can swap or free a material between the hot-path capture and a
    // (possibly much later) resolve, and a stale-but-mapped pointer reads as
    // garbage kDeepColor / a dangling spNormalMap01 with nothing to catch it.
    // The lighting resolver re-fetches live for exactly this reason
    // (BsExtraction::GetLightingMaterial). Gate on the RTTI leaf class, not
    // GetFeature(): feature values are build-dependent, the class name isn't.
    BSWaterShaderMaterial* waterMat = nullptr;
    if (NiProperty* prop = tri->shaderProperty) {
        BSShaderMaterial* liveMat =
            static_cast<BSShaderProperty*>(prop)->shaderMaterial;
        if (liveMat) {
            char leaf[64] = "";
            SemanticCapture::GetLeafClassName(liveMat, leaf, sizeof(leaf));
            if (std::strcmp(leaf, "BSWaterShaderMaterial") == 0) {
                waterMat = static_cast<BSWaterShaderMaterial*>(liveMat);
            }
        }
    }
    Resolvers::Trace::SetStep(Resolvers::Trace::kMaterialFetched);

    // Ripple normal map, probed BEFORE the geometry parse. The async texture
    // pipeline returns 0-and-pending on the first attempt(s) for any
    // not-yet-cached texture, and water's synthetic diffuse always passes
    // SubmitDrawable's diffuse gate -- so submitting while the normal decode
    // was still in flight made the submission PERMANENT with
    // normalTextureHash == 0 (no re-resolve poll exists for water), silently
    // killing the ripple feature on the common path. Pending -> retry next
    // tick via the normal backoff; probing here costs no parse. A 0 hash
    // with pending == false means the slot genuinely has no extractable
    // texture -- submit flat as before.
    std::vector<ExtractedTexture> newTextures;
    uint64_t normalHash = 0;
    if (waterMat) {
        bool pendNormal = false;
        normalHash = BsExtraction::ExtractMaterialTexture(
            waterMat->spNormalMap01, "water_normal", device, newTextures,
            TexturePostProcess::Octahedral,
            /*minRoughness=*/0, /*albedoLumFloor=*/0, /*tintRGB=*/0xFFFFFFu,
            /*paletteLut=*/nullptr, /*paletteRowV=*/0.0f, &pendNormal);
        if (pendNormal) return false;
    }

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

    // spNormalMap01 (probed and extracted above) goes into the actual normal
    // slot so the path tracer reads ripple bump as surface perturbation
    // (Fresnel glints, refraction angle variation) instead of as flat
    // diffuse color. Hash is content-derived inside ExtractMaterialTexture
    // (deterministic over the .dds bytes), so it stays stable across game
    // restarts -- toolkit USD replacements keyed by this hash keep working
    // session-to-session.
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
    if (waterMat) {
        mesh.normalTextureHash = normalHash;
        mesh.isWater             = true;
        // Clamp: the live re-fetch + RTTI gate make garbage unlikely, but a
        // mid-frame material swap can still hand us junk floats, and
        // out-of-range transmittance ships silently wrong water color.
        auto clamp01 = [](float v) {
            return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
        };
        mesh.waterTransmittanceR = clamp01(waterMat->kDeepColor.r);
        mesh.waterTransmittanceG = clamp01(waterMat->kDeepColor.g);
        mesh.waterTransmittanceB = clamp01(waterMat->kDeepColor.b);
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
