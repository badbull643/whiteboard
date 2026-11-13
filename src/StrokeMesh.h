#pragma once
#include <d2d1.h>
#include <cstdint>
#include <vector>

// One vertex of a stroke's GPU triangle strip (20 bytes).
// Position is in world units relative to the stroke origin; the vertex shader pushes it
// out by `dir * (half width + 1px AA fringe)` in screen space, so line width and the
// minimum on-screen width are applied on the GPU without re-tessellating.
struct StrokeVertex {
    float x, y;       // centerline point (stroke-local world units)
    float dx, dy;     // offset direction (unit normal, miter-scaled; caps add the tangent)
    uint32_t packed;  // bits 0-23 stroke id, bit 24 side (+1/-1), bit 25 cap
};

constexpr uint32_t kLiveStrokeId = 0xFFFFFF;  // vertex shader reads this stroke from the constant buffer
constexpr uint32_t kMaxStrokeId = 0xFFFFFE;

// Appends a triangle strip for the stroke: Catmull-Rom smoothing (same curve as before),
// flattened adaptively, round caps (exact, done per pixel) and round joins at sharp turns.
void AppendStrokeStrip(const D2D1_POINT_2F* pts, size_t count, float width, uint32_t id,
                       std::vector<StrokeVertex>& out, std::vector<D2D1_POINT_2F>& scratch);
