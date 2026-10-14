#include "D3DRenderer.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <stdexcept>
#include <string>
#include <filesystem>
#include <cstddef>    // offsetof
#include <algorithm>  // std::max
#include <vector>
#include <Windows.h>
#include <wincodec.h> // WIC — screenshot PNG encoding

// Bytes of CRTSettings that are actually uploaded to the GPU constant buffer.
// Everything from menuStyle onward is CPU-only and must not be included.
static constexpr UINT kShaderDataSize =
    static_cast<UINT>(offsetof(CRTSettings, menuStyle));
static_assert(kShaderDataSize % 16 == 0,
    "CRTSettings shader fields must fill whole 16-byte constant-buffer rows");

// Size of the raster-bloom luminance grid; must match kLumSize / kLumLastMip
// in crt.hlsl (64 → 1×1 is mip 6).
static constexpr int kLumSize = 64;

// Swap chain flags used at creation and during ResizeBuffers (they must match).
static constexpr UINT kSwapChainFlags =
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "windowscodecs.lib")

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void HR(HRESULT hr, const char* msg) {
    if (FAILED(hr)) {
        OutputDebugStringA(msg);
        OutputDebugStringA("\n");
        // Only break when a debugger is attached — without one, DebugBreak()
        // raises an unhandled breakpoint exception and kills the process
        // before the throw below can be caught.
        if (IsDebuggerPresent()) DebugBreak();
        throw std::runtime_error(msg);
    }
}

D3DRenderer::~D3DRenderer() {
    if (m_frameLatencyHandle) {
        CloseHandle(m_frameLatencyHandle);
        m_frameLatencyHandle = nullptr;
    }
}

static std::filesystem::path ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

// Map formats that cannot be used directly as SRV sources to a usable equivalent.
// Typeless formats are rejected by CreateShaderResourceView; sRGB causes double
// gamma-correction (Electron/Chromium compositors commonly produce these).
static DXGI_FORMAT MapSRVFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:  return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:  return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                                 return fmt;
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool D3DRenderer::Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx) {
    m_device  = device;
    m_context = ctx;

    {
        RECT rc = {};
        if (GetClientRect(hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
            m_width  = rc.right;
            m_height = rc.bottom;
        }
    }

    // -----------------------------------------------------------------------
    // Main swap chain
    // -----------------------------------------------------------------------
    ComPtr<IDXGIDevice1> dxgiDevice;
    HR(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)), "QI IDXGIDevice1");

    ComPtr<IDXGIAdapter> adapter;
    HR(dxgiDevice->GetAdapter(&adapter), "GetAdapter");

    ComPtr<IDXGIFactory2> factory;
    HR(adapter->GetParent(IID_PPV_ARGS(&factory)), "GetParent IDXGIFactory2");

    // -----------------------------------------------------------------------
    // Main swap chain — created for composition, NOT attached to the HWND
    // directly.  This allows the HWND to carry WS_EX_LAYERED|WS_EX_TRANSPARENT
    // so DWM excludes it from hit-testing, routing clicks cross-process to
    // whatever window is below (the game / browser).
    //
    // CreateSwapChainForHwnd is incompatible with WS_EX_LAYERED windows;
    // CreateSwapChainForComposition + DirectComposition is the correct path.
    // -----------------------------------------------------------------------
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width       = static_cast<UINT>(m_width);
    scd.Height      = static_cast<UINT>(m_height);
    scd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc  = { 1, 0 };
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; // flip required for DComp
    scd.Scaling     = DXGI_SCALING_STRETCH;
    scd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    scd.Flags       = kSwapChainFlags;

    HR(factory->CreateSwapChainForComposition(device, &scd, nullptr, &m_swapChain),
       "CreateSwapChainForComposition");

    // Reduce the frame queue from the DXGI default (3) to 1 so frames reach
    // the display one vsync interval after rendering instead of three.
    // GetFrameLatencyWaitableObject() returns a Win32 event that fires when
    // DXGI has room for the next frame — the main loop waits on it instead of
    // spinning between Present calls.
    {
        ComPtr<IDXGISwapChain2> sc2;
        if (SUCCEEDED(m_swapChain.As(&sc2))) {
            sc2->SetMaximumFrameLatency(1);
            m_frameLatencyHandle = sc2->GetFrameLatencyWaitableObject();
        }
    }

    ComPtr<ID3D11Texture2D> backbuffer;
    HR(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)), "GetBuffer");
    HR(device->CreateRenderTargetView(backbuffer.Get(), nullptr, &m_rtv), "CreateRTV");

    // -----------------------------------------------------------------------
    // DirectComposition — connects the composition swap chain to the overlay HWND.
    // -----------------------------------------------------------------------
    ComPtr<IDXGIDevice> dxgiDev;
    HR(device->QueryInterface(IID_PPV_ARGS(&dxgiDev)), "QI IDXGIDevice (dcomp)");
    HR(DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&m_dcompDevice)),
       "DCompositionCreateDevice");
    HR(m_dcompDevice->CreateTargetForHwnd(hwnd, TRUE, &m_dcompTarget),
       "DComp CreateTargetForHwnd");
    HR(m_dcompDevice->CreateVisual(&m_dcompVisual), "DComp CreateVisual");
    HR(m_dcompVisual->SetContent(m_swapChain.Get()), "DComp SetContent");
    HR(m_dcompTarget->SetRoot(m_dcompVisual.Get()), "DComp SetRoot");
    HR(m_dcompDevice->Commit(), "DComp Commit");

    // -----------------------------------------------------------------------
    // Constant buffer, sampler, rasteriser
    // -----------------------------------------------------------------------
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = kShaderDataSize;
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HR(device->CreateBuffer(&cbd, nullptr, &m_cbuffer), "CreateBuffer cbuffer");

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter        = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU      = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV      = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW      = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc= D3D11_COMPARISON_ALWAYS;
    sd.MaxLOD        = D3D11_FLOAT32_MAX;
    HR(device->CreateSamplerState(&sd, &m_sampler), "CreateSamplerState");

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    HR(device->CreateRasterizerState(&rd, &m_rsState), "CreateRasterizerState");

    // Sprite constant buffer (SpriteParams: 4 floats = 16 bytes).
    {
        D3D11_BUFFER_DESC sbd = {};
        sbd.ByteWidth      = 16;
        sbd.Usage          = D3D11_USAGE_DYNAMIC;
        sbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        sbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HR(device->CreateBuffer(&sbd, nullptr, &m_spriteCBuf), "CreateBuffer spriteCBuf");
        // Blur constant buffer (BlurParams: 4 floats = 16 bytes).
        HR(device->CreateBuffer(&sbd, nullptr, &m_blurCBuf), "CreateBuffer blurCBuf");
    }

    // Alpha blend state for cursor sprite (straight alpha).
    {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable           = TRUE;
        bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        HR(device->CreateBlendState(&bd, &m_alphaBlendState), "CreateBlendState (alpha)");
    }

    if (!LoadShaders())         return false;
    if (!CreateIntermediateRT()) return false;
    return true;
}

// ---------------------------------------------------------------------------
// InitStream — create a DXGI swap chain for the stream output window.
// ---------------------------------------------------------------------------
bool D3DRenderer::InitStream(HWND streamHwnd) {
    ComPtr<IDXGIDevice1> dxgiDevice;
    if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return false;

    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);

    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    RECT rc = {};
    if (GetClientRect(streamHwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
        m_streamWidth  = rc.right;
        m_streamHeight = rc.bottom;
    }

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width       = static_cast<UINT>(m_streamWidth);
    scd.Height      = static_cast<UINT>(m_streamHeight);
    scd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc  = { 1, 0 };
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Scaling     = DXGI_SCALING_STRETCH;
    scd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

    HRESULT hr = factory->CreateSwapChainForHwnd(
        m_device, streamHwnd, &scd, nullptr, nullptr, &m_streamSwapChain);
    if (FAILED(hr)) {
        OutputDebugStringA("D3DRenderer: InitStream CreateSwapChainForHwnd failed\n");
        return false;
    }
    factory->MakeWindowAssociation(streamHwnd, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3D11Texture2D> bb;
    m_streamSwapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    m_device->CreateRenderTargetView(bb.Get(), nullptr, &m_streamRtv);

    return true;
}

// ---------------------------------------------------------------------------
// ShutdownStream
// ---------------------------------------------------------------------------
void D3DRenderer::ShutdownStream() {
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_streamRtv.Reset();
    m_streamSwapChain.Reset();
    m_streamPost = {};
}

// ---------------------------------------------------------------------------
// ResizeStream
// ---------------------------------------------------------------------------
void D3DRenderer::ResizeStream(int w, int h) {
    if (!m_streamSwapChain || w <= 0 || h <= 0) return;
    if (w == m_streamWidth && h == m_streamHeight) return;
    m_streamWidth  = w;
    m_streamHeight = h;

    m_streamRtv.Reset();
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_context->Flush();

    m_streamSwapChain->ResizeBuffers(
        0, static_cast<UINT>(w), static_cast<UINT>(h),
        DXGI_FORMAT_UNKNOWN, 0);

    ComPtr<ID3D11Texture2D> bb;
    m_streamSwapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    m_device->CreateRenderTargetView(bb.Get(), nullptr, &m_streamRtv);
}

// ---------------------------------------------------------------------------
// LoadShaders
// ---------------------------------------------------------------------------
bool D3DRenderer::LoadShaders() {
    auto dir = ExeDir();

    ComPtr<ID3DBlob> vsBlob, psBlob, blitBlob;

    HRESULT hr = D3DReadFileToBlob((dir / "crt_vs.cso").wstring().c_str(), &vsBlob);
    if (FAILED(hr)) { OutputDebugStringA("D3DRenderer: failed to load crt_vs.cso\n"); return false; }

    hr = D3DReadFileToBlob((dir / "crt_ps.cso").wstring().c_str(), &psBlob);
    if (FAILED(hr)) { OutputDebugStringA("D3DRenderer: failed to load crt_ps.cso\n"); return false; }

    hr = D3DReadFileToBlob((dir / "crt_blit_ps.cso").wstring().c_str(), &blitBlob);
    if (FAILED(hr)) { OutputDebugStringA("D3DRenderer: failed to load crt_blit_ps.cso\n"); return false; }

    ComPtr<ID3DBlob> spriteVsBlob;
    hr = D3DReadFileToBlob((dir / "sprite_vs.cso").wstring().c_str(), &spriteVsBlob);
    if (FAILED(hr)) { OutputDebugStringA("D3DRenderer: failed to load sprite_vs.cso\n"); return false; }

    HR(m_device->CreateVertexShader(
           vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vs),
       "CreateVertexShader");
    HR(m_device->CreateVertexShader(
           spriteVsBlob->GetBufferPointer(), spriteVsBlob->GetBufferSize(), nullptr, &m_spriteVS),
       "CreateVertexShader (sprite)");
    HR(m_device->CreatePixelShader(
           psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_ps),
       "CreatePixelShader (CRT)");
    HR(m_device->CreatePixelShader(
           blitBlob->GetBufferPointer(), blitBlob->GetBufferSize(), nullptr, &m_blitPS),
       "CreatePixelShader (blit)");

    // Pre-pass pixel shaders.
    const struct { const wchar_t* file; ComPtr<ID3D11PixelShader>* ps; } kPrePasses[] = {
        { L"composite_ps.cso", &m_compositePS },
        { L"glow_down_ps.cso", &m_glowDownPS  },
        { L"halo_down_ps.cso", &m_haloDownPS  },
        { L"blur_ps.cso",      &m_blurPS      },
        { L"persist_ps.cso",   &m_persistPS   },
        { L"lum_down_ps.cso",  &m_lumDownPS   },
        { L"lum_smooth_ps.cso",&m_lumSmoothPS },
    };
    for (const auto& p : kPrePasses) {
        ComPtr<ID3DBlob> blob;
        if (FAILED(D3DReadFileToBlob((dir / p.file).wstring().c_str(), &blob))) {
            OutputDebugStringA("D3DRenderer: failed to load a pre-pass .cso\n");
            return false;
        }
        HR(m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                       nullptr, p.ps->ReleaseAndGetAddressOf()),
           "CreatePixelShader (pre-pass)");
    }

    return true;
}

// ---------------------------------------------------------------------------
// CreateIntermediateRT
// ---------------------------------------------------------------------------
bool D3DRenderer::CreateIntermediateRT() {
    m_intermediateTex.Reset();
    m_intermediateRTV.Reset();
    m_intermediateSRV.Reset();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = static_cast<UINT>(m_width);
    td.Height           = static_cast<UINT>(m_height);
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc       = { 1, 0 };
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &m_intermediateTex);
    if (FAILED(hr)) { OutputDebugStringA("D3DRenderer: CreateTexture2D (intermediate) failed\n"); return false; }

    hr = m_device->CreateRenderTargetView(m_intermediateTex.Get(), nullptr, &m_intermediateRTV);
    if (FAILED(hr)) { OutputDebugStringA("D3DRenderer: CreateRTV (intermediate) failed\n"); return false; }

    hr = m_device->CreateShaderResourceView(m_intermediateTex.Get(), nullptr, &m_intermediateSRV);
    if (FAILED(hr)) { OutputDebugStringA("D3DRenderer: CreateSRV (intermediate) failed\n"); return false; }

    // UI layer — same size and format; holds premultiplied-alpha UI.
    m_uiTex.Reset();
    m_uiRTV.Reset();
    m_uiSRV.Reset();
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_uiTex)) ||
        FAILED(m_device->CreateRenderTargetView(m_uiTex.Get(), nullptr, &m_uiRTV)) ||
        FAILED(m_device->CreateShaderResourceView(m_uiTex.Get(), nullptr, &m_uiSRV))) {
        OutputDebugStringA("D3DRenderer: UI layer creation failed\n");
        m_uiRTV.Reset();
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// UpdateConstantBuffer
// ---------------------------------------------------------------------------
void D3DRenderer::UpdateConstantBuffer(const CRTSettings& settings) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_context->Map(m_cbuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &settings, kShaderDataSize);
        m_context->Unmap(m_cbuffer.Get(), 0);
    }
}

// ---------------------------------------------------------------------------
// BuildCaptureSRV — create an SRV for a WGC texture with format remapping.
// ---------------------------------------------------------------------------
ComPtr<ID3D11ShaderResourceView> D3DRenderer::BuildCaptureSRV(ID3D11Texture2D* tex) {
    D3D11_TEXTURE2D_DESC desc = {};
    tex->GetDesc(&desc);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format              = MapSRVFormat(desc.Format);
    srvDesc.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = m_device->CreateShaderResourceView(tex, &srvDesc, &srv);
    if (FAILED(hr)) {
        OutputDebugStringA("D3DRenderer: BuildCaptureSRV failed\n");
        return nullptr;
    }
    return srv;
}

// ---------------------------------------------------------------------------
// GetOrBuildCaptureSRV — cached lookup so we don't call CreateShaderResourceView
// on every frame.  WGC frame pools reuse the same small set of D3D11 texture
// objects in round-robin order, so after the first cycle every lookup is a hit.
// ---------------------------------------------------------------------------
ID3D11ShaderResourceView* D3DRenderer::GetOrBuildCaptureSRV(ID3D11Texture2D* tex) {
    if (!tex) return nullptr;

    // Search the cache for a matching texture pointer.
    for (auto& e : m_srvCache) {
        if (e.tex == tex && e.srv)
            return e.srv.Get();
    }

    // Cache miss — build and store in the next eviction slot.
    ComPtr<ID3D11ShaderResourceView> srv = BuildCaptureSRV(tex);
    if (!srv) return nullptr;

    m_srvCache[m_srvCacheNext] = { tex, std::move(srv) };
    m_srvCacheNext = (m_srvCacheNext + 1) % static_cast<int>(m_srvCache.size());

    // Return from the slot we just filled (m_srvCacheNext has been advanced).
    int filled = (m_srvCacheNext + static_cast<int>(m_srvCache.size()) - 1)
                 % static_cast<int>(m_srvCache.size());
    return m_srvCache[filled].srv.Get();
}

// ---------------------------------------------------------------------------
// RenderFrame — pipeline:
//   Pass 1  : blit capturedTex → intermediate RT        (format conversion)
//   Stream  : pre-passes + CRT of streamTex → stream swap chain (if active)
//   UI      : ImGui + sprites → transparent UI RT
//   Pre     : composite signal filter, glow / halation blurs (as enabled)
//   Pass 2  : CRT of the picture, UI composited → back buffer
//   Present
// ---------------------------------------------------------------------------
void D3DRenderer::RenderFrame(ID3D11Texture2D* capturedTex, const CRTSettings& settings,
                              ID3D11Texture2D* streamTex, ImDrawData* imguiDrawData,
                              const Sprite* sprites, int spriteCount) {
    // Retry a resize that failed earlier (WM_SIZE won't fire again on its own).
    if (m_pendingWidth > 0 && m_pendingHeight > 0)
        Resize(m_pendingWidth, m_pendingHeight);
    if (!m_rtv || !m_intermediateRTV || !m_uiRTV) return;

    const float black[4] = { 0.f, 0.f, 0.f, 1.f };

    D3D11_VIEWPORT mainVP = {};
    mainVP.Width    = static_cast<float>(m_width);
    mainVP.Height   = static_cast<float>(m_height);
    mainVP.MaxDepth = 1.0f;

    // Update constant buffer once — shared by all passes this frame.
    UpdateConstantBuffer(settings);

    // Common IA / RS state used by all full-screen-quad passes.
    m_context->RSSetState(m_rsState.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(nullptr);

    // -----------------------------------------------------------------------
    // PASS 1 — blit captured game frame into the intermediate RT.
    // -----------------------------------------------------------------------
    m_context->ClearRenderTargetView(m_intermediateRTV.Get(), black);
    {
        ID3D11RenderTargetView* rtv = m_intermediateRTV.Get();
        m_context->OMSetRenderTargets(1, &rtv, nullptr);
    }
    m_context->RSSetViewports(1, &mainVP);

    if (capturedTex) {
        if (ID3D11ShaderResourceView* srvRaw = GetOrBuildCaptureSRV(capturedTex)) {
            m_context->VSSetShader(m_vs.Get(), nullptr, 0);
            m_context->PSSetShader(m_blitPS.Get(), nullptr, 0);

            ID3D11SamplerState* samp = m_sampler.Get();
            m_context->PSSetSamplers(0, 1, &samp);

            m_context->PSSetShaderResources(0, 1, &srvRaw);
            m_context->Draw(3, 0);

            ID3D11ShaderResourceView* nullSrv = nullptr;
            m_context->PSSetShaderResources(0, 1, &nullSrv);
        }
    }
    m_context->OMSetRenderTargets(0, nullptr, nullptr);

    // -----------------------------------------------------------------------
    // UI LAYER — settings panel, cursor and other sprites, drawn into their
    // own transparent RT.  The CRT pass composites it over the picture, so it
    // still gets curvature, scanlines and the mask, but per-line resolve
    // never smears the menu text.
    // -----------------------------------------------------------------------
    {
        const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
        m_context->ClearRenderTargetView(m_uiRTV.Get(), clear);
        ID3D11RenderTargetView* rtv = m_uiRTV.Get();
        m_context->OMSetRenderTargets(1, &rtv, nullptr);
        m_context->RSSetViewports(1, &mainVP);
    }

    if (imguiDrawData)
        ImGui_ImplDX11_RenderDrawData(imguiDrawData);

    // Used by secondary overlays, which have no ImGui context.
    for (int i = 0; i < spriteCount; ++i)
        DrawSprite(sprites[i]);

    // Unbind so the UI RT can be read as an SRV.
    m_context->OMSetRenderTargets(0, nullptr, nullptr);

    // -----------------------------------------------------------------------
    // STREAM — CRT of streamTex → stream swap chain, with its own pre-pass
    // chain sized to the stream source.  The UI layer goes in only when the
    // menu-in-stream option is on; otherwise the stream is a clean picture.
    // -----------------------------------------------------------------------
    if (m_streamSwapChain && m_streamRtv && streamTex) {
        if (ID3D11ShaderResourceView* streamSRV = GetOrBuildCaptureSRV(streamTex)) {
            D3D11_TEXTURE2D_DESC sd = {};
            streamTex->GetDesc(&sd);
            const PostResult post = RunPostChain(m_streamPost, streamSRV,
                static_cast<int>(sd.Width), static_cast<int>(sd.Height), settings);

            ID3D11ShaderResourceView* streamUI = nullptr;
            if (m_streamUIEnabled) {
                BlitStreamUI(static_cast<int>(sd.Width), static_cast<int>(sd.Height));
                streamUI = m_streamUI.srv.Get();
            }
            m_context->RSSetState(m_rsState.Get());
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->IASetInputLayout(nullptr);
            m_context->ClearRenderTargetView(m_streamRtv.Get(), black);
            CRTPass(m_streamRtv.Get(), m_streamWidth, m_streamHeight, post, streamUI);
            m_streamSwapChain->Present(0, 0); // no vsync — driven by main Present
        }
    }

    // -----------------------------------------------------------------------
    // PRE-PASSES + PASS 2 — CRT of the picture → main back buffer → Present.
    // -----------------------------------------------------------------------
    const PostResult post = RunPostChain(m_mainPost, m_intermediateSRV.Get(),
                                         m_width, m_height, settings);
    m_context->ClearRenderTargetView(m_rtv.Get(), black);
    CRTPass(m_rtv.Get(), m_width, m_height, post, m_uiSRV.Get());

    // Grab the finished frame before Present hands the buffer to the compositor.
    if (m_screenshotPending) {
        m_screenshotPending = false;
        m_screenshotResult  = SaveBackBuffer(m_screenshotPath);
    }

    m_swapChain->Present(1, 0); // vsync on main
}

// ---------------------------------------------------------------------------
// Menu in the stream
// ---------------------------------------------------------------------------
void D3DRenderer::SetStreamUI(bool enabled, const RECT& uiRect, const RECT& srcRect) {
    m_streamUIEnabled = enabled && srcRect.right > srcRect.left && srcRect.bottom > srcRect.top;
    m_streamUIRect    = uiRect;
    m_streamSrcRect   = srcRect;
}

// Copies the UI layer into m_streamUI (w x h, standing for the stream
// source's area on screen), placed by the viewport so each part of the menu
// lands over the part of the source it covers on screen.
void D3DRenderer::BlitStreamUI(int w, int h) {
    if (m_streamUI.w != w || m_streamUI.h != h || !m_streamUI.rtv)
        if (!CreateRenderTarget(m_streamUI, w, h, DXGI_FORMAT_R8G8B8A8_UNORM)) return;

    const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
    m_context->ClearRenderTargetView(m_streamUI.rtv.Get(), clear);

    const float sx = static_cast<float>(w) / static_cast<float>(m_streamSrcRect.right - m_streamSrcRect.left);
    const float sy = static_cast<float>(h) / static_cast<float>(m_streamSrcRect.bottom - m_streamSrcRect.top);
    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = static_cast<float>(m_streamUIRect.left - m_streamSrcRect.left) * sx;
    vp.TopLeftY = static_cast<float>(m_streamUIRect.top  - m_streamSrcRect.top)  * sy;
    vp.Width    = static_cast<float>(m_streamUIRect.right  - m_streamUIRect.left) * sx;
    vp.Height   = static_cast<float>(m_streamUIRect.bottom - m_streamUIRect.top)  * sy;
    vp.MaxDepth = 1.f;
    if (vp.Width <= 0.f || vp.Height <= 0.f) return;

    ID3D11RenderTargetView* rtv = m_streamUI.rtv.Get();
    m_context->OMSetRenderTargets(1, &rtv, nullptr);
    m_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);   // copy alpha as it is
    m_context->RSSetViewports(1, &vp);
    m_context->RSSetState(m_rsState.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(nullptr);
    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    m_context->PSSetShader(m_blitPS.Get(), nullptr, 0);
    ID3D11SamplerState* samp = m_sampler.Get();
    m_context->PSSetSamplers(0, 1, &samp);
    ID3D11ShaderResourceView* src = m_uiSRV.Get();
    m_context->PSSetShaderResources(0, 1, &src);
    m_context->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSrv);
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Screenshots
// ---------------------------------------------------------------------------
void D3DRenderer::RequestScreenshot(std::filesystem::path path) {
    m_screenshotPath    = std::move(path);
    m_screenshotPending = true;
}

std::optional<bool> D3DRenderer::PollScreenshot() {
    std::optional<bool> r = m_screenshotResult;
    m_screenshotResult.reset();
    return r;
}

// Copies the current back buffer to the CPU and writes it as a PNG via WIC.
bool D3DRenderer::SaveBackBuffer(const std::filesystem::path& path) {
    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) return false;
    return SaveTexturePng(backbuffer.Get(), path);
}

// Renders drawData on white into an off-screen w x h target and saves it.
bool D3DRenderer::SaveDrawData(ImDrawData* drawData, int w, int h, const std::filesystem::path& path) {
    if (!drawData || w <= 0 || h <= 0) return false;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width      = static_cast<UINT>(w);
    td.Height     = static_cast<UINT>(h);
    td.MipLevels  = 1;
    td.ArraySize  = 1;
    td.Format     = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc = { 1, 0 };
    td.BindFlags  = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D>        tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &tex)) ||
        FAILED(m_device->CreateRenderTargetView(tex.Get(), nullptr, &rtv)))
        return false;

    const float white[4] = { 1.f, 1.f, 1.f, 1.f };
    m_context->ClearRenderTargetView(rtv.Get(), white);
    ID3D11RenderTargetView* r = rtv.Get();
    m_context->OMSetRenderTargets(1, &r, nullptr);
    ImGui_ImplDX11_RenderDrawData(drawData);   // sets its own viewport from the draw data
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    return SaveTexturePng(tex.Get(), path);
}

// Copies a B8G8R8A8 texture to the CPU and writes it as an opaque PNG via WIC.
bool D3DRenderer::SaveTexturePng(ID3D11Texture2D* source, const std::filesystem::path& path) {
    D3D11_TEXTURE2D_DESC td = {};
    source->GetDesc(&td);
    td.BindFlags      = 0;
    td.MiscFlags      = 0;
    td.Usage          = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &staging))) return false;
    m_context->CopyResource(staging.Get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const UINT stride = td.Width * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(stride) * td.Height);
    for (UINT y = 0; y < td.Height; ++y)
        memcpy(&pixels[y * stride], static_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch, stride);
    m_context->Unmap(staging.Get(), 0);
    // Alpha isn't meaningful here (the swap chain ignores it) — make the PNG opaque.
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    ComPtr<IWICImagingFactory>    wic;
    ComPtr<IWICStream>            stream;
    ComPtr<IWICBitmapEncoder>     encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&wic)))
        && SUCCEEDED(wic->CreateStream(&stream))
        && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))
        && SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
        && SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))
        && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr))
        && SUCCEEDED(frame->Initialize(nullptr))
        && SUCCEEDED(frame->SetSize(td.Width, td.Height))
        && SUCCEEDED(frame->SetPixelFormat(&format))
        && format == GUID_WICPixelFormat32bppBGRA
        && SUCCEEDED(frame->WritePixels(td.Height, stride, static_cast<UINT>(pixels.size()), pixels.data()))
        && SUCCEEDED(frame->Commit())
        && SUCCEEDED(encoder->Commit());
}

// ---------------------------------------------------------------------------
// CRTPass — the main CRT shader into rtv, reading the picture (t0), the UI
// layer (t1, may be null) and the glow / halation results (t2, t3).
// ---------------------------------------------------------------------------
void D3DRenderer::CRTPass(ID3D11RenderTargetView* rtv, int w, int h,
                          const PostResult& post, ID3D11ShaderResourceView* ui) {
    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(w);
    vp.Height   = static_cast<float>(h);
    vp.MaxDepth = 1.0f;

    m_context->OMSetRenderTargets(1, &rtv, nullptr);
    m_context->RSSetViewports(1, &vp);
    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    m_context->PSSetShader(m_ps.Get(), nullptr, 0);

    ID3D11Buffer* cb = m_cbuffer.Get();
    m_context->PSSetConstantBuffers(0, 1, &cb);
    ID3D11SamplerState* samp = m_sampler.Get();
    m_context->PSSetSamplers(0, 1, &samp);

    ID3D11ShaderResourceView* srvs[5] = { post.picture, ui, post.glow, post.halo, post.avgLum };
    m_context->PSSetShaderResources(0, 5, srvs);
    m_context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[5] = {};
    m_context->PSSetShaderResources(0, 5, nullSrvs);
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// CreateRenderTarget — texture + RTV + SRV of the given size and format.
// ---------------------------------------------------------------------------
bool D3DRenderer::CreateRenderTarget(RenderTarget& rt, int w, int h, DXGI_FORMAT format,
                                     bool withMips) {
    rt = {};
    D3D11_TEXTURE2D_DESC td = {};
    td.Width      = static_cast<UINT>(std::max(w, 1));
    td.Height     = static_cast<UINT>(std::max(h, 1));
    td.MipLevels  = withMips ? 0 : 1;   // 0 = full chain, for GenerateMips
    td.ArraySize  = 1;
    td.Format     = format;
    td.SampleDesc = { 1, 0 };
    td.Usage      = D3D11_USAGE_DEFAULT;
    td.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags  = withMips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &rt.tex)) ||
        FAILED(m_device->CreateRenderTargetView(rt.tex.Get(), nullptr, &rt.rtv)) ||
        FAILED(m_device->CreateShaderResourceView(rt.tex.Get(), nullptr, &rt.srv))) {
        rt = {};
        return false;
    }
    rt.w = static_cast<int>(td.Width);
    rt.h = static_cast<int>(td.Height);
    return true;
}

// ---------------------------------------------------------------------------
// EnsurePostChain — (re)creates a chain's targets when the source size changes.
// Glow and halation are stored as half-float linear light so bright values
// and the blur's soft tails don't band.
// ---------------------------------------------------------------------------
bool D3DRenderer::EnsurePostChain(PostChain& chain, int w, int h) {
    if (chain.srcW == w && chain.srcH == h && chain.signal.rtv) return true;
    chain = {};

    constexpr DXGI_FORMAT kHalf = DXGI_FORMAT_R16G16B16A16_FLOAT;
    const bool ok =
        CreateRenderTarget(chain.signal,     w, h, DXGI_FORMAT_R8G8B8A8_UNORM) &&
        CreateRenderTarget(chain.history[0], w, h, kHalf) &&
        CreateRenderTarget(chain.history[1], w, h, kHalf) &&
        CreateRenderTarget(chain.glow[0], (w + 3) / 4, (h + 3) / 4, kHalf) &&
        CreateRenderTarget(chain.glow[1], (w + 3) / 4, (h + 3) / 4, kHalf) &&
        CreateRenderTarget(chain.halo[0], (w + 7) / 8, (h + 7) / 8, kHalf) &&
        CreateRenderTarget(chain.halo[1], (w + 7) / 8, (h + 7) / 8, kHalf) &&
        CreateRenderTarget(chain.lumGrid, kLumSize, kLumSize, kHalf, /*withMips=*/true) &&
        CreateRenderTarget(chain.lum[0], 1, 1, kHalf) &&
        CreateRenderTarget(chain.lum[1], 1, 1, kHalf);
    if (!ok) {
        OutputDebugStringA("D3DRenderer: post chain creation failed\n");
        chain = {};
        return false;
    }
    chain.srcW = w;
    chain.srcH = h;
    return true;
}

// ---------------------------------------------------------------------------
// FullscreenPass — runs ps over all of dst, reading source as t0.
// ---------------------------------------------------------------------------
void D3DRenderer::FullscreenPass(ID3D11PixelShader* ps, const RenderTarget& dst,
                                 ID3D11ShaderResourceView* source,
                                 ID3D11ShaderResourceView* history) {
    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(dst.w);
    vp.Height   = static_cast<float>(dst.h);
    vp.MaxDepth = 1.0f;

    ID3D11RenderTargetView* rtv = dst.rtv.Get();
    m_context->OMSetRenderTargets(1, &rtv, nullptr);
    m_context->RSSetViewports(1, &vp);
    m_context->PSSetShader(ps, nullptr, 0);
    m_context->PSSetShaderResources(0, 1, &source);
    m_context->PSSetShaderResources(5, 1, &history);   // t5; null reads as zero
    m_context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSrv);
    m_context->PSSetShaderResources(5, 1, &nullSrv);
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// BlurPass — one direction of a separable Gaussian, sigma in src texels.
// PS_Blur takes 13 taps; they are spread to cover ±3 sigma, never closer
// than one texel apart.
// ---------------------------------------------------------------------------
void D3DRenderer::BlurPass(const RenderTarget& src, const RenderTarget& dst,
                           bool horizontal, float sigmaTexels) {
    const float sigma   = std::max(sigmaTexels, 0.5f);
    const float spacing = std::max(sigma * 0.5f, 1.0f);   // texels between taps
    struct { float stepX, stepY, sigmaTaps, pad; } params = {
        horizontal ? spacing / src.w : 0.f,
        horizontal ? 0.f : spacing / src.h,
        sigma / spacing,
        0.f,
    };
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_context->Map(m_blurCBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &params, sizeof(params));
        m_context->Unmap(m_blurCBuf.Get(), 0);
    }
    ID3D11Buffer* cb = m_blurCBuf.Get();
    m_context->PSSetConstantBuffers(2, 1, &cb);
    FullscreenPass(m_blurPS.Get(), dst, src.srv.Get());
}

// ---------------------------------------------------------------------------
// RunPostChain — runs the enabled pre-passes on source (w × h) and returns
// what the CRT pass should bind.  Disabled stages cost nothing.
// ---------------------------------------------------------------------------
D3DRenderer::PostResult D3DRenderer::RunPostChain(PostChain& chain,
        ID3D11ShaderResourceView* source, int w, int h, const CRTSettings& s) {
    PostResult r;
    r.picture = source;

    const bool composite = s.chromaBleed > 0.001f || s.signalSoftness > 0.001f;
    const bool persist   = s.persistence > 0.001f;
    const bool raster    = s.rasterBloom > 0.001f;
    const bool glow      = s.glowStrength > 0.001f;
    const bool halo      = s.diffuseStrength > 0.001f;

    // A stage that is off forgets its history, so turning it back on later
    // doesn't flash a stale frame.
    if (!persist) chain.historyValid = false;
    if (!raster)  chain.lumValid     = false;

    if (!composite && !persist && !raster && !glow && !halo) return r;
    if (!EnsurePostChain(chain, w, h)) return r;

    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    ID3D11Buffer* cb = m_cbuffer.Get();
    m_context->PSSetConstantBuffers(0, 1, &cb);
    ID3D11SamplerState* samp = m_sampler.Get();
    m_context->PSSetSamplers(0, 1, &samp);

    // Signal stage, then display stages (phosphor persistence; everything
    // after it — glow, halation, raster bloom — sees the trails too).
    if (composite) {
        FullscreenPass(m_compositePS.Get(), chain.signal, source);
        r.picture = chain.signal.srv.Get();
    }

    if (persist) {
        RenderTarget&             dst  = chain.history[chain.historyCur];
        ID3D11ShaderResourceView* prev = chain.historyValid
            ? chain.history[chain.historyCur ^ 1].srv.Get() : nullptr;
        FullscreenPass(m_persistPS.Get(), dst, r.picture, prev);
        r.picture          = dst.srv.Get();
        chain.historyCur  ^= 1;
        chain.historyValid = true;
    }

    if (raster) {
        FullscreenPass(m_lumDownPS.Get(), chain.lumGrid, r.picture);
        m_context->GenerateMips(chain.lumGrid.srv.Get());

        RenderTarget&             dst  = chain.lum[chain.lumCur];
        ID3D11ShaderResourceView* prev = chain.lumValid
            ? chain.lum[chain.lumCur ^ 1].srv.Get() : nullptr;
        FullscreenPass(m_lumSmoothPS.Get(), dst, chain.lumGrid.srv.Get(), prev);
        r.avgLum       = dst.srv.Get();
        chain.lumCur  ^= 1;
        chain.lumValid = true;
    }

    if (glow || halo) {
        // Radii are set in pixels at 1080p; scale to this picture's height.
        const float pxScale = static_cast<float>(h) / 1080.f;

        FullscreenPass(m_glowDownPS.Get(), chain.glow[0], r.picture);

        // Halation starts from the unblurred 1/4-size source.
        if (halo) {
            FullscreenPass(m_haloDownPS.Get(), chain.halo[0], chain.glow[0].srv.Get());
            const float sigma = s.halationRadius * pxScale / 8.f;
            BlurPass(chain.halo[0], chain.halo[1], true,  sigma);
            BlurPass(chain.halo[1], chain.halo[0], false, sigma);
            r.halo = chain.halo[0].srv.Get();
        }
        if (glow) {
            const float sigma = s.glowRadius * pxScale / 4.f;
            BlurPass(chain.glow[0], chain.glow[1], true,  sigma);
            BlurPass(chain.glow[1], chain.glow[0], false, sigma);
            r.glow = chain.glow[0].srv.Get();
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// DrawSprite — alpha-blends a textured quad into the currently bound RT, with
// texture pixel (hotX, hotY) placed at window pixel (x, y).
// ---------------------------------------------------------------------------
void D3DRenderer::DrawSprite(const Sprite& sp) {
    if (!sp.srv || sp.texW <= 0 || sp.texH <= 0 || !m_spriteVS || !m_spriteCBuf) return;

    const float left   = sp.x - sp.hotX * sp.scale;
    const float top    = sp.y - sp.hotY * sp.scale;
    const float right  = left + sp.texW * sp.scale;
    const float bottom = top  + sp.texH * sp.scale;
    // Convert pixel coords to NDC (x: [-1,1] right; y: [-1,1] up).
    const float W = static_cast<float>(m_width);
    const float H = static_cast<float>(m_height);
    struct SpriteCB { float tlX, tlY, brX, brY; } scb;
    scb.tlX =  left  / W * 2.f - 1.f;
    scb.tlY = -(top    / H * 2.f - 1.f); // invert Y: screen-down → NDC-up
    scb.brX =  right  / W * 2.f - 1.f;
    scb.brY = -(bottom / H * 2.f - 1.f);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_context->Map(m_spriteCBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &scb, sizeof(scb));
        m_context->Unmap(m_spriteCBuf.Get(), 0);
    }

    const float kBlendFactor[4] = {};
    m_context->OMSetBlendState(m_alphaBlendState.Get(), kBlendFactor, 0xFFFFFFFF);
    m_context->VSSetShader(m_spriteVS.Get(), nullptr, 0);
    ID3D11Buffer* scbBuf = m_spriteCBuf.Get();
    m_context->VSSetConstantBuffers(1, 1, &scbBuf);
    m_context->PSSetShader(m_blitPS.Get(), nullptr, 0);
    ID3D11SamplerState* samp = m_sampler.Get();
    m_context->PSSetSamplers(0, 1, &samp);
    ID3D11ShaderResourceView* srv = sp.srv;
    m_context->PSSetShaderResources(0, 1, &srv);
    m_context->Draw(6, 0);

    // Restore state.
    ID3D11ShaderResourceView* nullSRV = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSRV);
    m_context->OMSetBlendState(nullptr, kBlendFactor, 0xFFFFFFFF);
}

// ---------------------------------------------------------------------------
// RenderPassthrough
// ---------------------------------------------------------------------------
void D3DRenderer::RenderPassthrough(ID3D11Texture2D* tex) {
    if (!m_rtv) return;
    const float clear[] = { 0.f, 0.f, 0.f, 0.f };
    m_context->ClearRenderTargetView(m_rtv.Get(), clear);

    if (tex) {
        if (ID3D11ShaderResourceView* srv = GetOrBuildCaptureSRV(tex)) {
            D3D11_VIEWPORT vp = {};
            vp.Width    = static_cast<float>(m_width);
            vp.Height   = static_cast<float>(m_height);
            vp.MaxDepth = 1.f;

            ID3D11RenderTargetView* rtv = m_rtv.Get();
            m_context->OMSetRenderTargets(1, &rtv, nullptr);
            m_context->RSSetViewports(1, &vp);
            m_context->VSSetShader(m_vs.Get(), nullptr, 0);
            m_context->PSSetShader(m_blitPS.Get(), nullptr, 0);

            ID3D11SamplerState* samp = m_sampler.Get();
            m_context->PSSetSamplers(0, 1, &samp);
            m_context->PSSetShaderResources(0, 1, &srv);
            m_context->Draw(3, 0);

            ID3D11ShaderResourceView* nullSrv = nullptr;
            m_context->PSSetShaderResources(0, 1, &nullSrv);
        }
    }

    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_swapChain->Present(1, 0);
}

// Resize
// ---------------------------------------------------------------------------
void D3DRenderer::Resize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (w == m_width && h == m_height && m_rtv && m_intermediateRTV) {
        m_pendingWidth = m_pendingHeight = 0;
        return;
    }

    m_rtv.Reset();
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_context->Flush();

    HRESULT hr = m_swapChain->ResizeBuffers(
        0,
        static_cast<UINT>(w),
        static_cast<UINT>(h),
        DXGI_FORMAT_UNKNOWN,
        kSwapChainFlags); // must match creation flags

    if (FAILED(hr)) {
        OutputDebugStringA("D3DRenderer: ResizeBuffers failed — will retry\n");
        // The buffers keep their old size; re-acquire a view of them so we
        // keep rendering, and leave m_width/m_height untouched so they still
        // describe the real buffers.  RenderFrame retries the resize.
        m_pendingWidth  = w;
        m_pendingHeight = h;
        CreateBackBufferRTV();
        return;
    }

    m_width  = w;
    m_height = h;
    m_pendingWidth = m_pendingHeight = 0;

    if (!CreateBackBufferRTV() || !CreateIntermediateRT()) {
        // Leave a retry pending so a transient failure doesn't stick.
        m_pendingWidth  = w;
        m_pendingHeight = h;
    }
}

// ---------------------------------------------------------------------------
// CreateBackBufferRTV — (re)creates m_rtv for swap chain buffer 0.
// ---------------------------------------------------------------------------
bool D3DRenderer::CreateBackBufferRTV() {
    m_rtv.Reset();
    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) ||
        FAILED(m_device->CreateRenderTargetView(backbuffer.Get(), nullptr, &m_rtv))) {
        OutputDebugStringA("D3DRenderer: CreateBackBufferRTV failed\n");
        m_rtv.Reset();
        return false;
    }
    return true;
}
