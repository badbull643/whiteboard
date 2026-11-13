#include "Stress.h"
#include <cmath>
#include <random>

void AddStressStrokes(Board& board, Vec2d center, double halfExtent, size_t targetPoints, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> pos(-halfExtent, halfExtent);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::normal_distribution<float> turn(0.0f, 0.12f);

    size_t added = 0;
    std::vector<Stroke> batch;
    while (added < targetPoints) {
        Stroke s;
        s.origin = { center.x + pos(rng), center.y + pos(rng) };
        s.width = 1.5f + 3.0f * unit(rng);
        s.autoInk = true;

        // A wandering, curling line like handwriting: 40-260 points, ~3 units apart.
        const int n = 40 + static_cast<int>(unit(rng) * 220.0f);
        const float step = 2.0f + 2.0f * unit(rng);
        float heading = unit(rng) * 6.2831853f;
        float curl = (unit(rng) - 0.5f) * 0.2f;
        D2D1_POINT_2F p{ 0.0f, 0.0f };
        s.points.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            s.points.push_back(p);
            curl = curl * 0.97f + turn(rng) * 0.3f;
            heading += curl;
            p.x += std::cos(heading) * step;
            p.y += std::sin(heading) * step;
        }
        s.ComputeBounds();
        added += s.points.size();
        batch.push_back(std::move(s));
    }
    board.AddMany(std::move(batch));  // a single Ctrl+Z removes the whole test
}
