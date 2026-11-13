#include "Toolbar.h"
#include <cmath>

void Toolbar::Layout(float viewportWidth, float s, bool swapButtons)
{
    m_dpiScale = s;
    m_buttons.clear();
    m_separators.clear();

    const float pad = 4.0f * s;      // inside the panel
    const float gap = 2.0f * s;      // between buttons
    const float button = 32.0f * s;  // icon buttons
    const float swatch = 28.0f * s;  // color / width buttons
    const float sepW = 9.0f * s;

    float x = pad;
    auto add = [&](UINT id, float w, const wchar_t* tip) {
        m_buttons.push_back({ id, { x, pad, x + w, pad + button }, tip });
        x += w + gap;
    };
    auto separator = [&] {
        m_separators.push_back(x - gap + sepW * 0.5f);
        x += sepW;
    };

    add(ID_TOOL_PEN, button, L"Pen  (P)");
    add(ID_TOOL_ERASER, button, swapButtons ? L"Eraser  (E, or middle-drag)" : L"Eraser  (E, or right-drag)");
    separator();
    for (int i = 0; i < kPaletteSize; ++i)
        add(ID_COLOR_BASE + i, swatch, kPaletteNames[i]);
    add(ID_COLOR_CUSTOM, swatch, L"Custom color...  (9)");
    separator();
    for (int i = 0; i < kPenWidthCount; ++i)
        add(ID_WIDTH_BASE + i, swatch, L"Pen width  ( ,  . )");
    separator();
    add(ID_GRID_MENU, button, L"Grid  (G,  [ ])");
    add(ID_UNDO, button, L"Undo  (Ctrl+Z)");
    add(ID_REDO, button, L"Redo  (Ctrl+Y)");
    add(ID_DARK, button, L"Dark mode  (D)");

    const float width = x - gap + pad;
    const float height = button + 2.0f * pad;
    const float ox = std::round((viewportWidth - width) * 0.5f);
    const float oy = std::round(10.0f * s);
    for (auto& b : m_buttons) {
        b.rect.left += ox; b.rect.right += ox;
        b.rect.top += oy; b.rect.bottom += oy;
    }
    for (float& sx : m_separators)
        sx = std::round(sx + ox) + 0.5f;
    m_panel = { ox, oy, ox + width, oy + height };
}

int Toolbar::HitTest(float x, float y) const
{
    for (size_t i = 0; i < m_buttons.size(); ++i) {
        const D2D1_RECT_F& r = m_buttons[i].rect;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return static_cast<int>(i);
    }
    return -1;
}

bool Toolbar::Contains(float x, float y) const
{
    return x >= m_panel.left && x < m_panel.right && y >= m_panel.top && y < m_panel.bottom;
}
