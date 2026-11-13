#include "Renderer.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "StrokeShader.h"

namespace {
constexpr UINT kSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
constexpr float kMinScreenWidthPx = 1.0f;          // strokes never get thinner than this on screen
constexpr size_t kPerStrokeDrawLimit = 256;        // more visible strokes than this -> one draw call
constexpr size_t kDrawAllVertexLimit = 8'000'000;  // beyond this, cull unless most of it is visible

struct GpuStrokeInfo {
    float originHi[2];
    float originLo[2];
    float width;
    uint32_t rgb;
    uint32_t flags;  // bit 0 dead, bit 1 auto ink
    float pad;
};
static_assert(sizeof(GpuStrokeInfo) == 32);

struct FrameConstants {
    float camHi[2];
    float camLo[2];
    float scale;
    float minWidthPx;
    float viewport[2];
    float ink[4];
    float liveOriginHi[2];
    float liveOriginLo[2];
    float liveWidth;
    uint32_t liveRgb;
    uint32_t liveFlags;
    float pad;
};
static_assert(sizeof(FrameConstants) % 16 == 0);

D2D1_COLOR_F Rgb(uint32_t rgb, float a = 1.0f)
{
    return { ((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a };
}

// Double -> float pair (hi + lo) so the GPU can subtract big coordinates without losing precision.
void Split(double v, float& hi, float& lo)
{
    hi = static_cast<float>(v);
    lo = static_cast<float>(v - static_cast<double>(hi));
}

GpuStrokeInfo MakeInfo(const Stroke& s)
{
    GpuStrokeInfo g{};
    Split(s.origin.x, g.originHi[0], g.originLo[0]);
    Split(s.origin.y, g.originHi[1], g.originLo[1]);
    g.width = s.width;
    g.rgb = s.rgb;
    g.flags = (s.alive ? 0u : 1u) | (s.autoInk ? 2u : 0u);
    return g;
}

double TicksToMs(LONGLONG ticks, const LARGE_INTEGER& freq)
{
    return double(ticks) * 1000.0 / double(freq.QuadPart);
}
}  // namespace

Renderer::~Renderer()
{
    if (m_deviceThread.joinable())
        m_deviceThread.join();
    ReleaseDeviceResources();
}

bool Renderer::Initialize(HWND hwnd)
{
    m_hwnd = hwnd;
    QueryPerformanceFrequency(&m_qpcFreq);
    QueryPerformanceCounter(&m_markLast);
    m_gpuBreakdown.clear();

    D2D1_FACTORY_OPTIONS options{};
#ifdef _DEBUG
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                 reinterpret_cast<void**>(m_d2dFactory.GetAddressOf()))))
        return false;
    Mark(L"d2d");

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(m_dwFactory.GetAddressOf()))))
        return false;

    if (!CreateTextFormats())
        return false;
    Mark(L"text");
    if (!CompileShaders())
        return false;
    Mark(L"shaders");

    return CreateDeviceResources();
}

void Renderer::Mark(const wchar_t* name)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    wchar_t buf[48];
    swprintf_s(buf, L"%ls%ls %.0f", m_gpuBreakdown.empty() ? L"" : L", ", name,
               TicksToMs(now.QuadPart - m_markLast.QuadPart, m_qpcFreq));
    m_gpuBreakdown += buf;
    m_markLast = now;
}

bool Renderer::CompileShaders()
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const bool ok = CompileShadersImpl();
    QueryPerformanceCounter(&t1);
    m_shaderMs = TicksToMs(t1.QuadPart - t0.QuadPart, m_qpcFreq);
    return ok;
}

bool Renderer::CompileShadersImpl()
{
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(kStrokeShaderSource, sizeof(kStrokeShaderSource) - 1, "StrokeShader", nullptr, nullptr,
                          "VSMain", "vs_4_0", flags, 0, &m_vsBlob, &errors))) {
        if (errors)
            OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        return false;
    }
    if (FAILED(D3DCompile(kStrokeShaderSource, sizeof(kStrokeShaderSource) - 1, "StrokeShader", nullptr, nullptr,
                          "PSMain", "ps_4_0", flags, 0, &m_psBlob, &errors))) {
        if (errors)
            OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        return false;
    }
    return true;
}

bool Renderer::CreateTextFormats()
{
    m_statsFormat.Reset();
    m_hintFormat.Reset();
    m_statusFormat.Reset();
    if (FAILED(m_dwFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             13.0f * m_dpiScale, L"en-us", &m_statsFormat)))
        return false;
    if (FAILED(m_dwFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             12.0f * m_dpiScale, L"en-us", &m_hintFormat)))
        return false;
    if (FAILED(m_dwFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             12.0f * m_dpiScale, L"en-us", &m_statusFormat)))
        return false;
    m_hintFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    m_statusFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    m_statusFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);

    // Toolbar icons: the Windows 11 icon font, or the Windows 10 one (same code points).
    const wchar_t* iconFamily = L"Segoe MDL2 Assets";
    ComPtr<IDWriteFontCollection> fonts;
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (SUCCEEDED(m_dwFactory->GetSystemFontCollection(&fonts)) &&
        SUCCEEDED(fonts->FindFamilyName(L"Segoe Fluent Icons", &index, &exists)) && exists)
        iconFamily = L"Segoe Fluent Icons";
    m_iconFormat.Reset();
    if (FAILED(m_dwFactory->CreateTextFormat(iconFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             16.0f * m_dpiScale, L"en-us", &m_iconFormat)))
        return false;
    m_iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return true;
}

void Renderer::SetDpiScale(float scale)
{
    if (scale == m_dpiScale)
        return;
    m_dpiScale = scale;
    if (m_dwFactory)  // not yet initialized during startup: formats are created in Initialize()
        CreateTextFormats();
}

bool Renderer::CreateD3DDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context)
{
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // required for Direct2D interop
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) {
        // No usable GPU: fall back to the WARP software rasterizer.
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &device, nullptr, &context);
    }
    return SUCCEEDED(hr);
}

void Renderer::StartDeviceCreation()
{
    // Loading the GPU driver is ~80% of startup (~180 ms on an NVIDIA card). Do it on a
    // worker thread while the window is created and shown; Initialize() picks it up.
    m_deviceThread = std::thread([this] { CreateD3DDevice(m_pendingDevice, m_pendingContext); });
}

bool Renderer::CreateDeviceResources()
{
    if (m_deviceThread.joinable()) {
        m_deviceThread.join();  // first start: device created in the background
        m_d3dDevice = std::move(m_pendingDevice);
        m_d3dContext = std::move(m_pendingContext);
    }
    if (!m_d3dDevice && !CreateD3DDevice(m_d3dDevice, m_d3dContext))
        return false;
    Mark(L"d3d device (wait)");

    ComPtr<IDXGIDevice1> dxgiDevice;
    if (FAILED(m_d3dDevice.As(&dxgiDevice)))
        return false;

    if (FAILED(m_d2dFactory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice)))
        return false;
    if (FAILED(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_dc)))
        return false;
    m_dc->SetDpi(96.0f, 96.0f);  // we work in physical pixels
    Mark(L"d2d device");

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter)))
        return false;
    DXGI_ADAPTER_DESC adapterDesc{};
    if (SUCCEEDED(adapter->GetDesc(&adapterDesc)))
        m_adapterName = adapterDesc.Description;

    ComPtr<IDXGIFactory2> dxgiFactory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory))))
        return false;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 0;   // use window size
    sd.Height = 0;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_NONE;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = kSwapChainFlags;

    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(dxgiFactory->CreateSwapChainForHwnd(m_d3dDevice.Get(), m_hwnd, &sd, nullptr, nullptr, &swapChain1)))
        return false;
    dxgiFactory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(swapChain1.As(&m_swapChain)))
        return false;

    m_swapChain->SetMaximumFrameLatency(1);
    m_waitable = m_swapChain->GetFrameLatencyWaitableObject();
    Mark(L"swapchain");

    ID2D1SolidColorBrush** brushes[] = { &m_textBrush, &m_hintBrush, &m_panelBrush, &m_borderBrush, &m_swatchBrush };
    for (auto* b : brushes)
        if (FAILED(m_dc->CreateSolidColorBrush(Rgb(0), b)))
            return false;
    for (auto& b : m_gridBrushes)
        if (FAILED(m_dc->CreateSolidColorBrush(Rgb(0), &b)))
            return false;

    const bool ok = CreateStrokePipeline() && CreateTargetBitmap();
    Mark(L"pipeline");
    return ok;
}

bool Renderer::CreateStrokePipeline()
{
    if (FAILED(m_d3dDevice->CreateVertexShader(m_vsBlob->GetBufferPointer(), m_vsBlob->GetBufferSize(), nullptr,
                                               &m_strokeVS)) ||
        FAILED(m_d3dDevice->CreatePixelShader(m_psBlob->GetBufferPointer(), m_psBlob->GetBufferSize(), nullptr,
                                              &m_strokePS)))
        return false;

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    static_assert(sizeof(StrokeVertex) == 20);
    if (FAILED(m_d3dDevice->CreateInputLayout(layout, ARRAYSIZE(layout), m_vsBlob->GetBufferPointer(),
                                              m_vsBlob->GetBufferSize(), &m_strokeLayout)))
        return false;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(FrameConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_d3dDevice->CreateBuffer(&cb, nullptr, &m_frameCB)))
        return false;

    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;  // premultiplied alpha
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(m_d3dDevice->CreateBlendState(&bd, &m_blend)))
        return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;  // strips flip winding; both sides are fine
    rd.DepthClipEnable = TRUE;
    return SUCCEEDED(m_d3dDevice->CreateRasterizerState(&rd, &m_raster));
}

bool Renderer::CreateTargetBitmap()
{
    ComPtr<IDXGISurface> surface;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&surface))))
        return false;

    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    if (FAILED(m_dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &m_targetBitmap)))
        return false;
    m_dc->SetTarget(m_targetBitmap.Get());

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
        FAILED(m_d3dDevice->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv))) {
        m_targetBitmap.Reset();
        return false;
    }
    return true;
}

void Renderer::ReleaseDeviceResources()
{
    if (m_dc)
        m_dc->SetTarget(nullptr);
    m_targetBitmap.Reset();
    m_rtv.Reset();
    m_textBrush.Reset();
    m_hintBrush.Reset();
    m_panelBrush.Reset();
    m_borderBrush.Reset();
    m_swatchBrush.Reset();
    for (auto& b : m_gridBrushes)
        b.Reset();

    m_strokeVS.Reset();
    m_strokePS.Reset();
    m_strokeLayout.Reset();
    m_frameCB.Reset();
    m_blend.Reset();
    m_raster.Reset();
    m_vb.Reset();
    m_vbCapacity = m_vbCount = 0;
    m_strokeSrv.Reset();
    m_strokeBuf.Reset();
    m_strokeCapacity = 0;
    m_uploadedStrokes = 0;  // everything is re-uploaded from the CPU copy
    m_liveVB.Reset();
    m_liveCapacity = 0;

    if (m_waitable) {
        CloseHandle(m_waitable);
        m_waitable = nullptr;
    }
    m_swapChain.Reset();
    m_dc.Reset();
    m_d2dDevice.Reset();
    if (m_d3dContext) {
        // Make sure the old swap chain is really destroyed before a new one is created on the same window.
        m_d3dContext->ClearState();
        m_d3dContext->Flush();
    }
    m_d3dContext.Reset();
    m_d3dDevice.Reset();
}

void Renderer::HandleDeviceLost()
{
    ++m_deviceLostCount;
    ReleaseDeviceResources();
    CreateDeviceResources();  // if this fails, CanRender() stays false
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void Renderer::Resize(UINT width, UINT height)
{
    if (!m_swapChain || width == 0 || height == 0)
        return;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    m_swapChain->GetDesc1(&desc);
    if (desc.Width == width && desc.Height == height && m_targetBitmap)
        return;

    m_dc->SetTarget(nullptr);
    m_targetBitmap.Reset();
    m_rtv.Reset();
    m_d3dContext->OMSetRenderTargets(0, nullptr, nullptr);

    const HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, kSwapChainFlags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        HandleDeviceLost();
        return;
    }
    if (SUCCEEDED(hr))
        CreateTargetBitmap();
}

// ---------------------------------------------------------------------------
// Stroke GPU buffers
// ---------------------------------------------------------------------------

bool Renderer::EnsureVertexCapacity(size_t vertices)
{
    if (vertices <= m_vbCapacity)
        return true;
    const size_t cap = std::max({ vertices, m_vbCapacity * 2, size_t{ 1 } << 16 });
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = static_cast<UINT>(cap * sizeof(StrokeVertex));
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    ComPtr<ID3D11Buffer> vb;
    if (FAILED(m_d3dDevice->CreateBuffer(&d, nullptr, &vb)))
        return false;
    if (m_vb && m_vbCount > 0) {
        const D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(m_vbCount * sizeof(StrokeVertex)), 1, 1 };
        m_d3dContext->CopySubresourceRegion(vb.Get(), 0, 0, 0, 0, m_vb.Get(), 0, &box);
    }
    m_vb = vb;
    m_vbCapacity = cap;
    return true;
}

bool Renderer::EnsureStrokeCapacity(size_t strokes)
{
    if (strokes <= m_strokeCapacity)
        return true;
    const size_t cap = std::max({ strokes, m_strokeCapacity * 2, size_t{ 1024 } });
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = static_cast<UINT>(cap * sizeof(GpuStrokeInfo));
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    d.StructureByteStride = sizeof(GpuStrokeInfo);
    ComPtr<ID3D11Buffer> buf;
    if (FAILED(m_d3dDevice->CreateBuffer(&d, nullptr, &buf)))
        return false;
    if (m_strokeBuf && m_uploadedStrokes > 0) {
        const D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(m_uploadedStrokes * sizeof(GpuStrokeInfo)), 1, 1 };
        m_d3dContext->CopySubresourceRegion(buf.Get(), 0, 0, 0, 0, m_strokeBuf.Get(), 0, &box);
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_UNKNOWN;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sv.Buffer.FirstElement = 0;
    sv.Buffer.NumElements = static_cast<UINT>(cap);
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(m_d3dDevice->CreateShaderResourceView(buf.Get(), &sv, &srv)))
        return false;
    m_strokeBuf = buf;
    m_strokeSrv = srv;
    m_strokeCapacity = cap;
    return true;
}

bool Renderer::EnsureLiveCapacity(size_t vertices)
{
    if (vertices <= m_liveCapacity)
        return true;
    const size_t cap = std::max({ vertices, m_liveCapacity * 2, size_t{ 4096 } });
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = static_cast<UINT>(cap * sizeof(StrokeVertex));
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_d3dDevice->CreateBuffer(&d, nullptr, &m_liveVB)))
        return false;
    m_liveCapacity = cap;
    return true;
}

void Renderer::SyncStrokes(Board& board)
{
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);

    // 1) Upload strokes added since last frame: tessellate once, append to the big buffer.
    const size_t first = m_uploadedStrokes;
    const size_t last = std::min(board.strokes.size(), size_t{ kMaxStrokeId });
    if (first < last) {
        m_stage.clear();
        std::vector<GpuStrokeInfo> infos;
        infos.reserve(last - first);
        for (size_t id = first; id < last; ++id) {
            Stroke& s = board.strokes[id];
            m_strip.clear();
            AppendStrokeStrip(s.points.data(), s.points.size(), s.width, static_cast<uint32_t>(id), m_strip, m_poly);
            if (m_strip.empty()) {
                s.gpuFirst = s.gpuCount = 0;
            } else {
                if (m_vbCount + m_stage.size() > 0) {
                    // Stitch strips with two degenerate vertices so everything is drawable in one call.
                    m_stage.push_back(m_lastVertex);
                    m_stage.push_back(m_strip.front());
                }
                s.gpuFirst = static_cast<uint32_t>(m_vbCount + m_stage.size());
                s.gpuCount = static_cast<uint32_t>(m_strip.size());
                m_stage.insert(m_stage.end(), m_strip.begin(), m_strip.end());
                m_lastVertex = m_strip.back();
            }
            infos.push_back(MakeInfo(s));
        }
        if (!m_stage.empty() && EnsureVertexCapacity(m_vbCount + m_stage.size())) {
            const D3D11_BOX box{ static_cast<UINT>(m_vbCount * sizeof(StrokeVertex)), 0, 0,
                                 static_cast<UINT>((m_vbCount + m_stage.size()) * sizeof(StrokeVertex)), 1, 1 };
            m_d3dContext->UpdateSubresource(m_vb.Get(), 0, &box, m_stage.data(), 0, 0);
            m_vbCount += m_stage.size();
        }
        if (EnsureStrokeCapacity(last)) {
            const D3D11_BOX box{ static_cast<UINT>(first * sizeof(GpuStrokeInfo)), 0, 0,
                                 static_cast<UINT>(last * sizeof(GpuStrokeInfo)), 1, 1 };
            m_d3dContext->UpdateSubresource(m_strokeBuf.Get(), 0, &box, infos.data(), 0, 0);
            m_uploadedStrokes = last;
        }
    }

    // 2) Undo/redo: only the stroke's flags change.
    board.TakeChanged(m_changedIds);
    for (uint32_t id : m_changedIds) {
        if (id >= m_uploadedStrokes)
            continue;  // not uploaded yet; uploads read the current flag
        const GpuStrokeInfo info = MakeInfo(board.strokes[id]);
        const D3D11_BOX box{ static_cast<UINT>(id * sizeof(GpuStrokeInfo)), 0, 0,
                             static_cast<UINT>((id + 1) * sizeof(GpuStrokeInfo)), 1, 1 };
        m_d3dContext->UpdateSubresource(m_strokeBuf.Get(), 0, &box, &info, 0, 0);
    }

    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    m_uploadMs = TicksToMs(t1.QuadPart - t0.QuadPart, m_qpcFreq);
}

void Renderer::DrawStrokes(Board* board, const StrokeBuilder* live, const Camera& camera, const Theme& theme)
{
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);

    const D2D1_SIZE_F vp = m_dc->GetSize();
    m_visibleStrokes = m_candidateStrokes = m_drawCalls = 0;
    m_drewAll = false;
    m_uploadMs = 0.0;

    // Visible strokes via the spatial index, then an exact bounds test.
    size_t visibleVerts = 0;
    if (board) {
        if (board->Generation() != m_boardGeneration) {
            // New / Open: stroke ids start over, so the GPU copy does too (buffers are reused).
            m_boardGeneration = board->Generation();
            m_vbCount = 0;
            m_uploadedStrokes = 0;
        }
        SyncStrokes(*board);
        const Vec2d w0 = camera.offset;
        const Vec2d w1 = camera.ScreenToWorld(vp.width, vp.height);
        LARGE_INTEGER q0, q1;
        QueryPerformanceCounter(&q0);
        board->Query({ w0.x, w0.y, w1.x, w1.y }, m_visibleIds);
        m_candidateStrokes = m_visibleIds.size();
        size_t w = 0;
        for (uint32_t id : m_visibleIds) {
            const Stroke& s = board->strokes[id];
            if (id >= m_uploadedStrokes || s.gpuCount == 0 ||
                s.origin.x + s.bounds.right < w0.x || s.origin.x + s.bounds.left > w1.x ||
                s.origin.y + s.bounds.bottom < w0.y || s.origin.y + s.bounds.top > w1.y)
                continue;
            m_visibleIds[w++] = id;
            visibleVerts += s.gpuCount;
        }
        m_visibleIds.resize(w);
        m_visibleStrokes = w;
        QueryPerformanceCounter(&q1);
        m_queryMs = TicksToMs(q1.QuadPart - q0.QuadPart, m_qpcFreq);
    }

    // Stroke being drawn: re-tessellated each frame (it is short), same shader.
    m_liveVerts.clear();
    const Stroke* liveStroke = nullptr;
    if (live && live->Active()) {
        liveStroke = &live->Live();
        m_livePts.assign(liveStroke->points.begin(), liveStroke->points.end());
        if (live->HasTail())
            m_livePts.push_back(live->Tail());
        AppendStrokeStrip(m_livePts.data(), m_livePts.size(), liveStroke->width, kLiveStrokeId, m_liveVerts, m_poly);
    }

    if (m_visibleStrokes == 0 && m_liveVerts.empty())
        return;

    // Per-frame constants.
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(m_d3dContext->Map(m_frameCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;
    FrameConstants& fc = *static_cast<FrameConstants*>(mapped.pData);
    fc = {};
    Split(camera.offset.x, fc.camHi[0], fc.camLo[0]);
    Split(camera.offset.y, fc.camHi[1], fc.camLo[1]);
    fc.scale = static_cast<float>(camera.Scale());
    fc.minWidthPx = kMinScreenWidthPx * m_dpiScale;
    fc.viewport[0] = vp.width;
    fc.viewport[1] = vp.height;
    const D2D1_COLOR_F ink = Rgb(theme.ink);
    fc.ink[0] = ink.r; fc.ink[1] = ink.g; fc.ink[2] = ink.b; fc.ink[3] = 1.0f;
    if (liveStroke) {
        Split(liveStroke->origin.x, fc.liveOriginHi[0], fc.liveOriginLo[0]);
        Split(liveStroke->origin.y, fc.liveOriginHi[1], fc.liveOriginLo[1]);
        fc.liveWidth = liveStroke->width;
        fc.liveRgb = liveStroke->rgb;
        fc.liveFlags = liveStroke->autoInk ? 2u : 0u;
    }
    m_d3dContext->Unmap(m_frameCB.Get(), 0);

    // Pipeline state (Direct2D keeps its own state separately).
    ID3D11DeviceContext* c = m_d3dContext.Get();
    c->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, vp.width, vp.height, 0.0f, 1.0f };
    c->RSSetViewports(1, &viewport);
    c->RSSetState(m_raster.Get());
    c->OMSetBlendState(m_blend.Get(), nullptr, 0xFFFFFFFF);
    c->OMSetDepthStencilState(nullptr, 0);
    c->IASetInputLayout(m_strokeLayout.Get());
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    c->VSSetShader(m_strokeVS.Get(), nullptr, 0);
    c->VSSetConstantBuffers(0, 1, m_frameCB.GetAddressOf());
    c->VSSetShaderResources(0, 1, m_strokeSrv.GetAddressOf());
    c->GSSetShader(nullptr, nullptr, 0);
    c->PSSetShader(m_strokePS.Get(), nullptr, 0);
    const UINT stride = sizeof(StrokeVertex), offset = 0;

    if (m_visibleStrokes > 0) {
        c->IASetVertexBuffers(0, 1, m_vb.GetAddressOf(), &stride, &offset);
        // Many strokes visible: drawing the whole buffer in one call is far cheaper than
        // culling per stroke (off-screen triangles are clipped by the GPU for free).
        const bool drawAll = m_visibleStrokes > kPerStrokeDrawLimit &&
                             (m_vbCount <= kDrawAllVertexLimit || visibleVerts * 3 > m_vbCount);
        if (drawAll) {
            c->Draw(static_cast<UINT>(m_vbCount), 0);
            m_drawCalls = 1;
            m_drewAll = true;
        } else {
            // Few strokes: draw just those, merging neighbors in the buffer into one call.
            size_t runStart = 0, runEnd = 0;
            for (uint32_t id : m_visibleIds) {
                const Stroke& s = board->strokes[id];
                if (runEnd != 0 && s.gpuFirst <= runEnd + 2) {
                    runEnd = s.gpuFirst + s.gpuCount;  // contiguous (stitch vertices are degenerate)
                    continue;
                }
                if (runEnd != 0) {
                    c->Draw(static_cast<UINT>(runEnd - runStart), static_cast<UINT>(runStart));
                    ++m_drawCalls;
                }
                runStart = s.gpuFirst;
                runEnd = s.gpuFirst + s.gpuCount;
            }
            if (runEnd != 0) {
                c->Draw(static_cast<UINT>(runEnd - runStart), static_cast<UINT>(runStart));
                ++m_drawCalls;
            }
        }
    }

    if (!m_liveVerts.empty() && EnsureLiveCapacity(m_liveVerts.size())) {
        D3D11_MAPPED_SUBRESOURCE lm;
        if (SUCCEEDED(c->Map(m_liveVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &lm))) {
            memcpy(lm.pData, m_liveVerts.data(), m_liveVerts.size() * sizeof(StrokeVertex));
            c->Unmap(m_liveVB.Get(), 0);
            c->IASetVertexBuffers(0, 1, m_liveVB.GetAddressOf(), &stride, &offset);
            c->Draw(static_cast<UINT>(m_liveVerts.size()), 0);
            ++m_drawCalls;
        }
    }

    ID3D11ShaderResourceView* nullSrv = nullptr;
    c->VSSetShaderResources(0, 1, &nullSrv);
    c->OMSetRenderTargets(0, nullptr, nullptr);

    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    m_strokesMs = TicksToMs(t1.QuadPart - t0.QuadPart, m_qpcFreq);
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

bool Renderer::Render(const RenderParams& params)
{
    if (!m_targetBitmap)
        return false;
    const Theme& theme = params.theme ? *params.theme : kLightTheme;

    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);

    m_textBrush->SetColor(Rgb(theme.text));
    m_hintBrush->SetColor(Rgb(theme.hint));
    m_panelBrush->SetColor(Rgb(theme.panel, theme.panelAlpha));
    m_borderBrush->SetColor(Rgb(theme.panelBorder));

    // Pass 1 (Direct2D): background + grid.
    m_dc->BeginDraw();
    m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
    m_dc->Clear(Rgb(theme.background));
    if (params.camera && params.grid) {
        ID2D1SolidColorBrush* const brushes[3] = { m_gridBrushes[0].Get(), m_gridBrushes[1].Get(),
                                                   m_gridBrushes[2].Get() };
        m_gridStats = DrawGrid(m_dc.Get(), brushes, *params.camera, *params.grid, m_dc->GetSize(),
                               theme.gridRgb, theme.gridAlpha);
    }
    HRESULT hr = m_dc->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        HandleDeviceLost();
        return true;
    }

    // Pass 2 (Direct3D): strokes.
    if (params.camera)
        DrawStrokes(params.board, params.live, *params.camera, theme);

    // Pass 3 (Direct2D): HUD on top.
    m_dc->BeginDraw();
    DrawHud(params);
    m_statsTop = 0.0f;
    if (params.toolbar && params.toolbar->toolbar) {
        DrawToolbar(*params.toolbar, theme);
        m_statsTop = params.toolbar->toolbar->Panel().bottom;
    }
    if (params.showStats)
        DrawStats(params);
    hr = m_dc->EndDraw();

    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    m_lastFrameMs = TicksToMs(t1.QuadPart - t0.QuadPart, m_qpcFreq);

    if (hr == D2DERR_RECREATE_TARGET) {
        HandleDeviceLost();
        return true;
    }

    hr = m_swapChain->Present(1, 0);  // vsync; frame latency 1 keeps it from queueing
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        HandleDeviceLost();
        return true;
    }
    ++m_frameCount;
    return false;
}

void Renderer::DrawHud(const RenderParams& params)
{
    static constexpr wchar_t kHint[] =
        L"1-9 color    , . width    E eraser / right-drag    Ctrl+Z / Y undo / redo    Ctrl+Wheel zoom    "
        L"Middle / Space+drag pan    G [ ] grid    Right-click menu    F1 hide";
    const D2D1_SIZE_F size = m_dc->GetSize();
    const float pad = 12.0f * m_dpiScale;
    const D2D1_RECT_F rect{ pad, size.height - pad - 18.0f * m_dpiScale, size.width - pad, size.height - pad };

    // Eraser circle (the system cursor is hidden while it shows).
    if (params.showEraser) {
        const D2D1_ELLIPSE e{ { params.eraserX, params.eraserY }, params.eraserRadiusPx, params.eraserRadiusPx };
        m_hintBrush->SetOpacity(0.18f);
        m_dc->FillEllipse(e, m_hintBrush.Get());
        m_hintBrush->SetOpacity(1.0f);
        m_dc->DrawEllipse(e, m_textBrush.Get(), 1.25f * m_dpiScale);
    }

    // Status (right): tool, grid mode, size, zoom.
    if (params.camera && params.grid) {
        const wchar_t* mode = params.grid->mode == GridMode::Lines ? L"Lines"
                            : params.grid->mode == GridMode::Dots  ? L"Dots" : L"Grid off";
        const wchar_t* tool = params.toolName ? params.toolName : L"";
        wchar_t status[128];
        const int n = params.grid->mode == GridMode::Off
            ? swprintf_s(status, L"%ls    %ls    %.0f%%", tool, mode, params.camera->zoom * 100.0)
            : swprintf_s(status, L"%ls    %ls  %d px    %.0f%%", tool, mode, params.grid->size,
                         params.camera->zoom * 100.0);
        if (n > 0)
            m_dc->DrawTextW(status, static_cast<UINT32>(n), m_statusFormat.Get(), rect, m_textBrush.Get());
    }

    // Hint (left) only when enabled (F1) and there's room for it.
    if (params.showHint && size.width > 1240.0f * m_dpiScale)
        m_dc->DrawTextW(kHint, ARRAYSIZE(kHint) - 1, m_hintFormat.Get(), rect, m_hintBrush.Get());
}

void Renderer::DrawStats(const RenderParams& params)
{
    const D2D1_SIZE_F size = m_dc->GetSize();
    Vec2d center{};
    double zoom = 1.0;
    if (params.camera) {
        center = params.camera->ScreenToWorld(size.width * 0.5, size.height * 0.5);
        zoom = params.camera->zoom;
    }
    const size_t strokes = params.board ? params.board->LiveStrokes() : 0;
    const size_t points = params.board ? params.board->TotalPoints() : 0;
    const SpatialIndex* index = params.board ? &params.board->Index() : nullptr;

    wchar_t text[2048];
    const int len = swprintf_s(text,
        L"GPU          %ls\n"
        L"Viewport     %.0f x %.0f px  (DPI %.0f%%)\n"
        L"Frames drawn %llu  (only on change)\n"
        L"Last frame   %.3f ms CPU\n"
        L"Device lost  %u\n"
        L"Zoom         %.2f%%\n"
        L"View center  %.1f, %.1f\n"
        L"Grid         %d primitives, level spacing %.0f\n"
        L"Strokes      %zu (%zu visible), %zu points\n"
        L"Index        %zu tiles, %zu visited, %zu candidates, %.3f ms\n"
        L"Stroke pass  %.3f ms CPU, %zu draw call%ls%ls, upload %.3f ms\n"
        L"GPU mesh     %.2fM vertices, %.1f MB\n"
        L"Startup      %ls\n"
        L"             GPU: %ls\n"
        L"F9 add 1M-point stress test (one Ctrl+Z removes it)    F10 benchmark%ls%ls",
        m_adapterName.c_str(), size.width, size.height, m_dpiScale * 100.0f,
        m_frameCount + 1, m_lastFrameMs, m_deviceLostCount,
        zoom * 100.0, center.x, center.y, m_gridStats.primitives, m_gridStats.spacing,
        strokes, m_visibleStrokes, points,
        index ? index->TileCount() : size_t{ 0 }, index ? index->LastTilesVisited() : size_t{ 0 },
        m_candidateStrokes, m_queryMs,
        m_strokesMs, m_drawCalls, m_drawCalls == 1 ? L"" : L"s", m_drewAll ? L" (whole buffer)" : L"", m_uploadMs,
        m_vbCount / 1e6, m_vbCount * sizeof(StrokeVertex) / (1024.0 * 1024.0), m_startupText.c_str(), m_gpuBreakdown.c_str(),
        params.benchText ? L"\n" : L"", params.benchText ? params.benchText : L"");
    if (len <= 0)
        return;

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(m_dwFactory->CreateTextLayout(text, static_cast<UINT32>(len), m_statsFormat.Get(),
                                             size.width, size.height, &layout)))
        return;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);

    const float margin = 12.0f * m_dpiScale;
    const float pad = 10.0f * m_dpiScale;
    const float top = m_statsTop + margin;
    const D2D1_ROUNDED_RECT panel{
        { margin, top, margin + m.width + 2 * pad, top + m.height + 2 * pad },
        6.0f * m_dpiScale, 6.0f * m_dpiScale };
    m_dc->FillRoundedRectangle(panel, m_panelBrush.Get());
    m_dc->DrawRoundedRectangle(panel, m_borderBrush.Get(), 1.0f);
    m_dc->DrawTextLayout({ margin + pad, top + pad }, layout.Get(), m_textBrush.Get());
}

void Renderer::DrawToolbar(const ToolbarView& view, const Theme& theme)
{
    const Toolbar& tb = *view.toolbar;
    const float s = tb.DpiScale();
    const D2D1_RECT_F panelRect = tb.Panel();

    // Panel with a soft shadow.
    m_swatchBrush->SetColor(Rgb(0x000000, theme.dark ? 0.35f : 0.10f));
    const D2D1_ROUNDED_RECT shadow{ { panelRect.left, panelRect.top + 2.0f * s, panelRect.right,
                                      panelRect.bottom + 2.0f * s }, 9.0f * s, 9.0f * s };
    m_dc->FillRoundedRectangle(shadow, m_swatchBrush.Get());
    const D2D1_ROUNDED_RECT panel{ panelRect, 8.0f * s, 8.0f * s };
    m_swatchBrush->SetColor(Rgb(theme.panel));
    m_dc->FillRoundedRectangle(panel, m_swatchBrush.Get());
    m_dc->DrawRoundedRectangle(panel, m_borderBrush.Get(), 1.0f);

    for (float x : tb.Separators())
        m_dc->DrawLine({ x, panelRect.top + 10.0f * s }, { x, panelRect.bottom - 10.0f * s }, m_borderBrush.Get(), 1.0f);

    const uint32_t penRgb = view.colorIndex == 0 ? theme.ink
                          : view.colorIndex == kCustomColorIndex ? view.customRgb
                          : kPalette[view.colorIndex];

    auto glyph = [&](wchar_t g, const D2D1_RECT_F& r, ID2D1Brush* brush) {
        m_dc->DrawTextW(&g, 1, m_iconFormat.Get(), r, brush);
    };
    auto ring = [&](D2D1_POINT_2F c) {
        m_dc->DrawEllipse({ c, 11.0f * s, 11.0f * s }, m_textBrush.Get(), 1.5f * s);
    };

    const auto& buttons = tb.Buttons();
    for (size_t i = 0; i < buttons.size(); ++i) {
        const ToolbarButton& b = buttons[i];
        const D2D1_RECT_F& r = b.rect;
        const D2D1_POINT_2F c{ (r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f };
        const bool hovered = static_cast<int>(i) == view.hovered;
        const bool pressed = static_cast<int>(i) == view.pressed;
        const UINT cmd = b.command;

        bool active = false;  // highlighted as the current mode
        if (cmd == ID_TOOL_PEN) active = !view.eraser;
        if (cmd == ID_TOOL_ERASER) active = view.eraser;

        if (hovered || pressed || active) {
            const float a = pressed ? 0.22f : active ? (hovered ? 0.20f : 0.14f) : 0.08f;
            m_swatchBrush->SetColor(Rgb(theme.text, a));
            m_dc->FillRoundedRectangle({ r, 6.0f * s, 6.0f * s }, m_swatchBrush.Get());
        }

        if (cmd >= ID_COLOR_BASE && cmd < ID_COLOR_BASE + kPaletteSize) {
            const int idx = static_cast<int>(cmd - ID_COLOR_BASE);
            m_swatchBrush->SetColor(Rgb(idx == 0 ? theme.ink : kPalette[idx]));
            const D2D1_ELLIPSE e{ c, 7.5f * s, 7.5f * s };
            m_dc->FillEllipse(e, m_swatchBrush.Get());
            m_dc->DrawEllipse(e, m_borderBrush.Get(), 1.0f);
            if (idx == view.colorIndex)
                ring(c);
        } else if (cmd == ID_COLOR_CUSTOM) {
            const D2D1_ELLIPSE e{ c, 7.5f * s, 7.5f * s };
            if (view.hasCustom) {
                m_swatchBrush->SetColor(Rgb(view.customRgb));
                m_dc->FillEllipse(e, m_swatchBrush.Get());
                m_dc->DrawEllipse(e, m_borderBrush.Get(), 1.0f);
            } else {
                m_dc->DrawEllipse(e, m_hintBrush.Get(), 1.25f * s);
                glyph(L'\xE710', r, m_hintBrush.Get());  // Add (+)
            }
            if (view.colorIndex == kCustomColorIndex)
                ring(c);
        } else if (cmd >= ID_WIDTH_BASE && cmd < ID_WIDTH_BASE + kPenWidthCount) {
            const int idx = static_cast<int>(cmd - ID_WIDTH_BASE);
            const float radius = std::clamp(kPenWidths[idx] * 0.5f + 0.75f, 1.75f, 7.5f) * s;
            m_swatchBrush->SetColor(Rgb(penRgb));
            m_dc->FillEllipse({ c, radius, radius }, m_swatchBrush.Get());
            if (idx == view.widthIndex)
                ring(c);
        } else {
            wchar_t g = 0;
            bool enabled = true;
            switch (cmd) {
            case ID_TOOL_PEN:    g = L'\xE70F'; break;  // Edit (pencil)
            case ID_TOOL_ERASER: g = L'\xE75C'; break;  // EraseTool
            case ID_GRID_MENU:   g = L'\xE80A'; break;  // GridView
            case ID_UNDO:        g = L'\xE7A7'; enabled = view.canUndo; break;
            case ID_REDO:        g = L'\xE7A6'; enabled = view.canRedo; break;
            case ID_DARK:        g = view.dark ? L'\xE706' : L'\xE708'; break;  // sun / moon
            }
            if (g)
                glyph(g, r, enabled ? m_textBrush.Get() : m_hintBrush.Get());
        }
    }

    // Tooltip under the hovered button.
    if (view.hovered >= 0 && view.hovered < static_cast<int>(buttons.size()) && view.pressed < 0) {
        const ToolbarButton& b = buttons[view.hovered];
        const wchar_t* tip = b.tooltip;
        ComPtr<IDWriteTextLayout> layout;
        if (tip && SUCCEEDED(m_dwFactory->CreateTextLayout(tip, static_cast<UINT32>(wcslen(tip)), m_hintFormat.Get(),
                                                           1000.0f * s, 100.0f * s, &layout))) {
            DWRITE_TEXT_METRICS m{};
            layout->GetMetrics(&m);
            const float padX = 8.0f * s, padY = 4.0f * s;
            const float cx = (b.rect.left + b.rect.right) * 0.5f;
            const float w = m.width + 2 * padX, h = m.height + 2 * padY;
            const float left = std::round(cx - w * 0.5f), top = std::round(panelRect.bottom + 6.0f * s);
            const D2D1_ROUNDED_RECT tipRect{ { left, top, left + w, top + h }, 5.0f * s, 5.0f * s };
            m_swatchBrush->SetColor(Rgb(theme.panel));
            m_dc->FillRoundedRectangle(tipRect, m_swatchBrush.Get());
            m_dc->DrawRoundedRectangle(tipRect, m_borderBrush.Get(), 1.0f);
            m_dc->DrawTextLayout({ left + padX, top + padY }, layout.Get(), m_textBrush.Get());
        }
    }
}
