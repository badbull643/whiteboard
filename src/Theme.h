#pragma once
#include <cstdint>

// All colors are 0xRRGGBB.
struct Theme {
    bool dark;
    uint32_t background;
    uint32_t gridRgb;
    float gridAlpha;     // multiplier on grid opacity
    uint32_t text;
    uint32_t hint;
    uint32_t panel;
    float panelAlpha;
    uint32_t panelBorder;
    uint32_t ink;        // default pen color ("auto ink" strokes follow the theme)
};

inline constexpr Theme kLightTheme{
    false, 0xFAFAF7, 0x2A2F36, 1.0f, 0x1F2328, 0x8A8F98, 0xFFFFFF, 0.92f, 0xC9CDD3, 0x1F2328,
};

inline constexpr Theme kDarkTheme{
    true, 0x1C1D20, 0xFFFFFF, 1.0f, 0xE3E5E8, 0x7D838C, 0x2A2C31, 0.92f, 0x43474F, 0xECEDEF,
};
