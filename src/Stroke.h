#pragma once
#include <d2d1.h>
#include <cstdint>
#include <vector>
#include "Camera.h"

// One pen stroke. Points are floats relative to a double-precision origin, so a
// stroke is exact anywhere on the infinite board while costing 8 bytes per point.
struct Stroke {
    Vec2d origin;                        // world position of the first point
    std::vector<D2D1_POINT_2F> points;   // world units, relative to origin
    float width = 2.0f;                  // world units
    uint32_t rgb = 0;                    // 0xRRGGBB, used when autoInk is false
    bool autoInk = true;                 // follows the theme (dark ink on light, light ink on dark)
    D2D1_RECT_F bounds{};                // local bounds including half the width
    bool alive = true;                   // false = undone (later: erased)

    // GPU mesh location (owned by the Renderer)
    uint32_t gpuFirst = 0;
    uint32_t gpuCount = 0;

    void ComputeBounds();
};
