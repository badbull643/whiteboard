#pragma once
#include <d2d1_1.h>
#include "Camera.h"

enum class GridMode { Lines, Dots, Off };

struct GridSettings {
    static constexpr int kMinSize = 4;
    static constexpr int kMaxSize = 512;
    static constexpr int kPresets[] = { 4, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256 };

    GridMode mode = GridMode::Lines;
    int size = 32;  // cell size in world units (DIPs at 100% zoom)

    void StepPreset(int direction);  // +1 = next larger preset, -1 = next smaller
    void StepFine(int delta);        // +-1 world unit, for custom sizes
    void CycleMode();
};

struct GridStats {
    int primitives = 0;    // lines or dots drawn
    double spacing = 0.0;  // world spacing of the finest visible level
};

// Draws only the visible part of the grid, in screen space.
// When cells get too small on screen the grid steps to 4x coarser levels, with
// alpha fading tied to on-screen spacing so zooming never pops.
// brushes: 3 brushes for the fine / every-4th / every-16th tiers (colors are set here).
// rgb / alphaScale: line color (0xRRGGBB) and opacity multiplier from the theme.
GridStats DrawGrid(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* const brushes[3],
                   const Camera& camera, const GridSettings& settings, D2D1_SIZE_F viewport,
                   unsigned rgb, float alphaScale);
