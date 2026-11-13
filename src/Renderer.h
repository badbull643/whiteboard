#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <thread>
#include <vector>
#include "Board.h"
#include "Camera.h"
#include "Grid.h"
#include "StrokeBuilder.h"
#include "StrokeMesh.h"
#include "Theme.h"
#include "Toolbar.h"

struct RenderParams {
    const ToolbarView* toolbar = nullptr;
    const Camera* camera = nullptr;
    const GridSettings* grid = nullptr;
    const Theme* theme = nullptr;
    Board* board = nullptr;                  // non-const: new strokes are uploaded to the GPU
    const StrokeBuilder* live = nullptr;     // stroke currently being drawn
    bool showStats = false;
    const wchar_t* benchText = nullptr;      // extra line(s) for the stats panel
    const wchar_t* toolName = nullptr;       // shown in the status bar
    bool showHint = true;                    // shortcut hint line (F1)
    bool showEraser = false;                 // eraser circle at (eraserX, eraserY), client px
    float eraserX = 0.0f, eraserY = 0.0f, eraserRadiusPx = 0.0f;
};

// Renderer on one D3D11 device, drawing into a flip-model swap chain with frame latency 1.
//  - Direct2D: background, grid, text/HUD.
//  - Direct3D 11 + own shader: all strokes. Every stroke is tessellated once into one big
//    GPU vertex buffer; a frame is a single draw call (or a few culled ranges), so CPU cost
//    doesn't grow with the number of strokes on screen.
// All drawing is in physical pixels; UI is scaled via dpiScale.
class Renderer {
public:
    ~Renderer();

    void StartDeviceCreation();  // call first thing at startup (background thread)
    bool Initialize(HWND hwnd);
    void Resize(UINT width, UINT height);
    void SetDpiScale(float scale);

    bool CanRender() const { return m_targetBitmap != nullptr; }
    HANDLE FrameLatencyWaitable() const { return m_waitable; }

    // Returns true if another frame is needed.
    bool Render(const RenderParams& params);
    double LastFrameMs() const { return m_lastFrameMs; }  // CPU time of the last frame

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    static bool CreateD3DDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context);
    std::thread m_deviceThread;
    ComPtr<ID3D11Device> m_pendingDevice;
    ComPtr<ID3D11DeviceContext> m_pendingContext;

    bool CompileShaders();
    bool CompileShadersImpl();
    void Mark(const wchar_t* stage);  // startup profiling: appends "stage ms" to m_gpuBreakdown
    LARGE_INTEGER m_markLast{};
    std::wstring m_gpuBreakdown;
    bool CreateDeviceResources();
    bool CreateStrokePipeline();
    void ReleaseDeviceResources();
    bool CreateTargetBitmap();
    bool CreateTextFormats();
    void HandleDeviceLost();

    // Stroke GPU data
    bool EnsureVertexCapacity(size_t vertices);
    bool EnsureStrokeCapacity(size_t strokes);
    bool EnsureLiveCapacity(size_t vertices);
    void SyncStrokes(Board& board);
    void DrawStrokes(Board* board, const StrokeBuilder* live, const Camera& camera, const Theme& theme);

    void DrawHud(const RenderParams& params);
    void DrawToolbar(const ToolbarView& view, const Theme& theme);
    void DrawStats(const RenderParams& params);

    HWND m_hwnd = nullptr;
    float m_dpiScale = 1.0f;

    // Device-independent
    ComPtr<ID2D1Factory1> m_d2dFactory;
    ComPtr<IDWriteFactory> m_dwFactory;
    ComPtr<IDWriteTextFormat> m_statsFormat;
    ComPtr<IDWriteTextFormat> m_hintFormat;
    ComPtr<IDWriteTextFormat> m_statusFormat;  // right-aligned
    ComPtr<IDWriteTextFormat> m_iconFormat;    // Segoe Fluent Icons (or MDL2 Assets), centered
    ComPtr<ID3DBlob> m_vsBlob;
    ComPtr<ID3DBlob> m_psBlob;

    // Device-dependent: D3D / D2D core
    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    ComPtr<ID2D1Device> m_d2dDevice;
    ComPtr<ID2D1DeviceContext> m_dc;
    ComPtr<IDXGISwapChain2> m_swapChain;
    ComPtr<ID2D1Bitmap1> m_targetBitmap;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    HANDLE m_waitable = nullptr;

    // D2D brushes (colors set from the theme every frame)
    ComPtr<ID2D1SolidColorBrush> m_textBrush;
    ComPtr<ID2D1SolidColorBrush> m_hintBrush;
    ComPtr<ID2D1SolidColorBrush> m_panelBrush;
    ComPtr<ID2D1SolidColorBrush> m_borderBrush;
    ComPtr<ID2D1SolidColorBrush> m_swatchBrush;  // any color (toolbar swatches)
    ComPtr<ID2D1SolidColorBrush> m_gridBrushes[3];
    float m_statsTop = 0.0f;  // stats panel sits below the toolbar

    // Stroke pipeline
    ComPtr<ID3D11VertexShader> m_strokeVS;
    ComPtr<ID3D11PixelShader> m_strokePS;
    ComPtr<ID3D11InputLayout> m_strokeLayout;
    ComPtr<ID3D11Buffer> m_frameCB;
    ComPtr<ID3D11BlendState> m_blend;
    ComPtr<ID3D11RasterizerState> m_raster;

    ComPtr<ID3D11Buffer> m_vb;                  // all strokes, triangle strips stitched together
    size_t m_vbCapacity = 0, m_vbCount = 0;    // in vertices
    ComPtr<ID3D11Buffer> m_strokeBuf;           // per-stroke info (structured buffer)
    ComPtr<ID3D11ShaderResourceView> m_strokeSrv;
    size_t m_strokeCapacity = 0;
    size_t m_uploadedStrokes = 0;
    uint32_t m_boardGeneration = 0;
    StrokeVertex m_lastVertex{};                // for stitching the next strip
    ComPtr<ID3D11Buffer> m_liveVB;              // stroke being drawn (rebuilt each frame)
    size_t m_liveCapacity = 0;

    // Scratch (reused, no per-frame allocations)
    std::vector<StrokeVertex> m_stage, m_strip, m_liveVerts;
    std::vector<D2D1_POINT_2F> m_poly, m_livePts;
    std::vector<uint32_t> m_visibleIds, m_changedIds;

    // Stats
    std::wstring m_adapterName;
    LARGE_INTEGER m_qpcFreq{};
    unsigned long long m_frameCount = 0;
    double m_lastFrameMs = 0.0;
    unsigned m_deviceLostCount = 0;
    GridStats m_gridStats;
    size_t m_visibleStrokes = 0;
    size_t m_candidateStrokes = 0;
    size_t m_drawCalls = 0;
    bool m_drewAll = false;
    double m_queryMs = 0.0;
    double m_strokesMs = 0.0;
    double m_uploadMs = 0.0;
    double m_shaderMs = 0.0;
    std::wstring m_startupText;

public:
    void SetStartupText(std::wstring text) { m_startupText = std::move(text); }
};
