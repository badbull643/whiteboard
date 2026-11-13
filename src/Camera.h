#pragma once
#include <algorithm>

struct Vec2d {
    double x = 0.0, y = 0.0;
};

// View onto the infinite board. Everything is double precision so the view stays
// exact even billions of units away from the origin.
// World units are DIPs: at zoom 1 on a 100% display, 1 world unit = 1 pixel.
class Camera {
public:
    static constexpr double kMinZoom = 0.01;
    static constexpr double kMaxZoom = 100.0;

    double zoom = 1.0;      // user zoom
    double dpiScale = 1.0;  // monitor scale (1.0 = 96 DPI)
    Vec2d offset;           // world position shown at the screen's top-left pixel

    // Screen pixels per world unit.
    double Scale() const { return zoom * dpiScale; }

    Vec2d ScreenToWorld(double sx, double sy) const
    {
        const double s = Scale();
        return { offset.x + sx / s, offset.y + sy / s };
    }

    Vec2d WorldToScreen(double wx, double wy) const
    {
        const double s = Scale();
        return { (wx - offset.x) * s, (wy - offset.y) * s };
    }

    // Move the content by (dx, dy) screen pixels.
    void PanPixels(double dx, double dy)
    {
        const double s = Scale();
        offset.x -= dx / s;
        offset.y -= dy / s;
    }

    // Zoom by `factor`, keeping the world point under (sx, sy) fixed on screen.
    void ZoomAt(double sx, double sy, double factor)
    {
        const Vec2d anchor = ScreenToWorld(sx, sy);
        zoom = std::clamp(zoom * factor, kMinZoom, kMaxZoom);
        const double s = Scale();
        offset.x = anchor.x - sx / s;
        offset.y = anchor.y - sy / s;
    }

    // Place world point `w` at screen position (sx, sy).
    void PlaceWorldAt(Vec2d w, double sx, double sy)
    {
        const double s = Scale();
        offset.x = w.x - sx / s;
        offset.y = w.y - sy / s;
    }

    // Zoom 100%, world origin in the middle of the view.
    void Reset(double viewWidth, double viewHeight)
    {
        zoom = 1.0;
        PlaceWorldAt({ 0.0, 0.0 }, viewWidth * 0.5, viewHeight * 0.5);
    }
};
