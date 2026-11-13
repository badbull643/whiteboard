#include "App.h"
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include "Stress.h"

namespace {
constexpr wchar_t kClassName[] = L"WhiteboardWindow";
constexpr int kDefaultWidth = 1280;   // in DIPs, scaled by DPI
constexpr int kDefaultHeight = 800;

constexpr double kWheelZoomStep = 1.2;    // per wheel notch (120 units)
constexpr double kKeyZoomStep = 1.25;
constexpr double kWheelPanDips = 80.0;    // per wheel notch

constexpr int kRightDragThresholdPx = 4;  // right button: click = menu, drag = erase

constexpr UINT_PTR kAutosaveTimerId = 1;
constexpr UINT kAutosaveIntervalMs = 30'000;  // session autosave (only if something changed)

// Windows "app mode" setting (Settings > Personalization > Colors).
bool SystemPrefersDark()
{
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
        return value == 0;
    return false;
}
}

namespace {
// Milliseconds since this process was created (for the startup breakdown in the F3 panel).
double MsSinceProcessStart()
{
    FILETIME created, exited, kernel, user, now;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimePreciseAsFileTime(&now);
    const auto toU64 = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return double(toU64(now) - toU64(created)) / 10000.0;
}
}

bool App::Init(HINSTANCE hInstance, int nCmdShow, const std::wstring& fileToOpen)
{
    const double tStart = MsSinceProcessStart();
    m_renderer.StartDeviceCreation();  // GPU driver loads in the background meanwhile

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = &App::WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = nullptr;  // set in WM_SETCURSOR
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));  // src/res/app.ico
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hbrBackground = nullptr;  // we paint everything ourselves; avoids white flashes
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc))
        return false;

    const UINT dpi = GetDpiForSystem();
    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r{ 0, 0, MulDiv(kDefaultWidth, dpi, 96), MulDiv(kDefaultHeight, dpi, 96) };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);

    m_hwnd = CreateWindowExW(0, kClassName, L"Whiteboard", style,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, hInstance, this);
    if (!m_hwnd)
        return false;
    const double tWindow = MsSinceProcessStart();

    // Board and theme first (no GPU needed), so the window can appear right away in the
    // right background color while the GPU driver is still loading.
    const float dpiScale = GetDpiForWindow(m_hwnd) / 96.0f;
    m_camera.dpiScale = dpiScale;
    ResetView();
    SetDark(SystemPrefersDark());  // start in the same mode as Windows (a saved session overrides it)
    RestoreSession();              // last board, file name, colors, view, window position
    UpdateTitle();
    const double tSession = MsSinceProcessStart();

    if (m_restoredWindow) {
        ShowWindow(m_hwnd, m_startMaximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
        ApplyView(m_restoredView);  // re-center for the final window size
    } else {
        ShowWindow(m_hwnd, nCmdShow);
    }
    UpdateWindow(m_hwnd);  // paints the plain background now (WM_ERASEBKGND without GPU)
    const double tShown = MsSinceProcessStart();

    if (!m_renderer.Initialize(m_hwnd)) {
        MessageBoxW(nullptr, L"Failed to initialize Direct3D 11 / Direct2D.", L"Whiteboard", MB_ICONERROR);
        return false;
    }
    m_renderer.SetDpiScale(GetDpiForWindow(m_hwnd) / 96.0f);  // may differ if restored onto another monitor
    m_camera.dpiScale = GetDpiForWindow(m_hwnd) / 96.0f;
    const double tGpu = MsSinceProcessStart();

    RenderNow(true);
    const double tFrame = MsSinceProcessStart();
    wchar_t startup[200];
    swprintf_s(startup,
               L"window visible %.0f ms, first frame %.0f ms (load %.0f, window %.0f, session %.0f, "
               L"show %.0f, GPU %.0f, frame %.0f)",
               tShown, tFrame, tStart, tWindow - tStart, tSession - tWindow, tShown - tSession, tGpu - tShown,
               tFrame - tGpu);
    m_renderer.SetStartupText(startup);
    RequestRedraw();

    DragAcceptFiles(m_hwnd, TRUE);
    SetTimer(m_hwnd, kAutosaveTimerId, kAutosaveIntervalMs, nullptr);

    // A file from the command line opens like File > Open (asks first if the session has unsaved work).
    if (!fileToOpen.empty())
        OpenFile(fileToOpen);
    return true;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

void App::FinishGestures()
{
    CommitStroke();
    EndErase();
    EndPan();
    m_rightDown = false;
    m_tbPressed = -1;
}

DocView App::CurrentView() const
{
    const POINT s = ClientSize();
    DocView v;
    v.center = m_camera.ScreenToWorld(s.x * 0.5, s.y * 0.5);
    v.zoom = m_camera.zoom;
    v.gridMode = m_grid.mode;
    v.gridSize = m_grid.size;
    return v;
}

void App::ApplyView(const DocView& v)
{
    StopZoomAnimation();
    const POINT s = ClientSize();
    m_camera.zoom = std::clamp(v.zoom, Camera::kMinZoom, Camera::kMaxZoom);
    m_camera.PlaceWorldAt(v.center, s.x * 0.5, s.y * 0.5);
    m_grid.mode = v.gridMode;
    m_grid.size = v.gridSize;
    RequestRedraw();
}

void App::UpdateTitle()
{
    std::wstring name = L"Untitled";
    if (!m_docPath.empty()) {
        const size_t slash = m_docPath.find_last_of(L"\\/");
        name = slash == std::wstring::npos ? m_docPath : m_docPath.substr(slash + 1);
    }
    std::wstring title = name + (IsModified() ? L"*" : L"") + L" - Whiteboard";
    if (title != m_title) {
        m_title = title;
        SetWindowTextW(m_hwnd, m_title.c_str());
    }
}

bool App::ConfirmDiscard()
{
    if (!IsModified() || (m_docPath.empty() && m_board.LiveStrokes() == 0))
        return true;
    std::wstring name = m_docPath.empty() ? L"Untitled" : m_docPath.substr(m_docPath.find_last_of(L"\\/") + 1);
    const std::wstring msg = L"Save changes to " + name + L"?";
    const int r = MessageBoxW(m_hwnd, msg.c_str(), L"Whiteboard", MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == IDCANCEL)
        return false;
    if (r == IDYES)
        return Save(false);
    return true;
}

bool App::AskSavePath(std::wstring& path)
{
    wchar_t buf[4096] = L"Untitled.wbd";
    if (!path.empty())
        wcsncpy_s(buf, path.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"Whiteboard (*.wbd)\0*.wbd\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = ARRAYSIZE(buf);
    ofn.lpstrDefExt = L"wbd";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn))
        return false;
    path = buf;
    return true;
}

bool App::AskOpenPath(std::wstring& path)
{
    wchar_t buf[4096] = L"";
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"Whiteboard (*.wbd)\0*.wbd\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = ARRAYSIZE(buf);
    ofn.lpstrDefExt = L"wbd";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
        return false;
    path = buf;
    return true;
}

bool App::Save(bool saveAs)
{
    FinishGestures();
    std::wstring path = m_docPath;
    if ((saveAs || path.empty()) && !AskSavePath(path))
        return false;
    std::wstring error;
    if (!SaveBoardFile(path, m_board, CurrentView(), nullptr, true, error)) {
        MessageBoxW(m_hwnd, (L"Could not save " + path + L"\n\n" + error).c_str(), L"Whiteboard", MB_ICONERROR);
        return false;
    }
    m_docPath = path;
    m_savedRevision = m_board.Revision();
    UpdateTitle();
    SaveSession();  // the session now points at the saved file
    return true;
}

bool App::OpenFile(const std::wstring& path)
{
    FinishGestures();
    if (!ConfirmDiscard())
        return false;
    std::vector<Stroke> strokes;
    DocView view;
    bool hasSession = false;
    std::wstring error;
    if (!LoadBoardFile(path, strokes, view, nullptr, hasSession, error)) {
        MessageBoxW(m_hwnd, (L"Could not open " + path + L"\n\n" + error).c_str(), L"Whiteboard", MB_ICONERROR);
        return false;
    }
    m_board.ReplaceAll(std::move(strokes));
    ApplyView(view);
    m_docPath = path;
    m_savedRevision = m_board.Revision();
    UpdateTitle();
    SaveSession();
    return true;
}

void App::NewBoard()
{
    FinishGestures();
    if (!ConfirmDiscard())
        return;
    m_board.ReplaceAll({});
    m_docPath.clear();
    m_savedRevision = m_board.Revision();
    ResetView();
    UpdateTitle();
    SaveSession();
}

std::wstring App::SessionPath() const
{
    // WHITEBOARD_SESSION_DIR overrides the folder (used by tests so a real session is never touched).
    wchar_t dir[MAX_PATH * 2] = L"";
    if (GetEnvironmentVariableW(L"WHITEBOARD_SESSION_DIR", dir, ARRAYSIZE(dir)) == 0) {
        PWSTR appData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)))
            return {};
        swprintf_s(dir, L"%ls\\Whiteboard", appData);
        CoTaskMemFree(appData);
    }
    CreateDirectoryW(dir, nullptr);  // fine if it already exists
    return std::wstring(dir) + L"\\session.wbd";
}

void App::SaveSession(bool background)
{
    const std::wstring path = SessionPath();
    if (path.empty())
        return;
    SessionInfo info;
    info.dark = m_dark;
    info.colorIndex = m_colorIndex;
    info.widthIndex = m_widthIndex;
    info.hasCustom = m_hasCustom;
    info.customRgb = m_customRgb;
    info.docPath = m_docPath;
    info.docModified = IsModified();
    info.showHelp = m_showHelp;
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (m_fullscreen)
        wp = m_prevPlacement;  // where the window goes back to after fullscreen
    else
        GetWindowPlacement(m_hwnd, &wp);
    if (wp.length == sizeof(wp)) {
        info.hasWindow = true;
        const RECT& r = wp.rcNormalPosition;
        info.windowRect[0] = r.left; info.windowRect[1] = r.top;
        info.windowRect[2] = r.right; info.windowRect[3] = r.bottom;
        info.maximized = wp.showCmd == SW_SHOWMAXIMIZED || (wp.flags & WPF_RESTORETOMAXIMIZED);
    }
    std::vector<uint8_t> data;
    SerializeBoard(m_board, CurrentView(), &info, data);  // fast: a memory copy (~8 MB for 1M points)
    if (background) {
        m_writer.Submit(path, std::move(data));
        m_sessionRevision = m_board.Revision();
        return;
    }
    m_writer.Flush();  // never two writers on the same file
    std::wstring error;
    if (WriteFileAtomic(path, data, false, error))
        m_sessionRevision = m_board.Revision();
}

void App::RestoreSession()
{
    const std::wstring path = SessionPath();
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    std::vector<Stroke> strokes;
    DocView view;
    SessionInfo info;
    bool hasSession = false;
    std::wstring error;
    if (!LoadBoardFile(path, strokes, view, &info, hasSession, error) || !hasSession)
        return;  // unreadable session: start with an empty board
    m_board.ReplaceAll(std::move(strokes));

    // Window position from last time (only if it's still on a connected monitor).
    if (info.hasWindow) {
        RECT r{ info.windowRect[0], info.windowRect[1], info.windowRect[2], info.windowRect[3] };
        if (r.right - r.left >= 200 && r.bottom - r.top >= 150 && MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            WINDOWPLACEMENT wp{ sizeof(wp) };
            wp.rcNormalPosition = r;
            wp.showCmd = SW_HIDE;  // move it now, show it later
            SetWindowPlacement(m_hwnd, &wp);
            m_startMaximized = info.maximized;
            m_restoredWindow = true;
        }
    }
    m_showHelp = info.showHelp;
    ApplyView(view);
    m_restoredView = view;
    SetDark(info.dark);
    m_hasCustom = info.hasCustom;
    m_customRgb = info.customRgb & 0xFFFFFF;
    m_colorIndex = std::clamp(info.colorIndex, 0, m_hasCustom ? kCustomColorIndex : kPaletteSize - 1);
    m_widthIndex = std::clamp(info.widthIndex, 0, kPenWidthCount - 1);
    m_docPath = info.docPath;
    m_savedRevision = info.docModified ? ~0ull : m_board.Revision();
    m_sessionRevision = m_board.Revision();
}

int App::Run()
{
    MSG msg{};
    for (;;) {
        if (m_dirty && m_renderer.CanRender() && !IsIconic(m_hwnd)) {
            // Wait until the swap chain can take a new frame OR input arrives, whichever is first.
            // Input that arrives meanwhile is processed first, so many mouse moves collapse into
            // one frame and pen-to-pixel latency stays at ~1 frame.
            HANDLE waitable = m_renderer.FrameLatencyWaitable();
            DWORD r = MsgWaitForMultipleObjectsEx(1, &waitable, 1000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT)
                RenderNow(false);
        } else {
            WaitMessage();  // nothing to draw: sleep, 0% CPU
        }

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                return static_cast<int>(msg.wParam);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

void App::RenderNow(bool waitForSwapChain)
{
    if (!m_renderer.CanRender() || IsIconic(m_hwnd))
        return;
    if (waitForSwapChain) {
        if (HANDLE h = m_renderer.FrameLatencyWaitable())
            WaitForSingleObjectEx(h, 100, TRUE);
    }
    StepZoomAnimation();
    LayoutToolbar();
    ToolbarView tv;
    tv.toolbar = &m_toolbar;
    tv.hovered = Busy() ? -1 : m_tbHover;
    tv.pressed = m_tbPressed;
    tv.eraser = m_tool == Tool::Eraser;
    tv.colorIndex = m_colorIndex;
    tv.customRgb = m_customRgb;
    tv.hasCustom = m_hasCustom;
    tv.widthIndex = m_widthIndex;
    tv.canUndo = m_board.CanUndo();
    tv.canRedo = m_board.CanRedo();
    tv.dark = m_dark;

    RenderParams params;
    params.toolbar = &tv;
    params.camera = &m_camera;
    params.grid = &m_grid;
    params.theme = &CurrentTheme();
    params.board = &m_board;
    params.live = &m_builder;
    params.showStats = m_showStats;
    params.benchText = m_bench.result.empty() ? nullptr : m_bench.result.c_str();
    params.toolName = m_erasing || m_tool == Tool::Eraser ? L"Eraser" : L"Pen";
    params.showHint = m_showHelp;
    if (EraserCursorVisible()) {
        params.showEraser = true;
        params.eraserX = m_mouse.x + 0.5f;
        params.eraserY = m_mouse.y + 0.5f;
        params.eraserRadiusPx = static_cast<float>(m_eraserRadiusDips * m_camera.dpiScale);
    }
    UpdateTitle();  // "*" when there are unsaved changes (cheap: only sets text when it changes)
    m_dirty = m_renderer.Render(params) || m_zoomAnim.active;  // keep drawing while zoom animates
    if (m_bench.active)
        BenchmarkFrameDone();
}

// ---------------------------------------------------------------------------
// Debug: stress test + benchmark
// ---------------------------------------------------------------------------

namespace {
constexpr double kStressHalfExtent = 20000.0;  // stress strokes fill a 40k x 40k world area
}

void App::AddStressTest()
{
    if (m_builder.Active())
        return;
    AddStressStrokes(m_board, { 0.0, 0.0 }, kStressHalfExtent, 1'000'000, m_stressSeed++);
    m_showStats = true;
    RequestRedraw();
}

void App::SetBenchmarkCamera(int f)
{
    // 0-159: zoom out 100% -> 1%   160-319: zoom in 1% -> 300%   320-479: pan across at 15%.
    const POINT size = ClientSize();
    Vec2d target{ 0.0, 0.0 };
    double zoom;
    if (f < 160) {
        zoom = std::pow(0.01, f / 159.0);
    } else if (f < 320) {
        zoom = 0.01 * std::pow(300.0, (f - 160) / 159.0);
    } else {
        zoom = 0.15;
        target.x = -kStressHalfExtent + 2.0 * kStressHalfExtent * (f - 320) / 159.0;
    }
    m_camera.zoom = zoom;
    m_camera.PlaceWorldAt(target, size.x * 0.5, size.y * 0.5);
}

void App::StartBenchmark()
{
    if (m_builder.Active())
        return;
    m_bench.active = true;
    m_bench.frame = 0;
    m_bench.cpuMs.clear();
    m_bench.intervalMs.clear();
    m_bench.result = L"Benchmark running...";
    m_showStats = true;
    QueryPerformanceCounter(&m_bench.last);
    StopZoomAnimation();
    SetBenchmarkCamera(0);
    RequestRedraw();
}

void App::BenchmarkFrameDone()
{
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (m_bench.frame > 0) {  // skip the first frame (includes setup)
        m_bench.cpuMs.push_back(m_renderer.LastFrameMs());
        m_bench.intervalMs.push_back(double(now.QuadPart - m_bench.last.QuadPart) * 1000.0 / double(freq.QuadPart));
    }
    m_bench.last = now;

    if (++m_bench.frame < Benchmark::kFrames) {
        SetBenchmarkCamera(m_bench.frame);
        RequestRedraw();
        return;
    }

    m_bench.active = false;
    auto summarize = [](std::vector<double> v, double& avg, double& p95, double& mx) {
        std::sort(v.begin(), v.end());
        double sum = 0.0;
        for (double x : v) sum += x;
        avg = v.empty() ? 0.0 : sum / v.size();
        p95 = v.empty() ? 0.0 : v[std::min(v.size() - 1, static_cast<size_t>(v.size() * 0.95))];
        mx = v.empty() ? 0.0 : v.back();
    };
    double cAvg, cP95, cMax, iAvg, iP95, iMax;
    summarize(m_bench.cpuMs, cAvg, cP95, cMax);
    summarize(m_bench.intervalMs, iAvg, iP95, iMax);
    wchar_t buf[256];
    swprintf_s(buf,
               L"Benchmark    %zu frames, %zu strokes / %zu points\n"
               L"  CPU/frame  avg %.2f  p95 %.2f  max %.2f ms\n"
               L"  Interval   avg %.2f  p95 %.2f  max %.2f ms  (%.0f fps)",
               m_bench.cpuMs.size(), m_board.LiveStrokes(), m_board.TotalPoints(),
               cAvg, cP95, cMax, iAvg, iP95, iMax, iAvg > 0 ? 1000.0 / iAvg : 0.0);
    m_bench.result = buf;
    RequestRedraw();
}

void App::SetDark(bool dark)
{
    m_dark = dark;
    const BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(m_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &value, sizeof(value));
    // Nudge the frame so the title bar repaints in the new color right away.
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RequestRedraw();
}

Vec2d App::ClientToWorld(int x, int y) const
{
    return m_camera.ScreenToWorld(x + 0.5, y + 0.5);  // pixel center
}

uint32_t App::PenRgb() const
{
    if (m_colorIndex == kCustomColorIndex)
        return m_customRgb;
    return kPalette[std::clamp(m_colorIndex, 0, kPaletteSize - 1)];
}

void App::SetColor(int index)
{
    if (index == kCustomColorIndex && !m_hasCustom) {
        PickCustomColor();
        return;
    }
    m_colorIndex = std::clamp(index, 0, kCustomColorIndex);
    SetTool(Tool::Pen);  // picking a color means you want to draw
}

void App::SetWidth(int index)
{
    m_widthIndex = std::clamp(index, 0, kPenWidthCount - 1);
    SetTool(Tool::Pen);
}

void App::PickCustomColor()
{
    if (Busy())
        return;
    static COLORREF customColors[16] = {};  // the dialog's "custom colors" row, kept for the session
    CHOOSECOLORW cc{ sizeof(cc) };
    cc.hwndOwner = m_hwnd;
    cc.lpCustColors = customColors;
    // COLORREF is 0x00BBGGRR, ours is 0xRRGGBB.
    const uint32_t start = m_hasCustom ? m_customRgb : PenRgb();
    cc.rgbResult = RGB((start >> 16) & 0xFF, (start >> 8) & 0xFF, start & 0xFF);
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (ChooseColorW(&cc)) {
        m_customRgb = (uint32_t(GetRValue(cc.rgbResult)) << 16) | (uint32_t(GetGValue(cc.rgbResult)) << 8) |
                      GetBValue(cc.rgbResult);
        m_hasCustom = true;
        m_colorIndex = kCustomColorIndex;
        SetTool(Tool::Pen);
    }
    RequestRedraw();
}

void App::LayoutToolbar()
{
    const POINT s = ClientSize();
    m_toolbar.Layout(static_cast<float>(s.x), static_cast<float>(m_camera.dpiScale));
}

void App::UpdateToolbarHover(int x, int y)
{
    LayoutToolbar();
    const bool over = m_toolbar.Contains(x + 0.5f, y + 0.5f);
    const int hover = over ? m_toolbar.HitTest(x + 0.5f, y + 0.5f) : -1;
    if (over != m_overToolbar || hover != m_tbHover) {
        m_overToolbar = over;
        m_tbHover = hover;
        UpdateCursor();
        RequestRedraw();
    }
}

void App::BeginStroke(int x, int y)
{
    m_builder.Begin(ClientToWorld(x, y), m_camera.Scale(),
                    static_cast<float>(kPenWidths[m_widthIndex] * m_camera.dpiScale), PenRgb(),
                    m_colorIndex == 0);
    SetCapture(m_hwnd);
    RequestRedraw();
}

void App::CommitStroke()
{
    if (!m_builder.Active())
        return;
    m_board.Add(m_builder.Finish());  // Finish() clears Active() before capture is released
    if (GetCapture() == m_hwnd)
        ReleaseCapture();
    RequestRedraw();
}

void App::CancelStroke()
{
    if (!m_builder.Active())
        return;
    m_builder.Cancel();
    if (GetCapture() == m_hwnd)
        ReleaseCapture();
    RequestRedraw();
}

void App::Undo()
{
    if (!m_builder.Active() && !m_erasing && m_board.Undo())
        RequestRedraw();
}

void App::Redo()
{
    if (!m_builder.Active() && !m_erasing && m_board.Redo())
        RequestRedraw();
}

void App::ClearBoard()
{
    if (!Busy() && m_board.Clear())  // one undo step brings everything back
        RequestRedraw();
}

void App::SetTool(Tool tool)
{
    m_tool = tool;
    UpdateCursor();
    RequestRedraw();
}

double App::EraserRadiusWorld() const
{
    return m_eraserRadiusDips * m_camera.dpiScale / m_camera.Scale();
}

bool App::EraserCursorVisible() const
{
    if (m_erasing)
        return true;
    return m_tool == Tool::Eraser && m_mouseInside && !m_overToolbar && !m_panning && !m_spaceDown &&
           !m_builder.Active() && m_tbPressed < 0;
}

void App::BeginErase(int x, int y, bool fromRightButton)
{
    m_erasing = true;
    m_eraseFromRight = fromRightButton;
    m_eraseLast = ClientToWorld(x, y);
    m_board.EraseBegin();
    m_board.EraseAlong(m_eraseLast, m_eraseLast, EraserRadiusWorld());
    if (GetCapture() != m_hwnd)
        SetCapture(m_hwnd);
    UpdateCursor();
    RequestRedraw();
}

void App::ContinueErase(int x, int y)
{
    // Erase along the swept capsule from the last position, so fast moves never skip strokes.
    const Vec2d p = ClientToWorld(x, y);
    m_board.EraseAlong(m_eraseLast, p, EraserRadiusWorld());
    m_eraseLast = p;
    RequestRedraw();
}

void App::EndErase()
{
    if (!m_erasing)
        return;
    m_erasing = false;  // before ReleaseCapture (it sends WM_CAPTURECHANGED)
    m_board.EraseEnd();
    if (GetCapture() == m_hwnd)
        ReleaseCapture();
    UpdateCursor();
    RequestRedraw();
}

void App::CancelErase()
{
    if (!m_erasing)
        return;
    m_erasing = false;
    m_board.EraseCancel();  // erased strokes come back
    if (GetCapture() == m_hwnd)
        ReleaseCapture();
    UpdateCursor();
    RequestRedraw();
}

void App::TrackMouse(int x, int y)
{
    m_mouse = { x, y };
    m_mouseInside = true;
    if (!m_trackingLeave) {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd, 0 };
        m_trackingLeave = TrackMouseEvent(&tme) != FALSE;
    }
    if (EraserCursorVisible())
        RequestRedraw();  // the eraser circle follows the mouse
}

void App::HandleCommand(UINT cmd)
{
    switch (cmd) {
    case ID_GRID_LINES: m_grid.mode = GridMode::Lines; break;
    case ID_GRID_DOTS:  m_grid.mode = GridMode::Dots; break;
    case ID_GRID_OFF:   m_grid.mode = GridMode::Off; break;
    case ID_GRID_SMALLER: m_grid.StepPreset(-1); break;
    case ID_GRID_LARGER:  m_grid.StepPreset(+1); break;
    case ID_RESET_VIEW: ResetView(); break;
    case ID_FULLSCREEN: ToggleFullscreen(); break;
    case ID_STATS: m_showStats = !m_showStats; break;
    case ID_HELP: m_showHelp = !m_showHelp; break;
    case ID_UNDO: Undo(); break;
    case ID_REDO: Redo(); break;
    case ID_DARK: SetDark(!m_dark); break;
    case ID_TOOL_PEN: SetTool(Tool::Pen); break;
    case ID_TOOL_ERASER: SetTool(Tool::Eraser); break;
    case ID_CLEAR: ClearBoard(); break;
    case ID_COLOR_CUSTOM: PickCustomColor(); break;
    case ID_FILE_NEW: NewBoard(); break;
    case ID_FILE_OPEN: {
        std::wstring path;
        FinishGestures();
        if (AskOpenPath(path))
            OpenFile(path);
        break;
    }
    case ID_FILE_SAVE: Save(false); break;
    case ID_FILE_SAVE_AS: Save(true); break;
    case ID_WIDTH_SMALLER: SetWidth(m_widthIndex - 1); break;
    case ID_WIDTH_LARGER: SetWidth(m_widthIndex + 1); break;
    case ID_GRID_MENU: {
        // Open under the toolbar's grid button.
        LayoutToolbar();
        for (const auto& b : m_toolbar.Buttons())
            if (b.command == ID_GRID_MENU) {
                POINT pt{ static_cast<LONG>(b.rect.left), static_cast<LONG>(m_toolbar.Panel().bottom + 4) };
                ClientToScreen(m_hwnd, &pt);
                ShowGridMenu(pt.x, pt.y);
            }
        break;
    }
    default:
        if (cmd >= ID_GRID_SIZE_BASE && cmd < ID_GRID_SIZE_BASE + std::size(GridSettings::kPresets))
            m_grid.size = GridSettings::kPresets[cmd - ID_GRID_SIZE_BASE];
        else if (cmd >= ID_COLOR_BASE && cmd < ID_COLOR_BASE + kPaletteSize)
            SetColor(static_cast<int>(cmd - ID_COLOR_BASE));
        else if (cmd >= ID_WIDTH_BASE && cmd < ID_WIDTH_BASE + kPenWidthCount)
            SetWidth(static_cast<int>(cmd - ID_WIDTH_BASE));
        break;
    }
    RequestRedraw();
}

POINT App::ClientSize() const
{
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    return { rc.right - rc.left, rc.bottom - rc.top };
}

void App::BeginPan(int x, int y)
{
    m_panning = true;
    m_panLast = { x, y };
    SetCapture(m_hwnd);
    UpdateCursor();
}

void App::EndPan()
{
    if (!m_panning)
        return;
    m_panning = false;
    if (GetCapture() == m_hwnd)
        ReleaseCapture();
    UpdateCursor();
}

void App::ZoomAtClient(double x, double y, double factor, bool animate)
{
    if (!animate) {
        // Touchpad pinch: many tiny steps; applying them directly feels most direct.
        StopZoomAnimation();
        m_camera.ZoomAt(x, y, factor);
        RequestRedraw();
        return;
    }
    if (!m_zoomAnim.active) {
        m_zoomAnim.target = m_camera.zoom;
        QueryPerformanceCounter(&m_zoomAnim.last);
    }
    // Further notches during the animation stack up on the target.
    m_zoomAnim.target = std::clamp(m_zoomAnim.target * factor, Camera::kMinZoom, Camera::kMaxZoom);
    m_zoomAnim.ax = x;
    m_zoomAnim.ay = y;
    m_zoomAnim.world = m_camera.ScreenToWorld(x, y);
    m_zoomAnim.active = true;
    RequestRedraw();
}

void App::StepZoomAnimation()
{
    if (!m_zoomAnim.active)
        return;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const double dt = std::min(double(now.QuadPart - m_zoomAnim.last.QuadPart) / double(freq.QuadPart), 0.05);
    m_zoomAnim.last = now;

    // Exponential ease in log space: equal-looking speed at any zoom level, ~150 ms to settle.
    constexpr double kTimeConstant = 0.045;
    const double target = std::log(m_zoomAnim.target);
    double current = std::log(m_camera.zoom);
    current += (target - current) * (1.0 - std::exp(-dt / kTimeConstant));
    if (std::fabs(target - current) < 0.002) {
        current = target;
        m_zoomAnim.active = false;
    }
    m_camera.zoom = std::exp(current);
    m_camera.PlaceWorldAt(m_zoomAnim.world, m_zoomAnim.ax, m_zoomAnim.ay);
}

void App::PanBy(double dx, double dy)
{
    m_camera.PanPixels(dx, dy);
    if (m_zoomAnim.active)
        m_zoomAnim.world = m_camera.ScreenToWorld(m_zoomAnim.ax, m_zoomAnim.ay);
    RequestRedraw();
}

void App::ZoomAtCenter(double factor)
{
    const POINT s = ClientSize();
    ZoomAtClient(s.x * 0.5, s.y * 0.5, factor);
}

void App::ResetView()
{
    StopZoomAnimation();
    const POINT s = ClientSize();
    m_camera.Reset(s.x, s.y);
    RequestRedraw();
}

void App::UpdateCursor()
{
    if (m_panning)
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    else if ((m_overToolbar && !m_builder.Active() && !m_erasing) || m_tbPressed >= 0)
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    else if (m_spaceDown)
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
    else if (m_erasing || m_tool == Tool::Eraser)
        SetCursor(nullptr);  // the eraser circle is drawn instead
    else
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
}

void App::ShowGridMenu(int screenX, int screenY)
{
    HMENU menu = CreatePopupMenu();
    AppendGridItems(menu);
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
                                                      screenX, screenY, 0, m_hwnd, nullptr));
    DestroyMenu(menu);
    m_tbHover = -1;  // the mouse probably left the button while the menu was open
    if (cmd != 0)
        HandleCommand(cmd);
    RequestRedraw();
}

// Grid mode radio items + "Grid size" submenu (shared by the context menu and the toolbar).
void App::AppendGridItems(HMENU menu)
{
    HMENU sizeMenu = CreatePopupMenu();
    bool isPreset = false;
    for (UINT i = 0; i < std::size(GridSettings::kPresets); ++i) {
        const int p = GridSettings::kPresets[i];
        wchar_t label[32];
        swprintf_s(label, L"%d px", p);
        const bool checked = p == m_grid.size;
        isPreset |= checked;
        AppendMenuW(sizeMenu, MF_STRING | (checked ? MF_CHECKED : 0u), ID_GRID_SIZE_BASE + i, label);
    }
    if (!isPreset) {
        wchar_t label[48];
        swprintf_s(label, L"Custom: %d px", m_grid.size);
        AppendMenuW(sizeMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sizeMenu, MF_STRING | MF_CHECKED | MF_DISABLED, 0, label);
    }
    AppendMenuW(sizeMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sizeMenu, MF_STRING, ID_GRID_SMALLER, L"Smaller\t[   (Shift+[  = -1 px)");
    AppendMenuW(sizeMenu, MF_STRING, ID_GRID_LARGER, L"Larger\t]   (Shift+]  = +1 px)");

    wchar_t sizeLabel[48];
    swprintf_s(sizeLabel, L"Grid size (%d px)", m_grid.size);

    auto radio = [&](UINT id, const wchar_t* text, GridMode mode) {
        AppendMenuW(menu, MF_STRING | (m_grid.mode == mode ? MF_CHECKED : 0u), id, text);
    };
    radio(ID_GRID_LINES, L"Grid: Lines\tG", GridMode::Lines);
    radio(ID_GRID_DOTS, L"Grid: Dots\tG", GridMode::Dots);
    radio(ID_GRID_OFF, L"Grid: Off\tG", GridMode::Off);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sizeMenu), sizeLabel);
}

void App::ShowContextMenu(int screenX, int screenY)
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_FILE_NEW, L"New board\tCtrl+N");
    AppendMenuW(menu, MF_STRING, ID_FILE_OPEN, L"Open...\tCtrl+O");
    AppendMenuW(menu, MF_STRING, ID_FILE_SAVE, L"Save\tCtrl+S");
    AppendMenuW(menu, MF_STRING, ID_FILE_SAVE_AS, L"Save as...\tCtrl+Shift+S");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (m_board.CanUndo() ? 0u : MF_GRAYED), ID_UNDO, L"Undo\tCtrl+Z");
    AppendMenuW(menu, MF_STRING | (m_board.CanRedo() ? 0u : MF_GRAYED), ID_REDO, L"Redo\tCtrl+Y");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (m_tool == Tool::Pen ? MF_CHECKED : 0u), ID_TOOL_PEN, L"Pen\tP");
    AppendMenuW(menu, MF_STRING | (m_tool == Tool::Eraser ? MF_CHECKED : 0u), ID_TOOL_ERASER,
                L"Eraser\tE  (or right-drag)");
    AppendMenuW(menu, MF_STRING | (m_board.LiveStrokes() ? 0u : MF_GRAYED), ID_CLEAR, L"Clear board\tCtrl+Del");
    AppendMenuW(menu, MF_STRING, ID_COLOR_CUSTOM, L"Custom color...\t9");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendGridItems(menu);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (m_dark ? MF_CHECKED : 0u), ID_DARK, L"Dark mode\tD");
    AppendMenuW(menu, MF_STRING, ID_RESET_VIEW, L"Reset view\tHome");
    AppendMenuW(menu, MF_STRING | (m_fullscreen ? MF_CHECKED : 0u), ID_FULLSCREEN, L"Fullscreen\tF11");
    AppendMenuW(menu, MF_STRING | (m_showHelp ? MF_CHECKED : 0u), ID_HELP, L"Show shortcuts\tF1");
    AppendMenuW(menu, MF_STRING | (m_showStats ? MF_CHECKED : 0u), ID_STATS, L"Show stats\tF3");

    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                                      screenX, screenY, 0, m_hwnd, nullptr));
    DestroyMenu(menu);  // also destroys the submenu
    if (cmd != 0)
        HandleCommand(cmd);
}

void App::ToggleFullscreen()
{
    const DWORD style = static_cast<DWORD>(GetWindowLongW(m_hwnd, GWL_STYLE));
    if (!m_fullscreen) {
        MONITORINFO mi{ sizeof(mi) };
        if (GetWindowPlacement(m_hwnd, &m_prevPlacement) &&
            GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(m_hwnd, GWL_STYLE, static_cast<LONG>(style & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            m_fullscreen = true;
        }
    } else {
        SetWindowLongW(m_hwnd, GWL_STYLE, static_cast<LONG>(style | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(m_hwnd, &m_prevPlacement);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        m_fullscreen = false;
    }
    RequestRedraw();
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    App* app = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->m_hwnd = hwnd;
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (app)
        return app->HandleMessage(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT App::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    // ---- Window / rendering -------------------------------------------------
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            m_renderer.Resize(LOWORD(lParam), HIWORD(lParam));
            // The main loop doesn't run during the modal resize loop, so draw right away.
            if (m_inSizeMove)
                RenderNow(true);
            else
                RequestRedraw();
        }
        return 0;

    case WM_ENTERSIZEMOVE:
        m_inSizeMove = true;
        return 0;

    case WM_EXITSIZEMOVE:
        m_inSizeMove = false;
        RequestRedraw();
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(m_hwnd, &ps);
        EndPaint(m_hwnd, &ps);
        if (!m_inSizeMove)
            RequestRedraw();
        return 0;
    }

    case WM_ERASEBKGND:
        if (!m_renderer.CanRender()) {
            // Startup (GPU still loading): plain background in the theme color, no white flash.
            const uint32_t bg = CurrentTheme().background;
            HBRUSH brush = CreateSolidBrush(RGB((bg >> 16) & 0xFF, (bg >> 8) & 0xFF, bg & 0xFF));
            RECT rc;
            GetClientRect(m_hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, brush);
            DeleteObject(brush);
        }
        return 1;

    case WM_DPICHANGED: {
        // Keep the world point at the view center fixed across the DPI change.
        const POINT oldSize = ClientSize();
        const Vec2d center = m_camera.ScreenToWorld(oldSize.x * 0.5, oldSize.y * 0.5);
        const float dpiScale = HIWORD(wParam) / 96.0f;
        m_renderer.SetDpiScale(dpiScale);
        m_camera.dpiScale = dpiScale;
        const RECT* s = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(m_hwnd, nullptr, s->left, s->top, s->right - s->left, s->bottom - s->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        const POINT newSize = ClientSize();
        m_camera.PlaceWorldAt(center, newSize.x * 0.5, newSize.y * 0.5);
        RequestRedraw();
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize = { 320, 240 };
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            UpdateCursor();
            return TRUE;
        }
        break;

    // ---- Mouse: pan / zoom ----------------------------------------------------
    case WM_MBUTTONDOWN:
        if (!Busy())
            BeginPan(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MBUTTONUP:
        EndPan();
        return 0;

    case WM_LBUTTONDOWN: {
        if (Busy())
            return 0;
        const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        LayoutToolbar();
        if (m_toolbar.Contains(x + 0.5f, y + 0.5f)) {
            // Toolbar: the command runs on release over the same button (like a normal button).
            m_tbPressed = m_toolbar.HitTest(x + 0.5f, y + 0.5f);
            if (m_tbPressed >= 0) {
                SetCapture(m_hwnd);
                RequestRedraw();
            }
            return 0;
        }
        if (m_spaceDown)
            BeginPan(x, y);
        else if (m_tool == Tool::Eraser)
            BeginErase(x, y, false);
        else
            BeginStroke(x, y);
        return 0;
    }

    case WM_LBUTTONUP:
        if (m_tbPressed >= 0) {
            const int pressed = m_tbPressed;
            m_tbPressed = -1;  // before ReleaseCapture (WM_CAPTURECHANGED)
            if (GetCapture() == m_hwnd)
                ReleaseCapture();
            LayoutToolbar();
            const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            if (m_toolbar.HitTest(x + 0.5f, y + 0.5f) == pressed &&
                pressed < static_cast<int>(m_toolbar.Buttons().size()))
                HandleCommand(m_toolbar.Buttons()[pressed].command);
            UpdateToolbarHover(x, y);
            RequestRedraw();
        } else if (m_builder.Active())
            CommitStroke();
        else if (m_erasing && !m_eraseFromRight)
            EndErase();
        else if (m_panning && !(wParam & MK_MBUTTON))
            EndPan();
        return 0;

    case WM_RBUTTONDOWN:
        // Undecided until the mouse moves: a click opens the menu, a drag erases.
        if (!Busy()) {
            m_rightDown = true;
            m_rightDownPt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetCapture(m_hwnd);
        }
        return 0;

    case WM_RBUTTONUP:
        if (m_erasing && m_eraseFromRight) {
            EndErase();
        } else if (m_rightDown) {
            m_rightDown = false;
            if (GetCapture() == m_hwnd)
                ReleaseCapture();
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(m_hwnd, &pt);
            ShowContextMenu(pt.x, pt.y);
        }
        return 0;  // handled: no WM_CONTEXTMENU from DefWindowProc

    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        if (!m_builder.Active() && !m_erasing && !m_panning)
            UpdateToolbarHover(x, y);
        TrackMouse(x, y);
        if (m_rightDown && !m_erasing &&
            (std::abs(x - m_rightDownPt.x) > kRightDragThresholdPx ||
             std::abs(y - m_rightDownPt.y) > kRightDragThresholdPx)) {
            m_rightDown = false;
            BeginErase(m_rightDownPt.x, m_rightDownPt.y, true);
        }
        if (m_builder.Active()) {
            m_builder.Move(ClientToWorld(x, y));
            RequestRedraw();
        } else if (m_erasing) {
            ContinueErase(x, y);
        } else if (m_panning) {
            PanBy(x - m_panLast.x, y - m_panLast.y);
            m_panLast = { x, y };
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        const bool wasVisible = EraserCursorVisible();
        m_trackingLeave = false;
        m_mouseInside = false;
        if ((wasVisible && !EraserCursorVisible()) || m_tbHover >= 0)
            RequestRedraw();  // hide the eraser circle / toolbar hover
        m_tbHover = -1;
        m_overToolbar = false;
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) == m_hwnd)
            return 0;  // we re-captured ourselves (e.g. right-click turning into an erase drag)
        // Capture taken away (e.g. Alt+Tab mid-gesture): keep what was done so far.
        CommitStroke();
        EndErase();
        m_rightDown = false;
        m_tbPressed = -1;
        m_panning = false;
        return 0;

    case WM_COMMAND:
        HandleCommand(LOWORD(wParam));
        return 0;

    case WM_MOUSEWHEEL: {
        const double notches = GET_WHEEL_DELTA_WPARAM(wParam) / double(WHEEL_DELTA);
        const UINT keys = GET_KEYSTATE_WPARAM(wParam);
        if (keys & MK_CONTROL) {
            // Ctrl+wheel and precision-touchpad pinch: zoom around the cursor.
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(m_hwnd, &pt);
            // Whole wheel notches animate smoothly; fractional deltas (touchpad pinch) apply directly.
            const bool wholeNotch = GET_WHEEL_DELTA_WPARAM(wParam) % WHEEL_DELTA == 0;
            ZoomAtClient(pt.x, pt.y, std::pow(kWheelZoomStep, notches), wholeNotch);
        } else {
            const double px = notches * kWheelPanDips * m_camera.dpiScale;
            if (keys & MK_SHIFT)
                PanBy(px, 0.0);
            else
                PanBy(0.0, px);
        }
        return 0;
    }

    case WM_MOUSEHWHEEL: {
        const double notches = GET_WHEEL_DELTA_WPARAM(wParam) / double(WHEEL_DELTA);
        PanBy(-notches * kWheelPanDips * m_camera.dpiScale, 0.0);
        return 0;
    }

    case WM_CONTEXTMENU: {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        if (x == -1 && y == -1) {  // from keyboard (Shift+F10 / menu key)
            POINT pt{ 0, 0 };
            ClientToScreen(m_hwnd, &pt);
            x = pt.x;
            y = pt.y;
        }
        ShowContextMenu(x, y);
        return 0;
    }

    // ---- Keyboard -------------------------------------------------------------
    case WM_KEYDOWN: {
        const bool repeat = (lParam & (1 << 30)) != 0;
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        switch (wParam) {
        case VK_SPACE:
            if (!repeat) {
                m_spaceDown = true;
                UpdateCursor();
            }
            return 0;
        case 'G':
            m_grid.CycleMode();
            RequestRedraw();
            return 0;
        case 'D':
            if (!ctrl)
                SetDark(!m_dark);
            return 0;
        case 'N':
            if (ctrl)
                NewBoard();
            return 0;
        case 'O':
            if (ctrl)
                HandleCommand(ID_FILE_OPEN);
            return 0;
        case 'S':
            if (ctrl)
                Save(shift);  // Ctrl+Shift+S = save as
            return 0;
        case 'P':
            SetTool(Tool::Pen);
            return 0;
        case 'E':
            SetTool(Tool::Eraser);
            return 0;
        case VK_DELETE:
            if (ctrl)
                ClearBoard();
            return 0;
        case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8':
            if (!ctrl && !Busy())
                SetColor(static_cast<int>(wParam - '1'));
            return 0;
        case '9':
            if (!ctrl)
                SetColor(kCustomColorIndex);  // opens the picker the first time
            return 0;
        case VK_OEM_COMMA:
            SetWidth(m_widthIndex - 1);
            return 0;
        case VK_OEM_PERIOD:
            SetWidth(m_widthIndex + 1);
            return 0;
        case 'Z':
            if (ctrl)
                shift ? Redo() : Undo();
            return 0;
        case 'Y':
            if (ctrl)
                Redo();
            return 0;
        case VK_OEM_4:  // [
            shift ? m_grid.StepFine(-1) : m_grid.StepPreset(-1);
            RequestRedraw();
            return 0;
        case VK_OEM_6:  // ]
            shift ? m_grid.StepFine(+1) : m_grid.StepPreset(+1);
            RequestRedraw();
            return 0;
        case VK_HOME:
            ResetView();
            return 0;
        case '0':
        case VK_NUMPAD0:
            if (ctrl) {  // Ctrl+0: zoom 100% around the center
                ZoomAtCenter(1.0 / m_camera.zoom);
                return 0;
            }
            break;
        case VK_OEM_PLUS:
        case VK_ADD:
            ZoomAtCenter(kKeyZoomStep);
            return 0;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            ZoomAtCenter(1.0 / kKeyZoomStep);
            return 0;
        case VK_F1:
            m_showHelp = !m_showHelp;
            RequestRedraw();
            return 0;
        case VK_F3:
            m_showStats = !m_showStats;
            RequestRedraw();
            return 0;
        case VK_F9:  // debug tools: only while the stats panel (F3) is open
            if (!repeat && m_showStats)
                AddStressTest();
            return 0;
        case VK_F10:  // F10 arrives as WM_SYSKEYDOWN; handled there too
            if (!repeat && m_showStats)
                StartBenchmark();
            return 0;
        case VK_F11:
            ToggleFullscreen();
            return 0;
        case VK_ESCAPE:
            if (m_builder.Active())
                CancelStroke();  // Esc mid-stroke throws the stroke away
            else if (m_erasing)
                CancelErase();   // Esc mid-erase brings the strokes back
            else if (m_fullscreen)
                ToggleFullscreen();
            return 0;
        }
        break;
    }

    case WM_SYSKEYDOWN:
        // F10 is a "system" key (it would activate the menu bar); use it for the benchmark.
        if (wParam == VK_F10) {
            if (!(lParam & (1 << 30)) && m_showStats)
                StartBenchmark();
            return 0;
        }
        break;

    case WM_KEYUP:
        if (wParam == VK_SPACE) {
            m_spaceDown = false;
            UpdateCursor();
            return 0;
        }
        break;

    case WM_KILLFOCUS:
        m_spaceDown = false;
        CommitStroke();
        EndErase();
        m_rightDown = false;
        m_tbPressed = -1;
        EndPan();
        return 0;

    case WM_TIMER:
        if (wParam == kAutosaveTimerId && m_board.Revision() != m_sessionRevision && !Busy())
            SaveSession(true);
        return 0;

    case WM_DROPFILES: {
        // Dropping a .wbd file on the window opens it.
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        wchar_t path[4096];
        const bool got = DragQueryFileW(drop, 0, path, ARRAYSIZE(path)) > 0;
        DragFinish(drop);
        if (got) {
            SetForegroundWindow(m_hwnd);
            OpenFile(path);
        }
        return 0;
    }

    case WM_CLOSE:
        // No "save?" prompt: the whole session (including unsaved changes) is restored next time.
        FinishGestures();
        KillTimer(m_hwnd, kAutosaveTimerId);
        SaveSession();
        DestroyWindow(m_hwnd);
        return 0;

    case WM_ENDSESSION:
        if (wParam) {  // Windows is shutting down / signing out
            FinishGestures();
            SaveSession();
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}
