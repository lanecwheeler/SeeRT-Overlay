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
    // sprites: textured quads (cursor, click-point marker) composited into the
    //          intermediate RT after ImGui, in order.
    struct Sprite {
        ID3D11ShaderResourceView* srv = nullptr;
        float x = 0.f, y = 0.f;        // window pixels
        int   texW = 0, texH = 0;
        float scale = 1.f;
        float hotX = 0.f, hotY = 0.f;  // texture pixel placed at (x, y)
    };
    void RenderFrame(ID3D11Texture2D* capturedTex, const CRTSettings& settings,
                     ID3D11Texture2D*           streamTex    = nullptr,
                     ImDrawData*                imguiDrawData = nullptr,
                     const Sprite*              sprites      = nullptr,
                     int                        spriteCount  = 0);

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
    struct RenderTarget {
        ComPtr<ID3D11Texture2D>          tex;
        ComPtr<ID3D11RenderTargetView>   rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
        int w = 0, h = 0;
    };

    // Pre-passes run on a picture before the CRT pass: composite signal
    // filtering, and the glow / halation blur pyramids.  One chain per
    // picture source (overlay and stream), sized to that source.
    struct PostChain {
        RenderTarget signal;   // composite-filtered picture, full size (signal space)
        RenderTarget glow[2];  // 1/4 size, linear light — [0] holds the result
        RenderTarget halo[2];  // 1/8 size, linear light — [0] holds the result
        int srcW = 0, srcH = 0;
    };
    // SRVs the CRT pass binds to t0, t2 and t3 (glow / halo null when off).
    struct PostResult {
        ID3D11ShaderResourceView* picture = nullptr;
        ID3D11ShaderResourceView* glow    = nullptr;
        ID3D11ShaderResourceView* halo    = nullptr;
    };

    bool LoadShaders();
    bool CreateBackBufferRTV();
    bool CreateIntermediateRT();
    bool CreateRenderTarget(RenderTarget& rt, int w, int h, DXGI_FORMAT format);
    bool EnsurePostChain(PostChain& chain, int w, int h);
    PostResult RunPostChain(PostChain& chain, ID3D11ShaderResourceView* source,
                            int w, int h, const CRTSettings& settings);
    void FullscreenPass(ID3D11PixelShader* ps, const RenderTarget& dst,
                        ID3D11ShaderResourceView* source);
    void BlurPass(const RenderTarget& src, const RenderTarget& dst,
                  bool horizontal, float sigmaTexels);
    void CRTPass(ID3D11RenderTargetView* rtv, int w, int h, const PostResult& post,
                 ID3D11ShaderResourceView* ui);
    void UpdateConstantBuffer(const CRTSettings& settings);
    void DrawSprite(const Sprite& sprite);

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

    // UI layer — ImGui + cursor sprites (premultiplied alpha), composited by
    // the CRT pass on top of the picture.
    ComPtr<ID3D11Texture2D>            m_uiTex;
    ComPtr<ID3D11RenderTargetView>     m_uiRTV;
    ComPtr<ID3D11ShaderResourceView>   m_uiSRV;

    // Stream window swap chain (optional)
    ComPtr<IDXGISwapChain1>            m_streamSwapChain;
    ComPtr<ID3D11RenderTargetView>     m_streamRtv;
    int m_streamWidth  = 1280;
    int m_streamHeight = 720;

    ComPtr<ID3D11VertexShader>         m_vs;
    ComPtr<ID3D11VertexShader>         m_spriteVS; // positioned quad for cursor
    ComPtr<ID3D11PixelShader>          m_ps;       // CRT effects PS
    ComPtr<ID3D11PixelShader>          m_blitPS;   // pass-through PS
    ComPtr<ID3D11PixelShader>          m_compositePS; // composite signal filter
    ComPtr<ID3D11PixelShader>          m_glowDownPS;  // linearise + 1/4 downsample
    ComPtr<ID3D11PixelShader>          m_haloDownPS;  // threshold + 1/8 downsample
    ComPtr<ID3D11PixelShader>          m_blurPS;      // separable Gaussian blur

    PostChain                          m_mainPost;   // overlay picture
    PostChain                          m_streamPost; // stream picture

    ComPtr<ID3D11Buffer>               m_cbuffer;
    ComPtr<ID3D11Buffer>               m_spriteCBuf; // SpriteParams b1
    ComPtr<ID3D11Buffer>               m_blurCBuf;   // BlurParams b2
    ComPtr<ID3D11SamplerState>         m_sampler;
    ComPtr<ID3D11RasterizerState>      m_rsState;
    ComPtr<ID3D11BlendState>           m_alphaBlendState;

    int m_width  = 1280;
    int m_height = 720;

    // Non-zero when a Resize() failed — RenderFrame retries it each frame.
    int m_pendingWidth  = 0;
    int m_pendingHeight = 0;

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
