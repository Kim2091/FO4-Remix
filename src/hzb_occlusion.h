// Hierarchical-Z occlusion — self-contained CPU software rasterizer + max-mip
// pyramid + conservative AABB occlusion test. NO engine or Remix dependencies:
// it takes plain float arrays so it can be unit-tested and reasoned about in
// isolation. See memory/fo4-hzb-occlusion-plan.md for how this slots into the
// FO4-Remix smoothness workstream.
//
// EVERYTHING here is in one coordinate space (the plan uses Remix space, since
// camera basis, uploaded verts, world transforms and cached AABBs are all
// already Remix-space). This module never converts; the caller feeds one space
// and reads results in that same space.
//
// THE ONE INVARIANT THAT MATTERS: never report an AABB as occluded unless it is
// confidently, fully behind rasterized occluder surfaces. Every approximation
// in here is biased toward UNDER-culling (reporting "visible" when unsure).
// The three safety levers, all pushing the same direction (harder to cull):
//   1. The mip pyramid reduces by MAX (farthest / empty dominates), so a coarse
//      texel is "solid" only when ALL its finer children are — the occluder is
//      effectively eroded by one test-level texel at every silhouette, and any
//      gap in a coarse texel's footprint defeats a cull over that whole texel.
//      This is where silhouette conservatism lives (coverage itself is kept
//      watertight — an inward coverage bias would open seams; see the .cpp).
//   2. Recorded occluder depth is biased FARTHER (kDepthBias) — absorbs affine-
//      interpolation error so a texel never claims an occluder nearer than real.
//   3. The occludee is tested at its NEAREST point against a caller depthMargin.
// Anything that can't be projected cleanly (crosses the near plane, spills off
// screen) returns "visible", never "occluded".

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HzbOcclusion {

// Camera as the plugin already has it (mirrors the fields of CameraState in
// camera.h — position + orthonormal basis + vertical FOV + aspect + near/far).
struct CameraParams {
    float position[3];
    float forward[3];   // unit, points into the screen
    float up[3];        // unit
    float right[3];     // unit
    float fovYDeg;      // vertical field of view, degrees
    float aspect;       // width / height
    float nearPlane;    // > 0
    float farPlane;     // > nearPlane
};

// One occluder's geometry as mesh-local positions + a mesh->world transform,
// which is exactly what the occluder cache stores (positions-only copy of the
// uploaded verts, keyed like g_meshCache; per-drawable worldTransform). The
// rasterizer applies the transform then the camera projection itself.
//
// positionBase/positionStride let this point straight at an array of
// remixapi_HardcodedVertex (position[3] at offset 0) with
// positionStride = sizeof(remixapi_HardcodedVertex) — no repacking.
struct OccluderMesh {
    const void*     positionBase   = nullptr; // -> first vertex's float[3] position
    size_t          positionStride = 0;       // bytes between consecutive positions
    size_t          vertexCount    = 0;
    const uint32_t* indices        = nullptr;
    size_t          indexCount     = 0;       // multiple of 3
    // Row-major 3x4 mesh-local -> world, same layout as ExtractedMesh /
    // DrawableInstance::worldTransform.
    float           worldTransform[3][4] = {
        {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}
    };
};

// A depth buffer (nearest occluder per texel, empty = far) plus its max-reduced
// mip chain. Build on a worker; read (IsOccluded) after BuildMips completes.
// Not internally synchronized — the integration layer double-buffers and swaps
// a completed Hzb via a generation counter (see the plan). One instance is
// cheap to keep and Reset() each build, avoiding per-frame allocation.
class Hzb {
public:
    // Allocates the mip chain for a base resolution. ~256x144 is the plan's
    // starting point. Safe to call once and reuse across Reset() builds.
    void Init(int baseWidth, int baseHeight);

    // Clears level 0 to "far" and captures the camera for this build. Must be
    // called before rasterizing. Cheap (a memset-equivalent); no realloc.
    void Reset(const CameraParams& cam);

    // Rasterize one occluder into level 0 (depth-only, nearest wins). Triangles
    // that cross the near plane or fall fully off screen are skipped (safe).
    void RasterizeOccluder(const OccluderMesh& occ);

    // Reduce level 0 up the pyramid by MAX. Call once after all occluders.
    void BuildMips();

    // Is this world-space AABB confidently, fully occluded (safe to cull)?
    // Returns false (visible) on any doubt: near-plane crossing, off-screen
    // spill, or a single unoccluded texel in the tested footprint. depthMargin
    // (in normalized-linear-depth units, >= 0) is an extra "must be behind by
    // at least this much" cushion; 0 disables it.
    bool IsOccluded(const float aabbMin[3], const float aabbMax[3],
                    float depthMargin) const;

    // Raw depth at a level's texel (1.0 = empty/far). For the spike's log-only
    // validation pass and unit tests; returns <0 for out-of-range queries.
    float DebugAt(int level, int tx, int ty) const {
        if (level < 0 || level >= Levels()) return -1.0f;
        const auto& L = m_levels[level];
        if (tx < 0 || ty < 0 || tx >= L.w || ty >= L.h) return -2.0f;
        return L.depth[static_cast<size_t>(ty) * L.w + tx];
    }

    int  Width()  const { return m_levels.empty() ? 0 : m_levels[0].w; }
    int  Height() const { return m_levels.empty() ? 0 : m_levels[0].h; }
    int  Levels() const { return static_cast<int>(m_levels.size()); }
    bool Ready()  const { return m_ready; }

private:
    struct Level {
        int w = 0;
        int h = 0;
        std::vector<float> depth; // size w*h, "far" (1.0) = empty
    };

    // Project a world point. Returns false if behind/at the near plane (the
    // caller then bails to the safe "visible" answer). On success fills screen
    // pixel coords (sx,sy) and normalized-linear depth [0,1] (0 near, 1 far).
    bool Project(const float world[3], float& sx, float& sy, float& depth) const;

    std::vector<Level> m_levels; // [0] = full res, each next halved (ceil)
    CameraParams       m_cam = {};
    // Precomputed from m_cam in Reset().
    float m_tanHalfV = 0.0f;
    float m_tanHalfH = 0.0f;
    float m_depthA = 0.0f; // far/(far-near): perspective depth scale (see Project)
    bool  m_ready = false;        // BuildMips completed since last Reset
};

// Rasterizes a single unit quad and tests one AABB behind it (expect occluded)
// and one in front (expect visible), all offset from the given camera. Returns
// true iff both verdicts are correct. Cheap; call it from the spike / a startup
// self-check to prove the projection + reduction + test math before trusting
// any live cull. Writes a one-line human summary to `outSummary` if non-null.
bool SelfTest(std::string* outSummary);

} // namespace HzbOcclusion
