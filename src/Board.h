#pragma once
#include <vector>
#include "Camera.h"
#include "SpatialIndex.h"
#include "Stroke.h"

// All strokes on the board, a spatial index over the live ones, and a command history.
//
// Stroke ids are indices into `strokes` and never change. Nothing is ever deleted while
// the app runs: erase / undo only mark a stroke dead (O(1), the GPU copy just flips a flag),
// so every command can be undone and redone exactly. Dead strokes are dropped on save.
class Board {
public:
    std::vector<Stroke> strokes;  // includes dead strokes; check `alive`

    static WorldRect BoundsOf(const Stroke& s)
    {
        return { s.origin.x + s.bounds.left, s.origin.y + s.bounds.top,
                 s.origin.x + s.bounds.right, s.origin.y + s.bounds.bottom };
    }

    // ---- Edits (each one is a single undo step) ----
    void Add(Stroke&& s);
    void AddMany(std::vector<Stroke>&& batch);  // one undo step for the whole batch

    // Erasing is a gesture: Begin, any number of EraseAlong calls while dragging (strokes
    // disappear immediately), then End (one undo step) or Cancel (everything comes back).
    void EraseBegin();
    size_t EraseAlong(Vec2d a, Vec2d b, double radius);  // capsule a-b; returns strokes erased now
    void EraseEnd();
    void EraseCancel();
    bool Erasing() const { return m_erasing; }

    bool Clear();  // erase every live stroke (undoable)

    // Replace the whole board (New / Open). Clears the history; all strokes are live.
    void ReplaceAll(std::vector<Stroke>&& newStrokes);

    // Changes on every edit/undo/redo (for "modified" tracking and autosave).
    uint64_t Revision() const { return m_revision; }
    // Changes when stroke ids are reset by ReplaceAll (the renderer re-uploads everything).
    uint32_t Generation() const { return m_generation; }

    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }
    bool Undo();
    bool Redo();

    // ---- Queries ----
    // Ids of live strokes that may intersect `view`, ascending (= draw order).
    void Query(const WorldRect& view, std::vector<uint32_t>& out) { m_index.Query(view, strokes.size(), out); }

    // Ids whose `alive` flag changed since the last call (the renderer updates the GPU copy).
    void TakeChanged(std::vector<uint32_t>& out)
    {
        out.clear();
        out.swap(m_changed);
    }

    size_t LiveStrokes() const { return m_live; }
    size_t TotalPoints() const { return m_points; }  // live strokes only
    const SpatialIndex& Index() const { return m_index; }

private:
    struct Command {
        enum class Type { Add, Erase } type;
        std::vector<uint32_t> ids;
    };

    void Push(Command&& c);
    void Apply(const Command& c, bool forward);  // forward = do/redo, !forward = undo
    void SetAlive(uint32_t id, bool alive);

    SpatialIndex m_index;
    std::vector<Command> m_undo;
    std::vector<Command> m_redo;
    std::vector<uint32_t> m_changed;
    std::vector<uint32_t> m_eraseIds;    // erased during the current gesture
    std::vector<uint32_t> m_candidates;  // scratch
    bool m_erasing = false;
    size_t m_points = 0;
    size_t m_live = 0;
    uint64_t m_revision = 0;
    uint32_t m_generation = 0;
};
