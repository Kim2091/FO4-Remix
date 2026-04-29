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

    // Pull spNormalMap01 from the water material as the diffuse slot. Hash
    // is content-derived inside ExtractMaterialTexture (deterministic over
    // the .dds bytes), so it stays stable across game restarts -- toolkit
    // USD replacements keyed by this hash will keep working session-to-
    // session. Fall back to a synthetic 1x1 blue if the material slot is
    // null (defensive; lets us still pass SubmitDrawable's diffuseHash gate
    // for water without a normal map).
    std::vector<ExtractedTexture> newTextures;
    auto* waterMat = static_cast<BSWaterShaderMaterial*>(state.material);
    if (waterMat) {
        mesh.diffuseTextureHash = BsExtraction::ExtractMaterialTexture(
            waterMat->spNormalMap01, "water_normal", device, newTextures);
    }
    if (mesh.diffuseTextureHash == 0) {
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
        Resolvers::Trace::SetStep(Resolvers::Trace::kSubmitFailed);
        return false;
    }
    Resolvers::Trace::SetStep(Resolvers::Trace::kSubmitOK);

    state.submittedToRemix = true;
    state.meshHash = hash;
    state.textureHashes.insert(kSyntheticDiffuseHash);

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
