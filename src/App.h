#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "Board.h"
#include "Camera.h"
#include "FileIO.h"
#include "Grid.h"
#include "Renderer.h"
#include "StrokeBuilder.h"
#include "Theme.h"
#include "Toolbar.h"

// Owns the window, the message loop and the high-level app state.
class App {
public:
    // fileToOpen: from the command line (e.g. double-clicked .wbd); empty = restore last session.
    bool Init(HINSTANCE hInstance, int nCmdShow, const std::wstring& fileToOpen);
    int Run();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void RequestRedraw() { m_dirty = true; }
    // waitForSwapChain: block on the frame-latency object first (used outside the main loop).
    void RenderNow(bool waitForSwapChain);
    void ToggleFullscreen();

    // View / grid controls
    POINT ClientSize() const;
    void BeginPan(int x, int y);
    void EndPan();
    void ZoomAtClient(double x, double y, double factor, bool animate = true);
    void ZoomAtCenter(double factor);
    void PanBy(double dx, double dy);  // pans and keeps a running zoom animation anchored
    void StepZoomAnimation();           // called once per frame
    void StopZoomAnimation() { m_zoomAnim.active = false; }

    // Smooth zoom: the camera eases toward `target` (in log space) around a fixed anchor.
    struct ZoomAnimation {
        bool active = false;
        double target = 1.0;
        double ax = 0.0, ay = 0.0;  // anchor, client px
        Vec2d world{};              // world point that stays under the anchor
        LARGE_INTEGER last{};
    } m_zoomAnim;
    void ResetView();
    void ShowContextMenu(int screenX, int screenY);
    void UpdateCursor();

    // Commands (menu items, keyboard shortcuts and WM_COMMAND all end up here)
    void HandleCommand(UINT id);

    // Toolbar
    void LayoutToolbar();
    void UpdateToolbarHover(int x, int y);
    void ShowGridMenu(int screenX, int screenY);
    void AppendGridItems(HMENU menu);

    // Pen color / width
    void SetColor(int index);      // 0..7 palette, kCustomColorIndex = custom
    void SetWidth(int index);
    void PickCustomColor();
    uint32_t PenRgb() const;

    // Drawing
    enum class Tool { Pen, Eraser };
    void SetTool(Tool tool);
    bool Busy() const { return m_builder.Active() || m_panning || m_erasing || m_rightDown || m_tbPressed >= 0; }
    Vec2d ClientToWorld(int x, int y) const;
    void BeginStroke(int x, int y);
    void CommitStroke();
    void CancelStroke();
    void Undo();
    void Redo();
    void ClearBoard();

    // Eraser: left-drag with the eraser tool, or right-drag with any tool.
    double EraserRadiusWorld() const;
    bool EraserCursorVisible() const;
    void BeginErase(int x, int y, bool fromRightButton);
    void ContinueErase(int x, int y);
    void EndErase();
    void CancelErase();
    void TrackMouse(int x, int y);

    // Files
    bool IsModified() const { return m_board.Revision() != m_savedRevision; }
    void FinishGestures();                       // commit/cancel anything in progress
    bool ConfirmDiscard();                       // "Save changes?" -> false = cancelled
    bool Save(bool saveAs);
    bool AskSavePath(std::wstring& path);
    bool AskOpenPath(std::wstring& path);
    bool OpenFile(const std::wstring& path);
    void NewBoard();
    DocView CurrentView() const;
    void ApplyView(const DocView& view);
    void UpdateTitle();

    // Session: the board, its file name and preferences are saved on exit and every
    // 30 s if changed, and restored on the next start.
    std::wstring SessionPath() const;
    void SaveSession(bool background = false);  // background: the disk write happens off the UI thread
    void RestoreSession();
    BackgroundWriter m_writer;
    bool m_showHelp = true;          // F1: hint line at the bottom
    // M: swap middle and right mouse buttons. Off: middle = pan, right = menu / drag to erase.
    // On: right = pan, middle = menu / drag to erase.
    bool m_swapButtons = false;
    void SetSwapButtons(bool swap);
    bool m_startMaximized = false;   // from the session
    bool m_restoredWindow = false;
    DocView m_restoredView;

    // Theme
    const Theme& CurrentTheme() const { return m_dark ? kDarkTheme : kLightTheme; }
    void SetDark(bool dark);

    // Debug: stress test + benchmark (F9 / F10)
    void AddStressTest();
    void StartBenchmark();
    void BenchmarkFrameDone();
    void SetBenchmarkCamera(int frame);

    struct Benchmark {
        static constexpr int kFrames = 480;
        bool active = false;
        int frame = 0;
        std::vector<double> cpuMs;
        std::vector<double> intervalMs;
        LARGE_INTEGER last{};
        std::wstring result;
    } m_bench;
    unsigned m_stressSeed = 1;

    HWND m_hwnd = nullptr;
    Renderer m_renderer;
    Camera m_camera;
    GridSettings m_grid;
    Board m_board;
    StrokeBuilder m_builder;

    std::wstring m_docPath;          // "" = untitled
    uint64_t m_savedRevision = 0;    // board revision at the last save/open
    uint64_t m_sessionRevision = 0;  // board revision at the last session autosave
    std::wstring m_title;

    Toolbar m_toolbar;
    int m_tbHover = -1;
    int m_tbPressed = -1;
    bool m_overToolbar = false;

    int m_colorIndex = 0;  // ink
    int m_widthIndex = 1;  // 3 px
    uint32_t m_customRgb = 0;
    bool m_hasCustom = false;
    float m_eraserRadiusDips = 10.0f;
    bool m_dark = false;
    Tool m_tool = Tool::Pen;

    bool m_erasing = false;
    bool m_eraseFromRight = false;  // gesture started with the right button
    Vec2d m_eraseLast{};
    bool m_rightDown = false;       // right button held, not yet decided: click (menu) or drag (erase)
    POINT m_rightDownPt{};
    POINT m_mouse{};
    bool m_mouseInside = false;
    bool m_trackingLeave = false;

    bool m_dirty = true;        // render-on-demand: only draw when something changed
    bool m_inSizeMove = false;  // inside the modal resize/move loop
    bool m_showStats = false;   // F3 overlay

    bool m_panning = false;
    bool m_spaceDown = false;
    POINT m_panLast{};

    bool m_fullscreen = false;
    WINDOWPLACEMENT m_prevPlacement{ sizeof(WINDOWPLACEMENT) };
};
