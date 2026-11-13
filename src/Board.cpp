#include "Board.h"
#include <algorithm>
#include <cmath>

namespace {

struct P2 {
    double x, y;
};
inline P2 Sub(P2 a, P2 b) { return { a.x - b.x, a.y - b.y }; }
inline double Dot(P2 a, P2 b) { return a.x * b.x + a.y * b.y; }

// Squared distance between segments p1-q1 and p2-q2 (Ericson, "Real-Time Collision Detection" 5.1.9).
double SegmentSegmentDistSq(P2 p1, P2 q1, P2 p2, P2 q2)
{
    const P2 d1 = Sub(q1, p1), d2 = Sub(q2, p2), r = Sub(p1, p2);
    const double a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
    constexpr double eps = 1e-12;
    double s, t;
    if (a <= eps && e <= eps) {
        s = t = 0.0;
    } else if (a <= eps) {
        s = 0.0;
        t = std::clamp(f / e, 0.0, 1.0);
    } else {
        const double c = Dot(d1, r);
        if (e <= eps) {
            t = 0.0;
            s = std::clamp(-c / a, 0.0, 1.0);
        } else {
            const double b = Dot(d1, d2);
            const double denom = a * e - b * b;
            s = denom > eps ? std::clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
            t = (b * s + f) / e;
            if (t < 0.0) {
                t = 0.0;
                s = std::clamp(-c / a, 0.0, 1.0);
            } else if (t > 1.0) {
                t = 1.0;
                s = std::clamp((b - c) / a, 0.0, 1.0);
            }
        }
    }
    const P2 c1{ p1.x + d1.x * s, p1.y + d1.y * s };
    const P2 c2{ p2.x + d2.x * t, p2.y + d2.y * t };
    const P2 d = Sub(c1, c2);
    return Dot(d, d);
}

// Does the eraser capsule (a-b, radius r; stroke-local coords) touch the stroke?
bool CapsuleHitsStroke(const Stroke& s, P2 a, P2 b, double r)
{
    const double reach = r + s.width * 0.5;
    const double reach2 = reach * reach;
    const auto& pts = s.points;
    if (pts.size() == 1) {
        const P2 p{ pts[0].x, pts[0].y };
        return SegmentSegmentDistSq(p, p, a, b) <= reach2;
    }
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const P2 p{ pts[i].x, pts[i].y }, q{ pts[i + 1].x, pts[i + 1].y };
        // Cheap reject before the exact test.
        if (std::max(p.x, q.x) + reach < std::min(a.x, b.x) || std::min(p.x, q.x) - reach > std::max(a.x, b.x) ||
            std::max(p.y, q.y) + reach < std::min(a.y, b.y) || std::min(p.y, q.y) - reach > std::max(a.y, b.y))
            continue;
        if (SegmentSegmentDistSq(p, q, a, b) <= reach2)
            return true;
    }
    return false;
}

}  // namespace

void Board::Add(Stroke&& s)
{
    const uint32_t id = static_cast<uint32_t>(strokes.size());
    s.alive = false;  // SetAlive below brings it to life (index, counters, GPU flag)
    strokes.push_back(std::move(s));
    SetAlive(id, true);
    m_changed.pop_back();  // a brand-new stroke is uploaded anyway; no flag update needed
    Push({ Command::Type::Add, { id } });
}

void Board::AddMany(std::vector<Stroke>&& batch)
{
    if (batch.empty())
        return;
    Command c{ Command::Type::Add, {} };
    c.ids.reserve(batch.size());
    strokes.reserve(strokes.size() + batch.size());
    for (Stroke& s : batch) {
        const uint32_t id = static_cast<uint32_t>(strokes.size());
        s.alive = false;
        strokes.push_back(std::move(s));
        SetAlive(id, true);
        m_changed.pop_back();  // new strokes are uploaded anyway
        c.ids.push_back(id);
    }
    Push(std::move(c));
}

void Board::EraseBegin()
{
    m_erasing = true;
    m_eraseIds.clear();
}

size_t Board::EraseAlong(Vec2d a, Vec2d b, double radius)
{
    if (!m_erasing)
        return 0;
    const WorldRect area{ std::min(a.x, b.x) - radius, std::min(a.y, b.y) - radius,
                          std::max(a.x, b.x) + radius, std::max(a.y, b.y) + radius };
    Query(area, m_candidates);
    size_t erased = 0;
    for (uint32_t id : m_candidates) {
        const Stroke& s = strokes[id];
        if (!s.alive)
            continue;
        const WorldRect r = BoundsOf(s);
        if (r.x1 < area.x0 || r.x0 > area.x1 || r.y1 < area.y0 || r.y0 > area.y1)
            continue;
        // Test in stroke-local coordinates (exact anywhere on the infinite board).
        const P2 la{ a.x - s.origin.x, a.y - s.origin.y };
        const P2 lb{ b.x - s.origin.x, b.y - s.origin.y };
        if (CapsuleHitsStroke(s, la, lb, radius)) {
            SetAlive(id, false);
            m_eraseIds.push_back(id);
            ++erased;
        }
    }
    return erased;
}

void Board::EraseEnd()
{
    if (!m_erasing)
        return;
    m_erasing = false;
    if (!m_eraseIds.empty())
        Push({ Command::Type::Erase, std::move(m_eraseIds) });
    m_eraseIds.clear();
}

void Board::EraseCancel()
{
    if (!m_erasing)
        return;
    m_erasing = false;
    for (uint32_t id : m_eraseIds)
        SetAlive(id, true);
    m_eraseIds.clear();
}

bool Board::Clear()
{
    if (m_erasing || m_live == 0)
        return false;
    Command c{ Command::Type::Erase, {} };
    c.ids.reserve(m_live);
    for (uint32_t id = 0; id < strokes.size(); ++id)
        if (strokes[id].alive) {
            SetAlive(id, false);
            c.ids.push_back(id);
        }
    Push(std::move(c));
    return true;
}

void Board::ReplaceAll(std::vector<Stroke>&& newStrokes)
{
    m_index.Clear();
    m_undo.clear();
    m_redo.clear();
    m_changed.clear();
    m_eraseIds.clear();
    m_erasing = false;
    m_points = 0;
    m_live = 0;
    strokes = std::move(newStrokes);
    for (uint32_t id = 0; id < strokes.size(); ++id) {
        Stroke& s = strokes[id];
        s.alive = true;
        s.gpuFirst = s.gpuCount = 0;
        m_index.Insert(id, BoundsOf(s));
        m_points += s.points.size();
        ++m_live;
    }
    ++m_generation;
    ++m_revision;
}

bool Board::Undo()
{
    if (m_erasing || m_undo.empty())
        return false;
    Command c = std::move(m_undo.back());
    m_undo.pop_back();
    Apply(c, false);
    m_redo.push_back(std::move(c));
    ++m_revision;
    return true;
}

bool Board::Redo()
{
    if (m_erasing || m_redo.empty())
        return false;
    Command c = std::move(m_redo.back());
    m_redo.pop_back();
    Apply(c, true);
    m_undo.push_back(std::move(c));
    ++m_revision;
    return true;
}

void Board::Push(Command&& c)
{
    m_undo.push_back(std::move(c));
    m_redo.clear();  // strokes only reachable from redo stay dead for good
    ++m_revision;
}

void Board::Apply(const Command& c, bool forward)
{
    // Add: forward = alive.  Erase: forward = dead.
    const bool alive = (c.type == Command::Type::Add) == forward;
    for (uint32_t id : c.ids)
        SetAlive(id, alive);
}

void Board::SetAlive(uint32_t id, bool alive)
{
    Stroke& s = strokes[id];
    if (s.alive == alive)
        return;
    s.alive = alive;
    if (alive) {
        m_index.Insert(id, BoundsOf(s));
        m_points += s.points.size();
        ++m_live;
    } else {
        m_index.Remove(id, BoundsOf(s));
        m_points -= s.points.size();
        --m_live;
    }
    m_changed.push_back(id);
}
