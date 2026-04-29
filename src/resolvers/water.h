#pragma once

#include <cstdint>

struct ID3D11Device;
namespace SemanticCapture { struct DrawableState; }

namespace Resolvers {
namespace Water {

    // Try to resolve a captured-but-not-yet-submitted DrawableState into a
    // Remix mesh submission for water-shader drawables.
    //
    // Submits as plain opaque (no alpha-blend) with a synthetic 1x1 RGBA8
    // blue albedo. The toolkit replaces the submission via USD; the default
    // visual is a solid blue patch suitable only as a hash placeholder.
    bool TryResolve(SemanticCapture::DrawableState& state,
                    uint64_t hash,
                    ID3D11Device* device);

}  // namespace Water
}  // namespace Resolvers
