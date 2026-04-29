#pragma once

#include <cstdint>

struct ID3D11Device;
namespace SemanticCapture { struct DrawableState; }

namespace Resolvers {

    // In-flight resolver trace. Updated at each gate inside TryResolveStatic
    // (and inside RemixRenderer::SubmitDrawable via Trace::SetStep) so an
    // SEH handler in the caller can identify the failing drawable + step
    // when an access violation is caught. Safe to read from any thread
    // (atomic loads under the hood); the values are only meaningful while
    // a resolver call is in flight or just after it crashed.
    namespace Trace {
        // Resolver-level + inside-SubmitDrawable steps. Defined here (not in
        // the .cpp) so remix_renderer.cpp and semantic_capture.cpp can name
        // the inside-SubmitDrawable / gate constants when calling SetStep.
        enum Step : int {
            kIdle              = 0,
            kEntered           = 1,
            kCastOK            = 2,
            kSkinSkipped       = 3,
            kParseStart        = 4,
            kParseOK           = 5,
            kExtentRejected    = 6,
            kBuildMeshOK       = 7,
            kMaterialFetched   = 8,
            kLandscapeSkipped  = 9,
            kTexturesExtracted = 10,
            kSubmitStart       = 11,
            kSubmitOK          = 12,
            kSubmitFailed      = 13,

            // Inside-SubmitDrawable sub-steps. SubmitDrawable writes these
            // via SetStep so the resolver-level SEH catch can pinpoint
            // WHICH api->CreateX call threw.
            kSubmit_BeforeTextureCreate  = 14,
            kSubmit_AfterTextureCreate   = 15,
            kSubmit_BeforeMaterialCreate = 16,
            kSubmit_AfterMaterialCreate  = 17,
            kSubmit_BeforeMeshCreate     = 18,
            kSubmit_AfterMeshCreate      = 19,
            kSubmit_GateInputEmpty       = 20,  // skipped due to empty pixels/vertices
            kSubmit_GateVram             = 21,  // skipped due to VRAM gate (set in semantic_capture)
            kSubmit_GateBudget           = 22,  // skipped due to per-Tick budget

            kLODSkipped                  = 23,  // dropped: NiAVObject::kFlagIsMeshLOD set
            kTopFadeNodeSkipped          = 24,  // dropped: parent1 is kFlagTopFadeNode (LOD-fade group)
            kWorldLODChunkSkipped        = 25,  // dropped: parent chain identifies WRLD LOD chunk
        };

        int LastStep();
        uint64_t LastHash();
        const char* StepName(int s);

        // Setter so SubmitDrawable (and Tick gates) can update the trace
        // without needing a back-channel into the resolver TU.
        void SetStep(int s);
    }

    namespace Lighting {

    // Try to resolve a captured-but-not-yet-submitted DrawableState into a
    // Remix mesh submission for static lit (non-skinned) drawables.
    //
    // - state: the DrawableState produced by the hot-path detour. The
    //          resolver mutates state.submittedToRemix / meshHash /
    //          materialHash / textureHashes on success.
    // - hash:  the PassKey for this drawable (== state.meshHash on submit)
    // - device: D3D11 device used for texture readback
    //
    // Returns true on submission success. False on cast failure, parse
    // failure, missing diffuse, or any other condition that should retry
    // next frame (or never; the caller's TTL handles permanent failures).
    bool TryResolveStatic(SemanticCapture::DrawableState& state,
                          uint64_t hash,
                          ID3D11Device* device);

    }  // namespace Lighting
}  // namespace Resolvers
