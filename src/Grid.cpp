#include "Grid.h"
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

void GridSettings::StepPreset(int direction)
{
    if (direction > 0) {
        for (int p : kPresets)
            if (p > size) { size = p; return; }
        size = kMaxSize;
    } else {
        for (int i = static_cast<int>(std::size(kPresets)) - 1; i >= 0; --i)
            if (kPresets[i] < size) { size = kPresets[i]; return; }
        size = kMinSize;
    }
}

void GridSettings::StepFine(int delta)
{
    size = std::clamp(size + delta, kMinSize, kMaxSize);
}

void GridSettings::CycleMode()
{
    mode = mode == GridMode::Lines ? GridMode::Dots : mode == GridMode::Dots ? GridMode::Off : GridMode::Lines;
}

namespace {

// 0 when on-screen spacing p == minPx, 1 when p >= 4 * minPx (log scale -> smooth while zooming).
inline double Ramp(double p, double minPx)
{
    return std::clamp(std::log(p / minPx) / std::log(4.0), 0.0, 1.0);
}

// Tier of grid line i at the base level: 2 = every 16th, 1 = every 4th, 0 = other.
inline int TierOf(int64_t i)
{
    return (i % 16 == 0) ? 2 : (i % 4 == 0) ? 1 : 0;
}

struct Axis {
    std::vector<float> pos;   // snapped screen coordinate
    std::vector<uint8_t> tier;
};

void BuildAxis(Axis& axis, double worldMin, double worldMax, double spacing, double offset, double scale)
{
    axis.pos.clear();
    axis.tier.clear();
    const int64_t first = static_cast<int64_t>(std::floor(worldMin / spacing));
    const int64_t last = static_cast<int64_t>(std::ceil(worldMax / spacing));
    for (int64_t i = first; i <= last; ++i) {
        const double screen = (static_cast<double>(i) * spacing - offset) * scale;
        axis.pos.push_back(static_cast<float>(std::floor(screen)));  // pixel snap -> crisp lines
        axis.tier.push_back(static_cast<uint8_t>(TierOf(i)));
    }
}

}  // namespace

GridStats DrawGrid(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* const brushes[3],
                   const Camera& camera, const GridSettings& settings, D2D1_SIZE_F viewport,
                   unsigned rgb, float alphaScale)
{
    GridStats stats;
    if (settings.mode == GridMode::Off)
        return stats;

    const bool dots = settings.mode == GridMode::Dots;
    const double scale = camera.Scale();
    const double dpi = camera.dpiScale;
    const double minPx = (dots ? 10.0 : 6.0) * dpi;  // never draw denser than this

    // Pick the finest level whose on-screen spacing is at least minPx.
    int level = 0;
    double spacing = settings.size;
    while (spacing * scale < minPx && level < 60) {
        spacing *= 4.0;
        ++level;
    }
    stats.spacing = spacing;

    // Alpha per tier. Tier t has absolute level (level + t). Absolute level 0 (the
    // user's grid size) is always a minor line; levels >= 1 can become major lines.
    // Both parts depend only on on-screen spacing, so the look is continuous while zooming.
    const float minorA = dots ? 0.30f : 0.07f;
    const float majorA = dots ? 0.55f : 0.16f;
    float alpha[3];
    for (int t = 0; t < 3; ++t) {
        const double p = spacing * scale * std::pow(4.0, t);
        double a = minorA * Ramp(p, minPx);
        if (level + t >= 1)
            a += (majorA - minorA) * Ramp(p / 4.0, minPx);
        alpha[t] = static_cast<float>(a) * alphaScale;
        brushes[t]->SetColor(D2D1::ColorF(rgb, alpha[t]));
    }

    static Axis xs, ys;  // reused every frame: no allocations after warm-up
    const Vec2d w0 = camera.offset;
    const Vec2d w1 = camera.ScreenToWorld(viewport.width, viewport.height);
    BuildAxis(xs, w0.x, w1.x, spacing, camera.offset.x, scale);
    BuildAxis(ys, w0.y, w1.y, spacing, camera.offset.y, scale);

    const D2D1_ANTIALIAS_MODE oldMode = dc->GetAntialiasMode();
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    if (!dots) {
        const float w = static_cast<float>(std::max(1.0, std::round(dpi)));
        // Coarse tiers last so major lines sit on top.
        for (int t = 0; t < 3; ++t) {
            if (alpha[t] < 0.004f)
                continue;
            for (size_t i = 0; i < xs.pos.size(); ++i)
                if (xs.tier[i] == t) {
                    dc->FillRectangle({ xs.pos[i], 0.0f, xs.pos[i] + w, viewport.height }, brushes[t]);
                    ++stats.primitives;
                }
            for (size_t i = 0; i < ys.pos.size(); ++i)
                if (ys.tier[i] == t) {
                    dc->FillRectangle({ 0.0f, ys.pos[i], viewport.width, ys.pos[i] + w }, brushes[t]);
                    ++stats.primitives;
                }
        }
    } else {
        // A dot's tier is the coarser grid it belongs to on BOTH axes.
        const float size[3] = {
            static_cast<float>(std::max(1.0, std::round(2.0 * dpi))),
            static_cast<float>(std::max(2.0, std::round(2.5 * dpi))),
            static_cast<float>(std::max(2.0, std::round(3.0 * dpi))),
        };
        for (int t = 0; t < 3; ++t) {
            if (alpha[t] < 0.004f)
                continue;
            const float h = std::floor(size[t] * 0.5f);
            for (size_t iy = 0; iy < ys.pos.size(); ++iy) {
                if (ys.tier[iy] < t)
                    continue;
                const float y = ys.pos[iy] - h;
                for (size_t ix = 0; ix < xs.pos.size(); ++ix) {
                    if (std::min(xs.tier[ix], ys.tier[iy]) != t)
                        continue;
                    const float x = xs.pos[ix] - h;
                    dc->FillRectangle({ x, y, x + size[t], y + size[t] }, brushes[t]);
                    ++stats.primitives;
                }
            }
        }
    }

    dc->SetAntialiasMode(oldMode);
    return stats;
}
