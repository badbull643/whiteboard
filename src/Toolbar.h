#pragma once
#include <d2d1.h>
#include <cstdint>
#include <vector>
#include "Commands.h"

// Pen colors (0xRRGGBB). Index 0 is "ink": follows the theme (dark on light, light on dark).
inline constexpr int kPaletteSize = 8;
inline constexpr uint32_t kPalette[kPaletteSize] = {
    0x000000, 0xE5484D, 0xF76B15, 0xFFC53D, 0x30A46C, 0x0090FF, 0x8E4EC6, 0x8B8D98,
};
inline constexpr const wchar_t* kPaletteNames[kPaletteSize] = {
    L"Ink  (1)", L"Red  (2)", L"Orange  (3)", L"Yellow  (4)",
    L"Green  (5)", L"Blue  (6)", L"Purple  (7)", L"Gray  (8)",
};
inline constexpr int kCustomColorIndex = kPaletteSize;  // color index 8 = custom color

// Pen widths in DIPs (screen size at the zoom you draw at).
inline constexpr int kPenWidthCount = 5;
inline constexpr float kPenWidths[kPenWidthCount] = { 1.5f, 3.0f, 5.0f, 8.0f, 14.0f };

struct ToolbarButton {
    UINT command;
    D2D1_RECT_F rect;      // physical px
    const wchar_t* tooltip;
};

// Floating toolbar at the top center. Pure layout + hit testing; drawn by the Renderer.
class Toolbar {
public:
    void Layout(float viewportWidth, float dpiScale);
    int HitTest(float x, float y) const;  // button index or -1
    bool Contains(float x, float y) const;

    const std::vector<ToolbarButton>& Buttons() const { return m_buttons; }
    const std::vector<float>& Separators() const { return m_separators; }  // x positions
    D2D1_RECT_F Panel() const { return m_panel; }
    float DpiScale() const { return m_dpiScale; }

private:
    std::vector<ToolbarButton> m_buttons;
    std::vector<float> m_separators;
    D2D1_RECT_F m_panel{};
    float m_dpiScale = 1.0f;
};

// Everything the renderer needs to draw the toolbar in its current state.
struct ToolbarView {
    const Toolbar* toolbar = nullptr;
    int hovered = -1;
    int pressed = -1;
    bool eraser = false;
    int colorIndex = 0;          // 0..7 palette, 8 custom
    uint32_t customRgb = 0;
    bool hasCustom = false;
    int widthIndex = 1;
    bool canUndo = false, canRedo = false, dark = false;
};
