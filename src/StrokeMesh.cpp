#include "StrokeMesh.h"
#include <algorithm>
#include <cmath>

namespace {

struct V2 {
    float x, y;
};
inline V2 operator+(V2 a, V2 b) { return { a.x + b.x, a.y + b.y }; }
inline V2 operator-(V2 a, V2 b) { return { a.x - b.x, a.y - b.y }; }
inline V2 operator*(V2 a, float k) { return { a.x * k, a.y * k }; }
inline float Dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
inline float Len(V2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
inline V2 Normalize(V2 a, V2 fallback)
{
    const float l = Len(a);
    return l > 1e-12f ? a * (1.0f / l) : fallback;
}
inline V2 Perp(V2 t) { return { -t.y, t.x }; }
inline V2 ClampLen(V2 v, float maxLen)
{
    const float l = Len(v);
    return (l > maxLen && l > 0.0f) ? v * (maxLen / l) : v;
}
inline V2 ToV2(D2D1_POINT_2F p) { return { p.x, p.y }; }

constexpr float kPi = 3.14159265f;
constexpr float kRoundJoinMinAngle = 25.0f * kPi / 180.0f;  // sharper turns get a round join
constexpr float kRoundJoinStep = 18.0f * kPi / 180.0f;

// Smooth the input points (Catmull-Rom -> cubic Bezier, tangents clamped against overshoot)
// and flatten to a polyline with max deviation `tolerance`.
void Flatten(const D2D1_POINT_2F* p, size_t n, float tolerance, std::vector<D2D1_POINT_2F>& out)
{
    out.clear();
    out.push_back(p[0]);
    for (size_t i = 0; i + 1 < n; ++i) {
        const V2 p0 = ToV2(p[i > 0 ? i - 1 : 0]);
        const V2 p1 = ToV2(p[i]);
        const V2 p2 = ToV2(p[i + 1]);
        const V2 p3 = ToV2(p[std::min(i + 2, n - 1)]);
        const float maxHandle = Len(p2 - p1) * 0.4f;
        const V2 c1 = p1 + ClampLen((p2 - p0) * (1.0f / 6.0f), maxHandle);
        const V2 c2 = p2 - ClampLen((p3 - p1) * (1.0f / 6.0f), maxHandle);

        // Subdivisions needed so the chords stay within `tolerance` of the curve.
        const float dd = std::max(Len(p1 - c1 * 2.0f + c2), Len(c1 - c2 * 2.0f + p2));
        const int steps = std::clamp(static_cast<int>(std::ceil(std::sqrt(dd * 0.75f / tolerance))), 1, 64);
        for (int k = 1; k <= steps; ++k) {
            const float t = float(k) / float(steps), u = 1.0f - t;
            const V2 q = p1 * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + p2 * (t * t * t);
            const D2D1_POINT_2F& last = out.back();
            if (std::fabs(q.x - last.x) + std::fabs(q.y - last.y) > 1e-5f)
                out.push_back({ q.x, q.y });
        }
    }
}

struct StripWriter {
    std::vector<StrokeVertex>& out;
    uint32_t id;

    void Pair(V2 pos, V2 dirPlus, V2 dirMinus, bool cap)
    {
        const uint32_t capBit = cap ? (1u << 25) : 0u;
        out.push_back({ pos.x, pos.y, dirMinus.x, dirMinus.y, id | capBit });
        out.push_back({ pos.x, pos.y, dirPlus.x, dirPlus.y, id | (1u << 24) | capBit });
    }
    void Body(V2 pos, V2 n, float k = 1.0f) { Pair(pos, n * k, n * -k, false); }
};

}  // namespace

void AppendStrokeStrip(const D2D1_POINT_2F* pts, size_t count, float width, uint32_t id,
                       std::vector<StrokeVertex>& out, std::vector<D2D1_POINT_2F>& poly)
{
    if (count == 0)
        return;
    // Flatten finely relative to the line width: stays smooth when zoomed in ~10x.
    const float tolerance = std::max(width * 0.02f, 1e-4f);
    Flatten(pts, count, tolerance, poly);
    const size_t n = poly.size();
    StripWriter w{ out, id };

    if (n == 1) {  // a dot: zero-length body with two round caps
        const V2 p = ToV2(poly[0]);
        const V2 t{ 1.0f, 0.0f }, nrm{ 0.0f, 1.0f };
        w.Pair(p, nrm - t, nrm * -1.0f - t, true);
        w.Pair(p, nrm + t, nrm * -1.0f + t, true);
        return;
    }

    // Start cap: extend half a width backwards; the pixel shader rounds it exactly.
    V2 t = Normalize(ToV2(poly[1]) - ToV2(poly[0]), { 1.0f, 0.0f });
    V2 nrm = Perp(t);
    w.Pair(ToV2(poly[0]), nrm - t, nrm * -1.0f - t, true);
    w.Body(ToV2(poly[0]), nrm);

    for (size_t i = 1; i + 1 < n; ++i) {
        const V2 p = ToV2(poly[i]);
        const V2 tNext = Normalize(ToV2(poly[i + 1]) - p, t);
        const V2 nNext = Perp(tNext);
        const float c = std::clamp(Dot(t, tNext), -1.0f, 1.0f);
        const float angle = std::acos(c);
        if (angle < kRoundJoinMinAngle) {
            // Gentle bend: one miter pair (scale stays close to 1 at these angles).
            const V2 m = Normalize(nrm + nNext, nNext);
            w.Body(p, m, 1.0f / std::max(Dot(m, nNext), 0.5f));
        } else {
            // Sharp turn: fan of pairs rotating from the old to the new normal.
            const int steps = static_cast<int>(std::ceil(angle / kRoundJoinStep));
            const float sign = (t.x * tNext.y - t.y * tNext.x) >= 0.0f ? 1.0f : -1.0f;
            for (int k = 0; k <= steps; ++k) {
                const float a = sign * angle * float(k) / float(steps);
                const float ca = std::cos(a), sa = std::sin(a);
                w.Body(p, { nrm.x * ca - nrm.y * sa, nrm.x * sa + nrm.y * ca });
            }
        }
        t = tNext;
        nrm = nNext;
    }

    // End cap.
    const V2 pe = ToV2(poly[n - 1]);
    w.Body(pe, nrm);
    w.Pair(pe, nrm + t, nrm * -1.0f + t, true);
}
