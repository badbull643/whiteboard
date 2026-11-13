#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Board.h"
#include "Grid.h"

// What a board file remembers about how it was being viewed.
struct DocView {
    Vec2d center{};  // world point in the middle of the window
    double zoom = 1.0;
    GridMode gridMode = GridMode::Lines;
    int gridSize = 32;
};

// Extra state stored only in the session file (autosave / restore on next start).
struct SessionInfo {
    bool dark = false;
    int colorIndex = 0;
    int widthIndex = 1;
    bool hasCustom = false;
    uint32_t customRgb = 0;
    std::wstring docPath;     // file the board belongs to ("" = untitled)
    bool docModified = false;  // had unsaved changes
    // v2: window
    bool hasWindow = false;
    int32_t windowRect[4] = {};  // normal (restored) position: left, top, right, bottom
    bool maximized = false;
    bool showHelp = true;
    // v3
    bool swapButtons = false;  // middle / right mouse buttons swapped
};

// .wbd format (little-endian), version 3 (versions 1-2 still load):
//   "WBRD" magic, u32 version, u32 flags (bit 0: session)
//   view: f64 centerX, f64 centerY, f64 zoom, u8 gridMode, i32 gridSize
//   [session: u8 dark, i32 color, i32 width, u8 hasCustom, u32 customRgb, u8 modified, u32 len, wchar[len] path
//             v2+: u8 hasWindow, i32[4] windowRect, u8 maximized, u8 showHelp
//             v3+: u8 swapButtons]
//   u32 strokeCount, then per stroke:
//     f64 originX, f64 originY, f32 width, u32 rgb, u8 flags (bit 0: auto ink), u32 n, f32[2n] points
//   u64 FNV-1a checksum of everything before it
// Only live strokes are written (erased/undone ones are dropped = compaction).
// Writes go to a temp file that then replaces the target, so a crash never leaves half a file.
bool SaveBoardFile(const std::wstring& path, const Board& board, const DocView& view,
                   const SessionInfo* session, bool flush, std::wstring& error);

// The two halves of SaveBoardFile, so autosave can serialize on the UI thread (fast, a memory
// copy) and do the slow disk write on a background thread.
void SerializeBoard(const Board& board, const DocView& view, const SessionInfo* session, std::vector<uint8_t>& out);
bool WriteFileAtomic(const std::wstring& path, const std::vector<uint8_t>& data, bool flush, std::wstring& error);

// Writes files on a background thread. Newer submissions replace older ones that haven't
// started yet (only the latest autosave matters).
class BackgroundWriter {
public:
    BackgroundWriter();
    ~BackgroundWriter();  // finishes any pending write
    void Submit(std::wstring path, std::vector<uint8_t>&& data);
    void Flush();         // wait until everything submitted is on disk

private:
    struct Impl;
    Impl* m_impl;
};

// `session` may be null; if the file has session data and `session` is given, it's filled in.
bool LoadBoardFile(const std::wstring& path, std::vector<Stroke>& strokes, DocView& view,
                   SessionInfo* session, bool& hasSession, std::wstring& error);
