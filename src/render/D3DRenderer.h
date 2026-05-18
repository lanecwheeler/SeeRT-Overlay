#pragma once
#include <d3d11.h>
#include <dxgi1_3.h>      // IDXGISwapChain2 — waitable swap chain
#include <dcomp.h>        // DirectComposition — lets the overlay be WS_EX_LAYERED
#include <wrl/client.h>   // Microsoft::WRL::ComPtr
#include "../settings/Settings.h"
#include <array>

// Forward-declare so D3DRenderer.h doesn't drag in all of ImGui.
struct ImDrawData;

using Microsoft::WRL::ComPtr;

class D3DRenderer {
public:
    D3DRenderer() = default;
    ~D3DRenderer();

    // Non-copyable.
    D3DRenderer(const D3DRenderer&)            = delete;
    D3DRenderer& operator=(const D3DRenderer&) = delete;

    // Initialise from an already-created device + context.
    // Creates the DComp swap chain for hwnd and loads/compiles shaders.
    // Returns false on failure.
    bool Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx);

    // Render one frame:
    //   Pass 1 : blit capturedTex → intermediate RT (+ optional cursor sprite)
    //   Stream  : CRT of streamTex → stream swap chain (if active)
    //   Pass 2 : CRT of intermediate → back buffer
    //   ImGui   : settings panel drawn on top of CRT (if imguiDrawData != null)
    //   Present
    // capturedTex / streamTex may each be null (renders black for that output).
    // cursorSRV: if non-null, a cursor sprite is composited into the intermediate RT
    //            at (cursorX, cursorY) in window pixels at the given scale.
    void RenderFrame(ID3D11Texture2D* capturedTex, const CRTSettings& settings,
                     ID3D11Texture2D*           streamTex    = nullptr,
                     ImDrawData*                imguiDrawData = nullptr,
                     ID3D11ShaderResourceView*  cursorSRV    = nullptr,
                     float cursorX = 0.f, float cursorY = 0.f,
                     int   cursorTexW = 0, int cursorTexH = 0,
                     float cursorScale = 1.f);

    // Resize the main swap chain buffers (call from WM_SIZE on overlay window).
    void Resize(int w, int h);

    // Blit capturedTex directly to the back buffer with no CRT effects (pause mode).
    // Pass nullptr to present a transparent frame.
    void RenderPassthrough(ID3D11Texture2D* capturedTex);

    // -----------------------------------------------------------------------
    // Stream window swap chain — optional, created on demand.
    // -----------------------------------------------------------------------

    bool InitStream(HWND streamHwnd);
    void ShutdownStream();
    void ResizeStream(int w, int h);

    bool HasStream() const { return m_streamSwapChain != nullptr; }

    // Accessors so other systems (e.g. ImGui) can use the same device.
    ID3D11Device*        GetDevice()  const { return m_device; }
    ID3D11DeviceContext* GetContext() const { return m_context; }

    // Overlay dimensions (updated by Resize).
    int GetWidth()  const { return m_width; }
    int GetHeight() const { return m_height; }

    // DXGI waitable-swap-chain handle.  Main loop waits on this before each
    // Tick() so the CPU sleeps until DXGI is ready for the next frame rather
    // than busy-polling.  Null until Init() succeeds.
    HANDLE GetFrameLatencyHandle() const { return m_frameLatencyHandle; }

private:
    bool LoadShaders();
    bool CreateIntermediateRT();
    void UpdateConstantBuffer(const CRTSettings& settings);

    ComPtr<ID3D11ShaderResourceView> BuildCaptureSRV(ID3D11Texture2D* tex);
    ID3D11ShaderResourceView*        GetOrBuildCaptureSRV(ID3D11Texture2D* tex);

    // Owned externally — we hold a raw pointer, not a ref-counted one.
    ID3D11Device*        m_device  = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    // Main (overlay) swap chain — attached via DirectComposition, not directly
    // to the HWND, so the HWND can have WS_EX_LAYERED | WS_EX_TRANSPARENT.
    ComPtr<IDXGISwapChain1>            m_swapChain;
    ComPtr<ID3D11RenderTargetView>     m_rtv;

    // DirectComposition objects for the overlay window.
    ComPtr<IDCompositionDevice>        m_dcompDevice;
    ComPtr<IDCompositionTarget>        m_dcompTarget;
    ComPtr<IDCompositionVisual>        m_dcompVisual;

    // Intermediate RT — game frame composited here before CRT pass.
    ComPtr<ID3D11Texture2D>            m_intermediateTex;
    ComPtr<ID3D11RenderTargetView>     m_intermediateRTV;
    ComPtr<ID3D11ShaderResourceView>   m_intermediateSRV;

    // Stream window swap chain (optional)
    ComPtr<IDXGISwapChain1>            m_streamSwapChain;
    ComPtr<ID3D11RenderTargetView>     m_streamRtv;
    int m_streamWidth  = 1280;
    int m_streamHeight = 720;

    ComPtr<ID3D11VertexShader>         m_vs;
    ComPtr<ID3D11VertexShader>         m_spriteVS; // positioned quad for cursor
    ComPtr<ID3D11PixelShader>          m_ps;       // CRT effects PS
    ComPtr<ID3D11PixelShader>          m_blitPS;   // pass-through PS

    ComPtr<ID3D11Buffer>               m_cbuffer;
    ComPtr<ID3D11Buffer>               m_spriteCBuf; // SpriteParams b1
    ComPtr<ID3D11SamplerState>         m_sampler;
    ComPtr<ID3D11RasterizerState>      m_rsState;
    ComPtr<ID3D11BlendState>           m_alphaBlendState;

    int m_width  = 1280;
    int m_height = 720;

    // Waitable swap chain — reduces frame queue depth to 1, cutting display
    // latency from ~3 frames (DXGI default) to ~1 frame.
    HANDLE m_frameLatencyHandle = nullptr;

    // SRV cache — WGC frame pools reuse the same small set of D3D11 textures
    // in round-robin order (pool size = 2).  Caching by raw pointer avoids
    // calling CreateShaderResourceView on every rendered frame.
    struct SRVEntry {
        ID3D11Texture2D*                  tex = nullptr;
        ComPtr<ID3D11ShaderResourceView>  srv;
    };
    std::array<SRVEntry, 4> m_srvCache{};  // 4 slots — covers any pool size
    int m_srvCacheNext = 0;               // round-robin eviction index
};
