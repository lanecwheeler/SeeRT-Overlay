#include "D3DRenderer.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <stdexcept>
#include <string>
#include <filesystem>
#include <cstddef>    // offsetof
#include <Windows.h>

// Bytes of CRTSettings that are actually uploaded to the GPU constant buffer.
// Everything from menuStyle onward is CPU-only and must not be included.
static constexpr UINT kShaderDataSize =
    static_cast<UINT>(offsetof(CRTSettings, menuStyle));

// Swap chain flags used at creation and during ResizeBuffers (they must match).
static constexpr UINT kSwapChainFlags =
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dcomp.lib")

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void HR(HRESULT hr, const char* msg) {
    if (FAILED(hr)) {
        OutputDebugStringA(msg);
        OutputDebugStringA("\n");
        DebugBreak();
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
//   Pass 1  : blit capturedTex → intermediate RT     (format conversion)
//   Stream  : CRT of streamTex → stream swap chain   (if active)
//   Pass 2  : CRT of intermediate → back buffer
//   ImGui   : settings panel on top of CRT           (if imguiDrawData != null)
//   Present
// ---------------------------------------------------------------------------
void D3DRenderer::RenderFrame(ID3D11Texture2D* capturedTex, const CRTSettings& settings,
                              ID3D11Texture2D* streamTex, ImDrawData* imguiDrawData,
                              ID3D11ShaderResourceView* cursorSRV,
                              float cursorX, float cursorY,
                              int cursorTexW, int cursorTexH, float cursorScale) {
    const float black[4] = { 0.f, 0.f, 0.f, 1.f };

    D3D11_VIEWPORT mainVP = {};
    mainVP.Width    = static_cast<float>(m_width);
    mainVP.Height   = static_cast<float>(m_height);
    mainVP.MaxDepth = 1.0f;

    // Update constant buffer once — shared by all CRT passes this frame.
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

    // -----------------------------------------------------------------------
    // STREAM PASS — CRT of streamTex → stream swap chain (no UI overlay).
    // Runs before ImGui is composited onto the intermediate so the stream
    // output is always a clean, UI-free CRT image.
    // -----------------------------------------------------------------------
    if (m_streamSwapChain && m_streamRtv && streamTex) {
        if (ID3D11ShaderResourceView* streamSRV = GetOrBuildCaptureSRV(streamTex)) {
            // Unbind intermediate RTV — it will become SRV... actually here we
            // read streamTex (not the intermediate), so no conflict. But we do
            // need to switch the active RTV to the stream back buffer.
            m_context->OMSetRenderTargets(0, nullptr, nullptr);

            m_context->ClearRenderTargetView(m_streamRtv.Get(), black);
            {
                ID3D11RenderTargetView* rtv = m_streamRtv.Get();
                m_context->OMSetRenderTargets(1, &rtv, nullptr);
            }

            D3D11_VIEWPORT streamVP = {};
            streamVP.Width    = static_cast<float>(m_streamWidth);
            streamVP.Height   = static_cast<float>(m_streamHeight);
            streamVP.MaxDepth = 1.0f;
            m_context->RSSetViewports(1, &streamVP);

            m_context->VSSetShader(m_vs.Get(), nullptr, 0);
            m_context->PSSetShader(m_ps.Get(), nullptr, 0);

            ID3D11Buffer* cb = m_cbuffer.Get();
            m_context->PSSetConstantBuffers(0, 1, &cb);

            ID3D11SamplerState* samp = m_sampler.Get();
            m_context->PSSetSamplers(0, 1, &samp);

            m_context->PSSetShaderResources(0, 1, &streamSRV);

            m_context->Draw(3, 0);

            ID3D11ShaderResourceView* nullSrv = nullptr;
            m_context->PSSetShaderResources(0, 1, &nullSrv);
            m_context->OMSetRenderTargets(0, nullptr, nullptr);

            m_streamSwapChain->Present(0, 0); // no vsync — driven by main Present

            // Restore intermediate RTV and main viewport for ImGui.
            {
                ID3D11RenderTargetView* rtv = m_intermediateRTV.Get();
                m_context->OMSetRenderTargets(1, &rtv, nullptr);
            }
            m_context->RSSetViewports(1, &mainVP);
        }
    }

    // Settings panel — rendered into the intermediate RT so the CRT pass
    // applies the filter (curvature, scanlines, etc.) to the UI as well.
    if (imguiDrawData)
        ImGui_ImplDX11_RenderDrawData(imguiDrawData);

    // Cursor sprite — drawn into the intermediate RT so it is CRT-filtered.
    // Used by secondary overlays that have no ImGui context.
    if (cursorSRV && cursorTexW > 0 && cursorTexH > 0 && m_spriteVS && m_spriteCBuf) {
        // Hotpoint is at pixel (1,1) of the texture — subtract it so the tip
        // sits exactly at (cursorX, cursorY).
        const float hotX = 1.f * cursorScale;
        const float hotY = 1.f * cursorScale;
        const float left = cursorX - hotX;
        const float top  = cursorY - hotY;
        const float right  = left + cursorTexW * cursorScale;
        const float bottom = top  + cursorTexH * cursorScale;
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
        m_context->PSSetShaderResources(0, 1, &cursorSRV);
        m_context->Draw(6, 0);

        // Restore state.
        ID3D11ShaderResourceView* nullSRV = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSRV);
        m_context->OMSetBlendState(nullptr, kBlendFactor, 0xFFFFFFFF);
    }

    // Unbind the intermediate RTV so it can be read as SRV in pass 2.
    m_context->OMSetRenderTargets(0, nullptr, nullptr);

    // -----------------------------------------------------------------------
    // PASS 2 — CRT shader on intermediate RT → main back buffer → Present.
    // -----------------------------------------------------------------------
    m_context->ClearRenderTargetView(m_rtv.Get(), black);
    {
        ID3D11RenderTargetView* rtv = m_rtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, nullptr);
    }
    m_context->RSSetViewports(1, &mainVP);

    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    m_context->PSSetShader(m_ps.Get(), nullptr, 0);

    ID3D11Buffer* cb = m_cbuffer.Get();
    m_context->PSSetConstantBuffers(0, 1, &cb);

    ID3D11SamplerState* samp = m_sampler.Get();
    m_context->PSSetSamplers(0, 1, &samp);

    ID3D11ShaderResourceView* intermediateSRV = m_intermediateSRV.Get();
    m_context->PSSetShaderResources(0, 1, &intermediateSRV);

    m_context->Draw(3, 0);

    {
        ID3D11ShaderResourceView* nullSrv = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSrv);
    }

    m_swapChain->Present(1, 0); // vsync on main
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
    if (w == m_width && h == m_height) return;

    m_width  = w;
    m_height = h;

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
        OutputDebugStringA("D3DRenderer: ResizeBuffers failed\n");
        return;
    }

    ComPtr<ID3D11Texture2D> backbuffer;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    m_device->CreateRenderTargetView(backbuffer.Get(), nullptr, &m_rtv);

    CreateIntermediateRT();
}
