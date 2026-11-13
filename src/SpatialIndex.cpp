#include "SpatialIndex.h"
#include <algorithm>
#include <cmath>

double SpatialIndex::TileSize(int level)
{
    return kBaseTile * std::ldexp(1.0, 2 * level);  // 1024 * 4^level
}

int SpatialIndex::LevelFor(const WorldRect& r)
{
    const double size = std::max(r.x1 - r.x0, r.y1 - r.y0);
    int level = 0;
    double tile = kBaseTile;
    while (tile < size && level < kLevels - 1) {
        tile *= 4.0;
        ++level;
    }
    return level;
}

void SpatialIndex::Insert(uint32_t id, const WorldRect& r)
{
    const int level = LevelFor(r);
    const double t = TileSize(level);
    const int64_t tx0 = static_cast<int64_t>(std::floor(r.x0 / t)), tx1 = static_cast<int64_t>(std::floor(r.x1 / t));
    const int64_t ty0 = static_cast<int64_t>(std::floor(r.y0 / t)), ty1 = static_cast<int64_t>(std::floor(r.y1 / t));
    for (int64_t ty = ty0; ty <= ty1; ++ty)
        for (int64_t tx = tx0; tx <= tx1; ++tx) {
            auto [it, inserted] = m_tiles.try_emplace(Key{ tx, ty, level });
            if (inserted)
                ++m_tilesPerLevel[level];
            it->second.push_back(id);
        }
}

void SpatialIndex::Remove(uint32_t id, const WorldRect& r)
{
    const int level = LevelFor(r);
    const double t = TileSize(level);
    const int64_t tx0 = static_cast<int64_t>(std::floor(r.x0 / t)), tx1 = static_cast<int64_t>(std::floor(r.x1 / t));
    const int64_t ty0 = static_cast<int64_t>(std::floor(r.y0 / t)), ty1 = static_cast<int64_t>(std::floor(r.y1 / t));
    for (int64_t ty = ty0; ty <= ty1; ++ty)
        for (int64_t tx = tx0; tx <= tx1; ++tx) {
            auto it = m_tiles.find(Key{ tx, ty, level });
            if (it == m_tiles.end())
                continue;
            auto& ids = it->second;
            // Newest ids are at the back (undo removes the newest stroke): search backwards.
            auto pos = std::find(ids.rbegin(), ids.rend(), id);
            if (pos != ids.rend())
                ids.erase(std::next(pos).base());
            if (ids.empty()) {
                m_tiles.erase(it);
                --m_tilesPerLevel[level];
            }
        }
}

void SpatialIndex::Clear()
{
    m_tiles.clear();
    std::fill(std::begin(m_tilesPerLevel), std::end(m_tilesPerLevel), size_t{ 0 });
    m_stamp.clear();
}

void SpatialIndex::Query(const WorldRect& view, size_t idLimit, std::vector<uint32_t>& out)
{
    out.clear();
    m_lastTilesVisited = 0;
    if (m_stamp.size() < idLimit)
        m_stamp.resize(idLimit, 0);
    if (++m_query == 0) {  // wrapped: reset stamps
        std::fill(m_stamp.begin(), m_stamp.end(), 0u);
        m_query = 1;
    }

    auto collect = [&](const std::vector<uint32_t>& ids) {
        ++m_lastTilesVisited;
        for (uint32_t id : ids)
            if (m_stamp[id] != m_query) {
                m_stamp[id] = m_query;
                out.push_back(id);
            }
    };

    for (int level = 0; level < kLevels; ++level) {
        if (m_tilesPerLevel[level] == 0)
            continue;
        const double t = TileSize(level);
        const int64_t tx0 = static_cast<int64_t>(std::floor(view.x0 / t)), tx1 = static_cast<int64_t>(std::floor(view.x1 / t));
        const int64_t ty0 = static_cast<int64_t>(std::floor(view.y0 / t)), ty1 = static_cast<int64_t>(std::floor(view.y1 / t));
        const double rangeTiles = double(tx1 - tx0 + 1) * double(ty1 - ty0 + 1);

        if (rangeTiles <= double(m_tilesPerLevel[level])) {
            // View covers few tiles: look each one up.
            for (int64_t ty = ty0; ty <= ty1; ++ty)
                for (int64_t tx = tx0; tx <= tx1; ++tx) {
                    auto it = m_tiles.find(Key{ tx, ty, level });
                    if (it != m_tiles.end())
                        collect(it->second);
                }
        } else {
            // Zoomed far out: fewer existing tiles than tiles in view, so scan the existing ones.
            for (const auto& [key, ids] : m_tiles)
                if (key.level == level && key.x >= tx0 && key.x <= tx1 && key.y >= ty0 && key.y <= ty1)
                    collect(ids);
        }
    }
    std::sort(out.begin(), out.end());  // stroke order = z-order
}
