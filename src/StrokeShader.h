#pragma once

// HLSL for stroke rendering, compiled once at startup with D3DCompile.
//
// Every stroke vertex carries its stroke id; per-stroke data (origin, width, color, flags)
// lives in a structured buffer, so undo/erase only flips a flag and zoom needs no re-upload.
// Origins are split into two floats (hi + lo) and subtracted from the camera split the same
// way, which keeps positions exact even far away from the world origin.
// Anti-aliasing and round caps are computed per pixel from a distance to the centerline.
inline constexpr char kStrokeShaderSource[] = R"HLSL(
struct StrokeInfo {
    float2 originHi;
    float2 originLo;
    float  width;
    uint   rgb;
    uint   flags;   // bit 0: dead (undone / erased), bit 1: auto ink (theme color)
    float  pad;
};
StructuredBuffer<StrokeInfo> gStrokes : register(t0);

cbuffer Frame : register(b0) {
    float2 camHi;
    float2 camLo;
    float  scale;        // screen px per world unit
    float  minWidthPx;   // strokes are never drawn thinner than this
    float2 viewport;     // px
    float4 ink;          // theme ink color (auto-ink strokes)
    float2 liveOriginHi;
    float2 liveOriginLo;
    float  liveWidth;
    uint   liveRgb;
    uint   liveFlags;
    float  pad0;
};

struct VSIn {
    float2 pos    : POSITION;
    float2 dir    : TEXCOORD0;
    uint   packed : TEXCOORD1;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 sc  : TEXCOORD0;                 // x: along the cap (0..1), y: across (-1..1)
    nointerpolation float  hw    : TEXCOORD1;  // half width in px
    nointerpolation float4 color : COLOR0;     // premultiplied
};

float4 UnpackRgb(uint rgb)
{
    return float4(((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0, 1.0);
}

VSOut VSMain(VSIn v)
{
    const uint id = v.packed & 0xFFFFFF;
    StrokeInfo s;
    if (id == 0xFFFFFF) {
        s.originHi = liveOriginHi; s.originLo = liveOriginLo;
        s.width = liveWidth; s.rgb = liveRgb; s.flags = liveFlags; s.pad = 0;
    } else {
        s = gStrokes[id];
    }

    const float side = ((v.packed >> 24) & 1) ? 1.0 : -1.0;
    const float cap  = (float)((v.packed >> 25) & 1);
    const float wpx  = s.width * scale;
    const float hw   = max(wpx, minWidthPx) * 0.5;
    const float R    = hw + 1.0;   // +1 px fringe for anti-aliasing

    const float2 rel    = (s.originHi - camHi) + (s.originLo - camLo);
    const float2 screen = (rel + v.pos) * scale + v.dir * R;

    VSOut o;
    o.pos = float4(screen.x * 2.0 / viewport.x - 1.0, 1.0 - screen.y * 2.0 / viewport.y, 0.5, 1.0);
    if (s.flags & 1)
        o.pos = float4(0.0, 0.0, -1.0, 1.0);   // dead stroke: clipped away
    o.sc = float2(cap, side);
    o.hw = hw;
    const float4 c = (s.flags & 2) ? ink : UnpackRgb(s.rgb);
    const float a = clamp(wpx / minWidthPx, 0.35, 1.0);   // sub-pixel strokes fade instead of vanishing
    o.color = float4(c.rgb * a, a);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    const float R = i.hw + 1.0;
    const float d = length(i.sc) * R;           // distance to the centerline (round at the caps)
    const float coverage = saturate(i.hw + 0.5 - d);
    return i.color * coverage;
}
)HLSL";
