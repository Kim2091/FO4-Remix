#include "hzb_occlusion.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace HzbOcclusion {

namespace {

constexpr float kFarDepth = 1.0f; // empty texel: "occluder infinitely far"

// kDepthBias pushes every recorded occluder depth farther by this much of the
// [0,1] range so a texel never claims an occluder nearer than it truly is. With
// the perspective depth (see Project) the barycentric interpolation of depth is
// exact, so this only needs to cover float rounding -- kept small; real z-fight
// slack between a coplanar occluder/occludee is the caller's depthMargin's job.
// Silhouette conservatism is handled structurally by the max-reduced pyramid
// (see BuildMips), NOT by shrinking coverage -- an inward coverage bias was
// tried and removed because it opened a one-texel EMPTY seam along every shared
// triangle edge (pixel centers exactly on the edge got excluded from both
// triangles), which the max pyramid then propagated up to poison coarse test
// texels. Coverage must be watertight; conservatism lives in the reduction.
constexpr float kDepthBias = 1.0f / 8192.0f;

inline float Dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// mesh-local point -> world via a row-major 3x4 transform.
inline void XformPoint(const float m[3][4], const float p[3], float out[3]) {
    for (int r = 0; r < 3; ++r) {
        out[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3];
    }
}

} // namespace

void Hzb::Init(int baseWidth, int baseHeight) {
    baseWidth  = (std::max)(baseWidth, 1);
    baseHeight = (std::max)(baseHeight, 1);
    m_levels.clear();
    int w = baseWidth, h = baseHeight;
    // Chain down to 1x1 so any occludee footprint resolves to a small texel set
    // at some level.
    for (;;) {
        Level lvl;
        lvl.w = w;
        lvl.h = h;
        lvl.depth.assign(static_cast<size_t>(w) * h, kFarDepth);
        m_levels.push_back(std::move(lvl));
        if (w == 1 && h == 1) break;
        w = (std::max)(1, (w + 1) / 2);
        h = (std::max)(1, (h + 1) / 2);
    }
    m_ready = false;
}

void Hzb::Reset(const CameraParams& cam) {
    m_cam = cam;
    const float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float halfV = 0.5f * (std::max)(cam.fovYDeg, 1.0f) * kDegToRad;
    m_tanHalfV = std::tan(halfV);
    const float aspect = (cam.aspect > 0.01f) ? cam.aspect : (16.0f / 9.0f);
    m_tanHalfH = m_tanHalfV * aspect;
    const float range = (std::max)(cam.farPlane - cam.nearPlane, 1e-3f);
    m_depthA = cam.farPlane / range;
    if (!m_levels.empty()) {
        std::fill(m_levels[0].depth.begin(), m_levels[0].depth.end(), kFarDepth);
    }
    m_ready = false;
}

bool Hzb::Project(const float world[3], float& sx, float& sy,
                  float& depth) const {
    float d[3] = {
        world[0] - m_cam.position[0],
        world[1] - m_cam.position[1],
        world[2] - m_cam.position[2],
    };
    const float vz = Dot3(d, m_cam.forward); // view-space distance ahead
    if (vz <= m_cam.nearPlane) return false; // behind / on near plane -> bail
    const float vx = Dot3(d, m_cam.right);
    const float vy = Dot3(d, m_cam.up);
    // Perspective divide; ndc in [-1,1] across the frustum.
    const float ndcX = vx / (vz * m_tanHalfH);
    const float ndcY = vy / (vz * m_tanHalfV);
    const Level& L0 = m_levels[0];
    sx = (ndcX * 0.5f + 0.5f) * L0.w;
    sy = (1.0f - (ndcY * 0.5f + 0.5f)) * L0.h; // flip: +y up -> +row down
    // Perspective depth, near-weighted precision: d = A*(1 - near/vz) in [0,1],
    // A = far/(far-near). LINEAR depth was tried first and FAILED in-game --
    // FO4's far plane is up to 1e7, so linear depth had almost no resolution in
    // the near range where occlusion happens, and the fixed depth bias/margin
    // became hundreds of world units of required clearance (nothing close behind
    // a wall ever culled). Bonus: d is affine in 1/vz, which is itself linear in
    // screen space, so barycentric interpolation of d is EXACT -- no
    // perspective correction, and the depth-bias no longer fights interp error.
    depth = m_depthA * (1.0f - m_cam.nearPlane / vz);
    depth = (std::min)((std::max)(depth, 0.0f), 1.0f);
    return true;
}

void Hzb::RasterizeOccluder(const OccluderMesh& occ) {
    if (m_levels.empty() || !occ.positionBase || !occ.indices) return;
    if (occ.positionStride < sizeof(float) * 3) return;

    Level& L0 = m_levels[0];
    const auto* base = static_cast<const uint8_t*>(occ.positionBase);

    auto readLocal = [&](uint32_t vi, float out[3]) {
        std::memcpy(out, base + static_cast<size_t>(vi) * occ.positionStride,
                    sizeof(float) * 3);
    };

    const size_t triCount = occ.indexCount / 3;
    for (size_t t = 0; t < triCount; ++t) {
        const uint32_t i0 = occ.indices[t * 3 + 0];
        const uint32_t i1 = occ.indices[t * 3 + 1];
        const uint32_t i2 = occ.indices[t * 3 + 2];
        if (i0 >= occ.vertexCount || i1 >= occ.vertexCount ||
            i2 >= occ.vertexCount) {
            continue;
        }
        float lp[3][3], wp[3][3];
        readLocal(i0, lp[0]);
        readLocal(i1, lp[1]);
        readLocal(i2, lp[2]);
        XformPoint(occ.worldTransform, lp[0], wp[0]);
        XformPoint(occ.worldTransform, lp[1], wp[1]);
        XformPoint(occ.worldTransform, lp[2], wp[2]);

        float sx[3], sy[3], sd[3];
        bool ok = true;
        for (int v = 0; v < 3; ++v) {
            if (!Project(wp[v], sx[v], sy[v], sd[v])) { ok = false; break; }
        }
        // Any vertex behind the near plane -> skip the whole triangle. Losing an
        // occluder only under-culls, which is safe.
        if (!ok) continue;

        // Screen-space bounding box, clamped to the buffer.
        float fMinX = (std::min)({sx[0], sx[1], sx[2]});
        float fMaxX = (std::max)({sx[0], sx[1], sx[2]});
        float fMinY = (std::min)({sy[0], sy[1], sy[2]});
        float fMaxY = (std::max)({sy[0], sy[1], sy[2]});
        int minX = (std::max)(0, static_cast<int>(std::floor(fMinX)));
        int maxX = (std::min)(L0.w - 1, static_cast<int>(std::ceil(fMaxX)));
        int minY = (std::max)(0, static_cast<int>(std::floor(fMinY)));
        int maxY = (std::min)(L0.h - 1, static_cast<int>(std::ceil(fMaxY)));
        if (minX > maxX || minY > maxY) continue;

        // Edge functions on screen coords. area2's sign is the winding; we
        // normalize by it so "inside" works for either winding (we deliberately
        // do NOT backface-cull — a single-sided wall must occlude whichever way
        // it faces, and for a solid body the nearer face wins the min anyway).
        auto edge = [](float ax, float ay, float bx, float by, float px,
                       float py) {
            return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
        };
        const float area2 = edge(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]);
        if (std::fabs(area2) < 1e-6f) continue; // degenerate
        const float sign = (area2 < 0.0f) ? -1.0f : 1.0f;
        const float invArea = 1.0f / area2; // barycentric normalizer (signed)

        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const float px = x + 0.5f;
                const float py = y + 0.5f;
                float e0 = sign * edge(sx[1], sy[1], sx[2], sy[2], px, py);
                float e1 = sign * edge(sx[2], sy[2], sx[0], sy[0], px, py);
                float e2 = sign * edge(sx[0], sy[0], sx[1], sy[1], px, py);
                // Watertight inclusive coverage: a center exactly on a shared
                // edge is covered by BOTH adjacent triangles (they write the
                // same depth), leaving no seam. Because the two triangles'
                // shared-edge functions are exact negations, an off-edge center
                // is claimed by exactly one — no double-gap, no double-miss.
                if (e0 < 0.0f || e1 < 0.0f || e2 < 0.0f) continue;
                // Affine barycentric depth. Perspective-correct 1/z interp is
                // the documented refinement; kDepthBias covers the error.
                const float b0 = e0 * invArea * sign;
                const float b1 = e1 * invArea * sign;
                const float b2 = e2 * invArea * sign;
                float depth = b0 * sd[0] + b1 * sd[1] + b2 * sd[2] + kDepthBias;
                float& cell = L0.depth[static_cast<size_t>(y) * L0.w + x];
                if (depth < cell) cell = depth; // nearest occluder wins
            }
        }
    }
}

void Hzb::BuildMips() {
    for (size_t l = 1; l < m_levels.size(); ++l) {
        const Level& src = m_levels[l - 1];
        Level& dst = m_levels[l];
        for (int y = 0; y < dst.h; ++y) {
            for (int x = 0; x < dst.w; ++x) {
                // MAX over the (up to) 2x2 source block: the FARTHEST nearest-
                // occluder, so an empty/gap child (kFarDepth) dominates and
                // defeats any cull over this coarse texel.
                float m = 0.0f;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        int sxc = (std::min)(x * 2 + dx, src.w - 1);
                        int syc = (std::min)(y * 2 + dy, src.h - 1);
                        float s = src.depth[static_cast<size_t>(syc) * src.w + sxc];
                        if (s > m) m = s;
                    }
                }
                dst.depth[static_cast<size_t>(y) * dst.w + x] = m;
            }
        }
    }
    m_ready = true;
}

bool Hzb::IsOccluded(const float aabbMin[3], const float aabbMax[3],
                     float depthMargin) const {
    if (!m_ready || m_levels.empty()) return false;
    const Level& L0 = m_levels[0];

    // Project all 8 corners. NEAREST depth is the occludee's strongest claim to
    // be visible, so we test that. Any corner that crosses the near plane or
    // lands off screen -> we can't prove full occlusion -> report visible.
    float minSx = 1e30f, minSy = 1e30f, maxSx = -1e30f, maxSy = -1e30f;
    float nearest = 1.0f;
    for (int c = 0; c < 8; ++c) {
        const float corner[3] = {
            (c & 1) ? aabbMax[0] : aabbMin[0],
            (c & 2) ? aabbMax[1] : aabbMin[1],
            (c & 4) ? aabbMax[2] : aabbMin[2],
        };
        float sx, sy, depth;
        if (!Project(corner, sx, sy, depth)) return false; // near-plane spill
        minSx = (std::min)(minSx, sx);
        maxSx = (std::max)(maxSx, sx);
        minSy = (std::min)(minSy, sy);
        maxSy = (std::max)(maxSy, sy);
        nearest = (std::min)(nearest, depth);
    }
    // Off-screen spill: the parts beyond the buffer edge have no occluder data,
    // so full occlusion is unprovable -> visible. (Such geometry is near/large
    // and the keep-radius owns it anyway.)
    if (minSx < 0.0f || minSy < 0.0f ||
        maxSx > static_cast<float>(L0.w) || maxSy > static_cast<float>(L0.h)) {
        return false;
    }

    // Pick the coarsest level where the footprint spans <= 2 texels per axis, so
    // the test reads a tiny (<= 2x2) neighborhood.
    const float spanX = maxSx - minSx;
    const float spanY = maxSy - minSy;
    const float span = (std::max)((std::max)(spanX, spanY), 1.0f);
    int level = static_cast<int>(std::ceil(std::log2(span)));
    level = (std::min)((std::max)(level, 0), Levels() - 1);
    const Level& L = m_levels[level];
    const int shift = level;

    int tx0 = static_cast<int>(std::floor(minSx)) >> shift;
    int tx1 = static_cast<int>(std::floor(maxSx)) >> shift;
    int ty0 = static_cast<int>(std::floor(minSy)) >> shift;
    int ty1 = static_cast<int>(std::floor(maxSy)) >> shift;
    tx0 = (std::max)(0, tx0); ty0 = (std::max)(0, ty0);
    tx1 = (std::min)(L.w - 1, tx1); ty1 = (std::min)(L.h - 1, ty1);

    // MAX over the covered texels = the farthest nearest-occluder in the whole
    // footprint. Cull only if the occludee's nearest point is behind even that
    // (plus the caller's margin) — i.e. behind an occluder at every texel.
    float regionFar = 0.0f;
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            float d = L.depth[static_cast<size_t>(ty) * L.w + tx];
            if (d > regionFar) regionFar = d;
        }
    }
    return nearest > regionFar + depthMargin;
}

// --- self-test ---------------------------------------------------------------

bool SelfTest(std::string* outSummary) {
    // Camera at origin looking down +Z (Remix-agnostic; any consistent basis).
    CameraParams cam = {};
    cam.position[0] = 0; cam.position[1] = 0; cam.position[2] = 0;
    cam.forward[0] = 0; cam.forward[1] = 0; cam.forward[2] = 1;
    cam.up[0] = 0; cam.up[1] = 1; cam.up[2] = 0;
    cam.right[0] = 1; cam.right[1] = 0; cam.right[2] = 0;
    cam.fovYDeg = 90.0f;
    cam.aspect = 1.0f;
    cam.nearPlane = 1.0f;
    cam.farPlane = 1000.0f;

    // A big wall at z=100, spanning well beyond the frustum at that distance.
    const float z = 100.0f;
    const float e = 500.0f;
    struct V { float p[3]; };
    V verts[4] = {
        {{-e, -e, z}}, {{ e, -e, z}}, {{ e,  e, z}}, {{-e,  e, z}},
    };
    uint32_t idx[6] = {0, 1, 2, 0, 2, 3};

    Hzb hzb;
    hzb.Init(256, 256);
    hzb.Reset(cam);
    OccluderMesh occ;
    occ.positionBase = verts;
    occ.positionStride = sizeof(V);
    occ.vertexCount = 4;
    occ.indices = idx;
    occ.indexCount = 6;
    hzb.RasterizeOccluder(occ);
    hzb.BuildMips();

    // Behind the wall, small, centered -> expect OCCLUDED.
    float behindMin[3] = {-5, -5, 200};
    float behindMax[3] = { 5,  5, 210};
    const bool behindOccluded = hzb.IsOccluded(behindMin, behindMax, 0.0f);

    // In front of the wall, same footprint -> expect VISIBLE.
    float frontMin[3] = {-5, -5, 40};
    float frontMax[3] = { 5,  5, 50};
    const bool frontOccluded = hzb.IsOccluded(frontMin, frontMax, 0.0f);

    const bool pass = behindOccluded && !frontOccluded;
    if (outSummary) {
        *outSummary = std::string("HZB self-test ") + (pass ? "PASS" : "FAIL") +
                      " (behindOccluded=" + (behindOccluded ? "1" : "0") +
                      " expect 1, frontOccluded=" + (frontOccluded ? "1" : "0") +
                      " expect 0)";
    }
    return pass;
}

} // namespace HzbOcclusion
