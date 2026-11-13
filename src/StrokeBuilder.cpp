#include "StrokeBuilder.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// Input points closer than this (screen px) to the previous kept point are not stored.
// Removes the 1-px staircase of mouse input; the tail point keeps the ink at the cursor.
constexpr double kMinPointDistancePx = 2.0;

// Ramer-Douglas-Peucker tolerance on release (screen px at the zoom the stroke was drawn).
constexpr double kSimplifyTolerancePx = 0.6;

double SegmentDistanceSq(D2D1_POINT_2F p, D2D1_POINT_2F a, D2D1_POINT_2F b)
{
    const double abx = double(b.x) - a.x, aby = double(b.y) - a.y;
    const double apx = double(p.x) - a.x, apy = double(p.y) - a.y;
    const double len2 = abx * abx + aby * aby;
    double t = len2 > 0.0 ? (apx * abx + apy * aby) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double dx = apx - t * abx, dy = apy - t * aby;
    return dx * dx + dy * dy;
}

// Iterative RDP (no recursion, so very long strokes can't blow the stack).
void Simplify(std::vector<D2D1_POINT_2F>& pts, double tolerance)
{
    const size_t n = pts.size();
    if (n < 3)
        return;
    std::vector<uint8_t> keep(n, 0);
    keep[0] = keep[n - 1] = 1;
    std::vector<std::pair<size_t, size_t>> stack{ { 0, n - 1 } };
    const double tol2 = tolerance * tolerance;
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        double maxD = -1.0;
        size_t idx = a;
        for (size_t i = a + 1; i < b; ++i) {
            const double d = SegmentDistanceSq(pts[i], pts[a], pts[b]);
            if (d > maxD) { maxD = d; idx = i; }
        }
        if (maxD > tol2) {
            keep[idx] = 1;
            if (idx - a > 1) stack.push_back({ a, idx });
            if (b - idx > 1) stack.push_back({ idx, b });
        }
    }
    size_t w = 0;
    for (size_t i = 0; i < n; ++i)
        if (keep[i])
            pts[w++] = pts[i];
    pts.resize(w);
}

}  // namespace

void Stroke::ComputeBounds()
{
    float x0 = std::numeric_limits<float>::max(), y0 = x0;
    float x1 = -x0, y1 = -x0;
    for (const auto& p : points) {
        x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
    }
    const float h = width * 0.5f + 1.0f;  // +1: smoothing can bulge slightly past the points
    bounds = { x0 - h, y0 - h, x1 + h, y1 + h };
}

void StrokeBuilder::Begin(Vec2d world, double pxPerUnit, float widthPx, uint32_t rgb, bool autoInk)
{
    m_live = Stroke{};
    m_live.origin = world;
    m_live.points.reserve(512);
    m_live.points.push_back({ 0.0f, 0.0f });
    m_raw.clear();
    m_raw.push_back({ 0.0f, 0.0f });
    m_live.width = static_cast<float>(widthPx / pxPerUnit);
    m_live.rgb = rgb;
    m_live.autoInk = autoInk;
    m_scale = pxPerUnit;
    m_hasTail = false;
    m_active = true;
    ++m_id;
}

void StrokeBuilder::Move(Vec2d world)
{
    if (!m_active)
        return;
    const D2D1_POINT_2F p{ static_cast<float>(world.x - m_live.origin.x),
                           static_cast<float>(world.y - m_live.origin.y) };
    const D2D1_POINT_2F last = m_raw.back();
    const double dx = (double(p.x) - last.x) * m_scale, dy = (double(p.y) - last.y) * m_scale;
    if (dx * dx + dy * dy >= kMinPointDistancePx * kMinPointDistancePx) {
        m_raw.push_back(p);
        m_live.points.push_back(p);
        const size_t n = m_raw.size();
        // Point n-3 now has both neighbors on each side: final. n-2 gets a provisional value.
        for (size_t i = (n >= 3 ? n - 3 : 1); i + 1 < n; ++i)
            m_live.points[i] = Smoothed(i);
        m_hasTail = false;
    } else {
        m_tail = p;
        m_hasTail = true;
    }
}

D2D1_POINT_2F StrokeBuilder::Smoothed(size_t i) const
{
    // 5-tap binomial filter [1 4 6 4 1] / 16 over raw input. Removes the 1-px quantization
    // wobble of mouse input; endpoints are kept exact so the stroke starts/ends at the cursor.
    const size_t n = m_raw.size();
    if (i == 0 || i + 1 >= n)
        return m_raw[i];
    auto at = [&](ptrdiff_t k) { return m_raw[static_cast<size_t>(std::clamp<ptrdiff_t>(k, 0, n - 1))]; };
    const ptrdiff_t c = static_cast<ptrdiff_t>(i);
    static constexpr float w[5] = { 1.0f / 16, 4.0f / 16, 6.0f / 16, 4.0f / 16, 1.0f / 16 };
    D2D1_POINT_2F r{ 0.0f, 0.0f };
    for (int k = -2; k <= 2; ++k) {
        const D2D1_POINT_2F q = at(c + k);
        r.x += w[k + 2] * q.x;
        r.y += w[k + 2] * q.y;
    }
    return r;
}

size_t StrokeBuilder::FinalCount() const
{
    const size_t n = m_live.points.size();
    return n >= 3 ? n - 2 : 1;
}

Stroke StrokeBuilder::Finish()
{
    m_active = false;
    if (m_hasTail) {
        m_raw.push_back(m_tail);
        m_live.points.push_back(m_tail);
    }
    m_hasTail = false;
    const size_t n = m_raw.size();
    for (size_t i = (n >= 3 ? n - 3 : 1); i < n; ++i)
        m_live.points[i] = Smoothed(i);
    Simplify(m_live.points, kSimplifyTolerancePx / m_scale);
    m_live.points.shrink_to_fit();
    m_live.ComputeBounds();
    return std::move(m_live);
}

void StrokeBuilder::Cancel()
{
    m_active = false;
    m_hasTail = false;
    m_live = Stroke{};
    m_raw.clear();
}
