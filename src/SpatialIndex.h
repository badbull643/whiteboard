#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

struct WorldRect {
    double x0, y0, x1, y1;
};

// Sparse, hierarchical hash grid over the infinite board.
// Level L has square tiles of kBaseTile * 4^L world units. Each item goes into the
// smallest level where its bounds span at most 2x2 tiles, so a stroke is stored in
// 1-4 tiles no matter how big it is. Only tiles that contain something exist.
// Queries cost O(tiles overlapping the view + items found), independent of board size.
class SpatialIndex {
public:
    static constexpr double kBaseTile = 1024.0;
    static constexpr int kLevels = 24;

    void Insert(uint32_t id, const WorldRect& r);
    void Remove(uint32_t id, const WorldRect& r);
    void Clear();

    // Appends every id whose tiles overlap `view` (each id once, ascending = draw order).
    // `idLimit` must be greater than every inserted id.
    void Query(const WorldRect& view, size_t idLimit, std::vector<uint32_t>& out);

    size_t TileCount() const { return m_tiles.size(); }
    size_t LastTilesVisited() const { return m_lastTilesVisited; }

private:
    struct Key {
        int64_t x, y;
        int32_t level;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && level == o.level; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const
        {
            uint64_t h = static_cast<uint64_t>(k.x) * 0x9E3779B97F4A7C15ull;
            h ^= (static_cast<uint64_t>(k.y) + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2));
            h ^= static_cast<uint64_t>(k.level) * 0xC2B2AE3D27D4EB4Full;
            return static_cast<size_t>(h ^ (h >> 31));
        }
    };

    static int LevelFor(const WorldRect& r);
    static double TileSize(int level);

    std::unordered_map<Key, std::vector<uint32_t>, KeyHash> m_tiles;
    size_t m_tilesPerLevel[kLevels] = {};
    std::vector<uint32_t> m_stamp;  // per id: last query that returned it (dedup)
    uint32_t m_query = 0;
    size_t m_lastTilesVisited = 0;
};
