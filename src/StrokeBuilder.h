#pragma once
#include <cstdint>
#include "Stroke.h"

// Collects input points for the stroke being drawn and finalizes it on release.
class StrokeBuilder {
public:
    bool Active() const { return m_active; }
    uint64_t Id() const { return m_id; }        // changes for every new stroke
    const Stroke& Live() const { return m_live; }
    // Live().points[0 .. FinalCount()) won't change any more; later ones are still being smoothed.
    size_t FinalCount() const;

    // The newest input position, not yet far enough from the last point to be kept.
    // Drawn as the end of the live stroke so the ink always reaches the cursor.
    bool HasTail() const { return m_hasTail; }
    D2D1_POINT_2F Tail() const { return m_tail; }

    // pxPerUnit: camera scale at the time of drawing. widthPx: pen width in screen pixels.
    void Begin(Vec2d world, double pxPerUnit, float widthPx, uint32_t rgb, bool autoInk);
    void Move(Vec2d world);
    Stroke Finish();  // simplifies the points and returns the finished stroke
    void Cancel();

private:
    D2D1_POINT_2F Smoothed(size_t i) const;

    Stroke m_live;                      // points = smoothed input
    std::vector<D2D1_POINT_2F> m_raw;   // unfiltered input (same indices)
    double m_scale = 1.0;
    bool m_active = false;
    bool m_hasTail = false;
    D2D1_POINT_2F m_tail{};
    uint64_t m_id = 0;
};
