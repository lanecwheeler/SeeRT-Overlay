#define OEMRESOURCE  // expose OCR_* cursor identifiers for SetSystemCursor
#include "App.h"

#include <imgui.h>
#include "ui/StyleFonts.h"
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <d3d11.h>
#include <stdexcept>
#include <algorithm>  // std::clamp

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib") // WIC — PNG loading
#pragma comment(lib, "ole32.lib")

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#pragma comment(lib, "propsys.lib")

#include <dxgi1_6.h>    // IDXGIFactory6 / DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
#include <wincodec.h>   // IWICImagingFactory
#include <filesystem>
#include <vector>

// WDA_EXCLUDEFROMCAPTURE: exclude our overlay window from all WGC captures
// so capturing the monitor our window is on doesn't produce a feedback loop.
// Available since Windows 10 version 2004 (build 19041).
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// ---------------------------------------------------------------------------
// HideSystemCursors — replaces the four most common system cursor images with
// a 32×32 fully-transparent cursor so no system cursor is visible anywhere.
// Call SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, 0) to restore.
// ---------------------------------------------------------------------------
static void HideSystemCursors() {
    BYTE andMask[128], xorMask[128];
    memset(andMask, 0xFF, sizeof(andMask));
    memset(xorMask, 0x00, sizeof(xorMask));
    auto blankCursor = [&]() {
        return CreateCursor(GetModuleHandleW(nullptr), 0, 0, 32, 32, andMask, xorMask);
    };
    SetSystemCursor(blankCursor(), OCR_NORMAL);
    SetSystemCursor(blankCursor(), OCR_HAND);
    SetSystemCursor(blankCursor(), OCR_IBEAM);
    SetSystemCursor(blankCursor(), OCR_SIZEALL);
}

// ---------------------------------------------------------------------------
// Style functions
// ---------------------------------------------------------------------------

static void ApplyCRTOSDStyle() {
    ImGuiIO& io    = ImGui::GetIO();
    io.FontGlobalScale = 1.15f;

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 0.f;
    style.ChildRounding     = 0.f;
    style.FrameRounding     = 0.f;
    style.GrabRounding      = 0.f;
    style.ScrollbarRounding = 0.f;
    style.TabRounding       = 0.f;
    style.PopupRounding     = 0.f;
    style.WindowBorderSize  = 2.f;
    style.FrameBorderSize   = 1.f;
    style.FramePadding      = ImVec2(7.f, 4.f);
    style.ItemSpacing       = ImVec2(6.f, 9.f);
    style.ItemInnerSpacing  = ImVec2(5.f, 4.f);
    style.GrabMinSize       = 14.f;
    style.WindowPadding     = ImVec2(12.f, 10.f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]             = ImVec4(0.03f, 0.02f, 0.18f, 0.97f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.03f, 0.02f, 0.18f, 0.97f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.03f, 0.02f, 0.18f, 0.99f);
    c[ImGuiCol_Border]               = ImVec4(0.00f, 0.55f, 1.00f, 0.85f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_Text]                 = ImVec4(0.88f, 0.93f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.35f, 0.45f, 0.65f, 1.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.05f, 0.04f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.08f, 0.07f, 0.40f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.10f, 0.09f, 0.50f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.00f, 0.04f, 0.28f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.00f, 0.06f, 0.38f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.00f, 0.02f, 0.18f, 0.92f);
    c[ImGuiCol_SliderGrab]           = ImVec4(1.00f, 0.78f, 0.00f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(1.00f, 0.92f, 0.30f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.00f, 0.20f, 0.60f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.00f, 0.30f, 0.80f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(1.00f, 0.78f, 0.00f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.00f, 0.18f, 0.55f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.00f, 0.28f, 0.72f, 1.00f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.00f, 0.38f, 0.85f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.00f, 0.50f, 1.00f, 0.55f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.00f, 0.65f, 1.00f, 0.80f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.00f, 0.80f, 1.00f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.02f, 0.02f, 0.12f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.00f, 0.45f, 0.80f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.00f, 0.60f, 1.00f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(1.00f, 0.78f, 0.00f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(1.00f, 0.78f, 0.00f, 1.00f);
    c[ImGuiCol_NavHighlight]         = ImVec4(0.00f, 0.75f, 1.00f, 0.80f);
    c[ImGuiCol_NavWindowingHighlight]= ImVec4(0.00f, 0.75f, 1.00f, 0.70f);
}

static void ApplyDefaultStyle() {
    ImGui::GetIO().FontGlobalScale = 1.0f;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 6.f;
    style.ChildRounding     = 4.f;
    style.FrameRounding     = 4.f;
    style.GrabRounding      = 4.f;
    style.ScrollbarRounding = 4.f;
    style.TabRounding       = 4.f;
    style.PopupRounding     = 4.f;
    style.WindowBorderSize  = 1.f;
    style.FrameBorderSize   = 0.f;
    style.FramePadding      = ImVec2(4.f, 3.f);
    style.ItemSpacing       = ImVec2(8.f, 4.f);
    style.ItemInnerSpacing  = ImVec2(4.f, 4.f);
    style.GrabMinSize       = 10.f;
    style.WindowPadding     = ImVec2(8.f, 8.f);
}

// ---------------------------------------------------------------------------
// LoadCursorTexture — decodes a PNG file using WIC and uploads it as an
// immutable D3D11 BGRA texture.  Returns the SRV; outW/outH receive the
// image dimensions.  Returns nullptr on any failure (cursor falls back to
// the built-in polygon shape).
// ---------------------------------------------------------------------------
static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
LoadCursorTexture(ID3D11Device* device, const wchar_t* path, int& outW, int& outH)
{
    using namespace Microsoft::WRL;

    outW = outH = 0;

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder)))
        return nullptr;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;

    // Convert to 32-bit BGRA so D3D11 can consume it directly.
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return nullptr;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom)))
        return nullptr;

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (w == 0 || h == 0) return nullptr;

    std::vector<BYTE> pixels(static_cast<size_t>(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4,
            static_cast<UINT>(pixels.size()), pixels.data())))
        return nullptr;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width          = w;
    td.Height         = h;
    td.MipLevels      = 1;
    td.ArraySize      = 1;
    td.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc     = { 1, 0 };
    td.Usage          = D3D11_USAGE_IMMUTABLE;
    td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA srd = {};
    srd.pSysMem      = pixels.data();
    srd.SysMemPitch  = w * 4;

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&td, &srd, &tex))) return nullptr;

    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
        return nullptr;

    outW = static_cast<int>(w);
    outH = static_cast<int>(h);
    return srv;
}

// ---------------------------------------------------------------------------
// DrawVirtualCursor — renders the custom cursor PNG at the barrel-distorted
// intermediate-RT position `p` so it appears at the real screen position
// after the CRT pass.
//
// If the texture failed to load, falls back to a simple white triangle.
// The hot-point of the PNG is at pixel (1, 1) — subtracted from `p` so the
// arrow tip sits exactly at the cursor position.
// ---------------------------------------------------------------------------

// Adds an image at position `center` with the given scale, keeping the PNG's
// hot-point (pixel 1,1) pinned to `center`.
static void AddCursorImage(ImDrawList* dl, ImVec2 center,
    ImTextureID tex, int texW, int texH, float scale, ImU32 tint)
{
    const ImVec2 tl(center.x - 1.f * scale, center.y - 1.f * scale);
    const ImVec2 br(tl.x + texW * scale,    tl.y + texH * scale);
    dl->AddImage(tex, tl, br, ImVec2(0, 0), ImVec2(1, 1), tint);
}

static void DrawVirtualCursor(ImDrawList* dl, ImVec2 p,
    ImTextureID cursorTex, int texW, int texH,
    float scale, float thickness, ImU32 tint,
    bool shadowEnabled)
{
    if (cursorTex) {
        // Shadow pass — same scale, offset (+5, +5) px, semi-transparent black.
        if (shadowEnabled) {
            constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 160);
            if (thickness > 0.f) {
                // 8 directions: cardinal + diagonal (diagonal scaled by 1/√2)
                constexpr float d = 0.7071f; // 1/sqrt(2)
                constexpr float dirs[8][2] = {
                    { 1.f,  0.f}, {-1.f,  0.f}, { 0.f,  1.f}, { 0.f, -1.f},
                    { d,    d  }, {-d,    d  }, { d,   -d  }, {-d,   -d  }
                };
                for (auto& dir : dirs) {
                    ImVec2 op((p.x + dir[0] * thickness) + 5.f, (p.y + dir[1] * thickness) + 5.f);
                    AddCursorImage(dl, op, cursorTex, texW, texH, scale, kShadow);
                }
            }
            AddCursorImage(dl, ImVec2(p.x + 5.f, p.y + 5.f),
                           cursorTex, texW, texH, scale, kShadow);
        }

        // Dilation passes — render the cursor at 8 evenly-spaced offsets
        // around the center.  Each copy overlaps the adjacent ones, filling
        // in the gaps between strokes and making thin lines appear bolder.
        // The hot-point stays at `p` because AddCursorImage subtracts (1*scale)
        // before placing the image; the offset only shifts the sample position.
        if (thickness > 0.f) {
            // 8 directions: cardinal + diagonal (diagonal scaled by 1/√2)
            constexpr float d = 0.7071f; // 1/sqrt(2)
            constexpr float dirs[8][2] = {
                { 1.f,  0.f}, {-1.f,  0.f}, { 0.f,  1.f}, { 0.f, -1.f},
                { d,    d  }, {-d,    d  }, { d,   -d  }, {-d,   -d  }
            };
            for (auto& dir : dirs) {
                ImVec2 op(p.x + dir[0] * thickness, p.y + dir[1] * thickness);
                AddCursorImage(dl, op, cursorTex, texW, texH, scale, tint);
            }
        }

        // Main cursor — drawn on top so it stays crisp.
        AddCursorImage(dl, p, cursorTex, texW, texH, scale, tint);

    } else {
        // Fallback: plain triangle (no texture).
        const float sz = 18.f * (scale / 0.1f);
        if (shadowEnabled) {
            constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 160);
            ImVec2 s0 = { p.x + 3.f,           p.y + 3.f };
            ImVec2 s1 = { p.x + 3.f,           p.y + 3.f + sz };
            ImVec2 s2 = { p.x + 3.f + sz*0.6f, p.y + 3.f + sz*0.6f };
            dl->AddTriangleFilled(s0, s1, s2, kShadow);
        }
        ImVec2 v0 = p;
        ImVec2 v1 = { p.x,           p.y + sz };
        ImVec2 v2 = { p.x + sz*0.6f, p.y + sz*0.6f };
        dl->AddTriangleFilled(v0, v1, v2, tint);
        dl->AddTriangle(v0, v1, v2, IM_COL32(0, 0, 0, 200), 1.5f);
    }
}

static void ApplyN64Style() {
    ImGui::GetIO().FontGlobalScale = 1.0f;
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 18.f;
    style.ChildRounding     = 12.f;
    style.FrameRounding     = 12.f;
    style.GrabRounding      = 12.f;
    style.ScrollbarRounding = 12.f;
    style.TabRounding       = 10.f;
    style.PopupRounding     = 16.f;
    style.WindowBorderSize  = 0.f;
    style.FrameBorderSize   = 0.f;
    style.FramePadding      = ImVec2(10.f, 7.f);
    style.ItemSpacing       = ImVec2(8.f, 10.f);
    style.ItemInnerSpacing  = ImVec2(6.f, 5.f);
    style.GrabMinSize       = 16.f;
    style.WindowPadding     = ImVec2(16.f, 14.f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]             = ImVec4(0.06f, 0.04f, 0.10f, 0.93f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.06f, 0.04f, 0.10f, 0.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.08f, 0.06f, 0.14f, 0.99f);
    c[ImGuiCol_Border]               = ImVec4(0.85f, 0.68f, 0.00f, 0.90f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_Text]                 = ImVec4(0.96f, 0.90f, 0.58f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.45f, 0.38f, 0.12f, 1.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.04f, 0.04f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.08f, 0.08f, 0.42f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.12f, 0.12f, 0.55f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.02f, 0.02f, 0.22f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.04f, 0.04f, 0.32f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.02f, 0.02f, 0.14f, 0.92f);
    c[ImGuiCol_SliderGrab]           = ImVec4(1.00f, 0.82f, 0.00f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(1.00f, 0.92f, 0.20f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.05f, 0.05f, 0.40f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.10f, 0.10f, 0.58f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.04f, 0.04f, 0.38f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.08f, 0.08f, 0.50f, 1.00f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.12f, 0.12f, 0.65f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.60f, 0.48f, 0.00f, 0.70f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.85f, 0.68f, 0.00f, 0.90f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(1.00f, 0.82f, 0.00f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.02f, 0.02f, 0.10f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.40f, 0.32f, 0.00f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.65f, 0.52f, 0.00f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(1.00f, 0.82f, 0.00f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(1.00f, 0.82f, 0.00f, 1.00f);
    c[ImGuiCol_NavHighlight]         = ImVec4(0.90f, 0.70f, 0.00f, 0.80f);
    c[ImGuiCol_NavWindowingHighlight]= ImVec4(0.90f, 0.70f, 0.00f, 0.70f);
}

static void ApplyCamcorderStyle() {
    ImGui::GetIO().FontGlobalScale = 1.0f;
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 0.f;
    style.ChildRounding     = 0.f;
    style.FrameRounding     = 0.f;
    style.GrabRounding      = 0.f;
    style.ScrollbarRounding = 0.f;
    style.TabRounding       = 0.f;
    style.PopupRounding     = 0.f;
    style.WindowBorderSize  = 1.f;
    style.FrameBorderSize   = 1.f;
    style.FramePadding      = ImVec2(5.f, 3.f);
    style.ItemSpacing       = ImVec2(5.f, 5.f);
    style.ItemInnerSpacing  = ImVec2(4.f, 3.f);
    style.GrabMinSize       = 10.f;
    style.WindowPadding     = ImVec2(8.f, 6.f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]             = ImVec4(0.04f, 0.04f, 0.06f, 0.96f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.04f, 0.04f, 0.06f, 0.96f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.06f, 0.06f, 0.08f, 0.99f);
    c[ImGuiCol_Border]               = ImVec4(0.75f, 0.12f, 0.08f, 0.75f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_Text]                 = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.08f, 0.04f, 0.04f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.14f, 0.06f, 0.06f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.04f, 0.04f, 0.06f, 0.92f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.85f, 0.15f, 0.10f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(1.00f, 0.20f, 0.12f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.38f, 0.06f, 0.04f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.60f, 0.10f, 0.06f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.90f, 0.18f, 0.10f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.20f, 0.08f, 0.08f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.35f, 0.12f, 0.10f, 1.00f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.50f, 0.15f, 0.12f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.55f, 0.10f, 0.08f, 0.70f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.75f, 0.14f, 0.10f, 0.90f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.95f, 0.20f, 0.12f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.02f, 0.02f, 0.03f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.35f, 0.06f, 0.04f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.55f, 0.10f, 0.06f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.85f, 0.15f, 0.10f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.90f, 0.18f, 0.10f, 1.00f);
    c[ImGuiCol_NavHighlight]         = ImVec4(0.90f, 0.20f, 0.12f, 0.80f);
    c[ImGuiCol_NavWindowingHighlight]= ImVec4(0.90f, 0.20f, 0.12f, 0.70f);
}

static void ApplyDOSStyle() {
    ImGui::GetIO().FontGlobalScale = 1.10f;
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 0.f;
    style.ChildRounding     = 0.f;
    style.FrameRounding     = 0.f;
    style.GrabRounding      = 0.f;
    style.ScrollbarRounding = 0.f;
    style.TabRounding       = 0.f;
    style.PopupRounding     = 0.f;
    style.WindowBorderSize  = 2.f;
    style.FrameBorderSize   = 1.f;
    style.FramePadding      = ImVec2(6.f, 3.f);
    style.ItemSpacing       = ImVec2(6.f, 6.f);
    style.ItemInnerSpacing  = ImVec2(5.f, 3.f);
    style.GrabMinSize       = 12.f;
    style.WindowPadding     = ImVec2(10.f, 8.f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.98f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.00f, 0.00f, 0.00f, 0.98f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.00f, 0.02f, 0.00f, 0.99f);
    c[ImGuiCol_Border]               = ImVec4(0.00f, 0.80f, 0.00f, 0.90f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_Text]                 = ImVec4(0.00f, 1.00f, 0.00f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.00f, 0.45f, 0.00f, 1.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.00f, 0.10f, 0.00f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.00f, 0.18f, 0.00f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.00f, 0.28f, 0.00f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.00f, 0.08f, 0.00f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.00f, 0.14f, 0.00f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.00f, 0.04f, 0.00f, 0.92f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.00f, 0.75f, 0.00f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.00f, 1.00f, 0.00f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.00f, 0.22f, 0.00f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.00f, 0.38f, 0.00f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.00f, 0.70f, 0.00f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.00f, 0.18f, 0.00f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.00f, 0.30f, 0.00f, 1.00f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.00f, 0.45f, 0.00f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.00f, 0.55f, 0.00f, 0.70f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.00f, 0.75f, 0.00f, 0.90f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.00f, 1.00f, 0.00f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.00f, 0.35f, 0.00f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.00f, 0.55f, 0.00f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.00f, 0.80f, 0.00f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.00f, 1.00f, 0.00f, 1.00f);
    c[ImGuiCol_NavHighlight]         = ImVec4(0.00f, 0.80f, 0.00f, 0.80f);
    c[ImGuiCol_NavWindowingHighlight]= ImVec4(0.00f, 0.80f, 0.00f, 0.70f);
}

static void ApplyVCRStyle() {
    ImGui::GetIO().FontGlobalScale = 1.15f;
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 0.f;
    style.ChildRounding     = 0.f;
    style.FrameRounding     = 0.f;
    style.GrabRounding      = 0.f;
    style.ScrollbarRounding = 0.f;
    style.TabRounding       = 0.f;
    style.PopupRounding     = 0.f;
    style.WindowBorderSize  = 2.f;
    style.FrameBorderSize   = 1.f;
    style.FramePadding      = ImVec2(7.f, 4.f);
    style.ItemSpacing       = ImVec2(6.f, 9.f);
    style.ItemInnerSpacing  = ImVec2(5.f, 4.f);
    style.GrabMinSize       = 14.f;
    style.WindowPadding     = ImVec2(12.f, 10.f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]             = ImVec4(0.02f, 0.02f, 0.02f, 0.97f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.02f, 0.02f, 0.02f, 0.97f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.04f, 0.04f, 0.04f, 0.99f);
    c[ImGuiCol_Border]               = ImVec4(0.55f, 0.55f, 0.55f, 0.65f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_Text]                 = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.02f, 0.02f, 0.02f, 0.92f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.65f, 0.65f, 0.65f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.38f, 0.38f, 0.38f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.45f, 0.45f, 0.45f, 0.65f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.65f, 0.65f, 0.65f, 0.90f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_NavHighlight]         = ImVec4(0.80f, 0.80f, 0.80f, 0.80f);
    c[ImGuiCol_NavWindowingHighlight]= ImVec4(0.80f, 0.80f, 0.80f, 0.70f);
}

// ---------------------------------------------------------------------------
// Shake-button layout constants — shared between BeginImGuiFrame (region) and
// Tick (ImGui window draw) so both always use identical geometry.
static constexpr float kShakeBtnW = 105.f;
static constexpr float kShakeBtnH =  39.f;

// ---------------------------------------------------------------------------

bool App::IsShakeInteractive() const {
    return m_shakeInteractive && !m_settings.menuVisible && m_cursorShowTimer > 0.f;
}

bool App::IsInShakeButtonRect(POINT pt) const {
    const float W  = static_cast<float>(m_renderer.GetWidth());
    const float bx = (W - kShakeBtnW) * 0.5f;
    const float by = 20.f;
    return pt.x >= static_cast<LONG>(bx)
        && pt.x <= static_cast<LONG>(bx + kShakeBtnW)
        && pt.y >= static_cast<LONG>(by)
        && pt.y <= static_cast<LONG>(by + kShakeBtnH);
}

App::App() = default;
App::~App() { Shutdown(); }

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool App::Init(HWND hwnd) {
    m_outputHwnd   = hwnd;
    m_settingsPath = Settings::DefaultPath();
    m_settings     = Settings::Load(m_settingsPath);
    m_settings.menuVisible = false; // always start closed

    m_presetsPath    = Presets::DefaultPath();
    Presets::Load(m_presetsPath, m_userPresets, m_presetOrder);
    if (m_presetOrder.empty())
        m_presetOrder = Presets::DefaultOrder(m_userPresets);
    RebuildOrderedPresets();
    RefreshAudioDevices();
    m_trayIcon.Init(hwnd);

    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // -----------------------------------------------------------------------
    // Adapter selection: prefer the high-performance (dedicated) GPU.
    //
    // IDXGIFactory6::EnumAdapterByGpuPreference returns adapters sorted by
    // Windows' performance ranking — index 0 is the dGPU on laptops, the
    // primary GPU on desktops.  This complements the NvOptimus / AMD
    // PowerXpress driver hints in main.cpp which operate before DXGI init.
    //
    // When passing an explicit adapter, D3D_DRIVER_TYPE must be UNKNOWN.
    // All fallback paths end up on D3D_DRIVER_TYPE_HARDWARE with the default
    // adapter so we never block initialisation on a single-GPU machine.
    // -----------------------------------------------------------------------
    Microsoft::WRL::ComPtr<IDXGIAdapter1> preferredAdapter;
    {
        Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory6)))) {
            if (FAILED(factory6->EnumAdapterByGpuPreference(
                    0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    IID_PPV_ARGS(&preferredAdapter))))
                preferredAdapter = nullptr;
        }

        if (preferredAdapter) {
            DXGI_ADAPTER_DESC1 desc{};
            preferredAdapter->GetDesc1(&desc);
            // Log the selected GPU name to the debug output so it's visible in
            // a debugger or DebugView without adding UI complexity.
            char narrow[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                narrow, static_cast<int>(sizeof(narrow)), nullptr, nullptr);
            OutputDebugStringA("App: GPU selected — ");
            OutputDebugStringA(narrow);
            OutputDebugStringA("\n");
        }
    }

    const D3D_DRIVER_TYPE driverType =
        preferredAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;

    D3D_FEATURE_LEVEL featureLevel = {};
    HRESULT hr = D3D11CreateDevice(
        preferredAdapter.Get(), driverType, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION,
        &m_device, &featureLevel, &m_context);

    // Retry without the debug layer (not installed on all machines).
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            preferredAdapter.Get(), driverType, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION,
            &m_device, &featureLevel, &m_context);
    }

    // Final fallback: let DXGI pick the default adapter (single-GPU machines,
    // or if IDXGIFactory6 returned an adapter the driver rejected).
    if (FAILED(hr)) {
        OutputDebugStringA("App: preferred adapter failed, falling back to default\n");
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION,
            &m_device, &featureLevel, &m_context);
    }

    if (FAILED(hr)) { OutputDebugStringA("App: D3D11CreateDevice failed\n"); return false; }

    if (!m_renderer.Init(hwnd, m_device.Get(), m_context.Get())) {
        OutputDebugStringA("App: D3DRenderer::Init failed\n"); return false;
    }

    MakeChildrenTransparent();

    if (!m_capture.Init(m_device.Get())) {
        OutputDebugStringA("App: WGCCapture::Init failed\n"); return false;
    }

    if (!m_streamCapture.Init(m_device.Get()))
        OutputDebugStringA("App: stream WGCCapture::Init failed (stream unavailable)\n");

    InitImGui(hwnd);

    // Hide the system cursor globally.  SetSystemCursor affects all processes so
    // games / the desktop cannot fight it back.  Restored in Shutdown().
    HideSystemCursors();
    m_cursorHidden = true;

    // Load the custom cursor texture from the exe directory.
    {
        wchar_t exeBuf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
        std::filesystem::path cursorPath =
            std::filesystem::path(exeBuf).parent_path() / L"cursor.png";

        m_cursorSRV = LoadCursorTexture(
            m_device.Get(), cursorPath.wstring().c_str(),
            m_cursorTexW, m_cursorTexH);

        if (!m_cursorSRV)
            OutputDebugStringA("App: cursor.png not found — using fallback cursor\n");
    }

    m_monitorPicker.Refresh();

    // Auto-capture the primary monitor so the CRT filter is active immediately
    // on launch without the user having to open settings and pick a source.
    // MonitorPicker::Refresh() sorts monitors with the primary first.
    {
        auto& monitors = m_monitorPicker.GetMonitors();
        if (!monitors.empty())
            SetMonitorCaptureTarget(monitors[0].hmon);
    }

    return true;
}

// ---------------------------------------------------------------------------
// InitImGui
// ---------------------------------------------------------------------------

void App::InitImGui(HWND hwnd) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // We draw a custom virtual cursor that is processed by the CRT shader;
    // prevent ImGui from calling SetCursor() and fighting WM_SETCURSOR.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(m_device.Get(), m_context.Get());

    // Load custom fonts from the directory containing the exe.
    // Must happen before the first frame so the atlas is built correctly.
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (char* sep = strrchr(exePath, '\\')) *(sep + 1) = '\0';
        auto ttf = [&](const char* name) -> std::string {
            return std::string(exePath) + name;
        };
        io.Fonts->AddFontDefault();
        StyleFonts::cherryBomb  = io.Fonts->AddFontFromFileTTF(ttf("CherryBombOne-Regular.ttf").c_str(), 22.f);
        StyleFonts::chakraPetch = io.Fonts->AddFontFromFileTTF(ttf("ChakraPetch-Regular.ttf").c_str(),   28.f);
    }
}

// ---------------------------------------------------------------------------
// ToggleMenu — flips the settings panel and toggles WS_EX_TRANSPARENT.
//
// WS_EX_TRANSPARENT removed → overlay receives mouse clicks for ImGui.
// WS_EX_TRANSPARENT added   → DWM skips overlay in hit-testing; full
//                              click-through to the game / browser below.
// ---------------------------------------------------------------------------

void App::ToggleMenu() {
    m_settings.menuVisible = !m_settings.menuVisible;

    if (m_settings.menuVisible) {
        // Opening: move settings host to whichever enabled overlay is nearest the cursor.
        POINT cursor = {};
        GetCursorPos(&cursor);
        HMONITOR cursorMon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);

        // Find the enabled overlay monitor nearest the cursor.
        auto monCenter = [](HMONITOR hm) -> POINT {
            MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
            GetMonitorInfoW(hm, &mi);
            return { (mi.rcMonitor.left + mi.rcMonitor.right) / 2,
                     (mi.rcMonitor.top  + mi.rcMonitor.bottom) / 2 };
        };
        auto dist2 = [](POINT a, POINT b) -> long long {
            long long dx = a.x - b.x, dy = a.y - b.y;
            return dx*dx + dy*dy;
        };

        // Prefer the monitor actually under the cursor; fall back to nearest.
        HMONITOR targetMon = m_selectedMonitor;
        if (cursorMon && cursorMon != m_selectedMonitor) {
            bool cursorOnSecondary = false;
            for (auto& s : m_secondaryOverlays)
                if (s->hmon == cursorMon) { cursorOnSecondary = true; break; }

            if (cursorOnSecondary) {
                targetMon = cursorMon;
            } else {
                // Cursor is on an uncaptured monitor — find nearest enabled overlay.
                long long best = dist2(cursor, monCenter(m_selectedMonitor));
                for (auto& s : m_secondaryOverlays) {
                    long long d = dist2(cursor, monCenter(s->hmon));
                    if (d < best) { best = d; targetMon = s->hmon; }
                }
            }
        }

        if (targetMon && targetMon != m_selectedMonitor) {
            // Promote targetMon to primary: destroy its secondary, demote old primary.
            auto it = std::find_if(m_secondaryOverlays.begin(), m_secondaryOverlays.end(),
                [targetMon](const auto& s) { return s->hmon == targetMon; });
            if (it != m_secondaryOverlays.end()) {
                (*it)->capture.Stop();
                (*it)->currentFrame = nullptr;
                DestroyWindow((*it)->hwnd);
                m_secondaryOverlays.erase(it);
            }
            HMONITOR oldPrimary = m_selectedMonitor;
            SetMonitorCaptureTarget(targetMon); // updates m_selectedMonitor
            AddMonitorOverlay(oldPrimary);      // now safe: oldPrimary != m_selectedMonitor
        }

        if (m_welcomeVisible) m_welcomeVisible = false;

        LONG_PTR ex = GetWindowLongPtrW(m_outputHwnd, GWL_EXSTYLE);
        ex &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
        SetWindowLongPtrW(m_outputHwnd, GWL_EXSTYLE, ex);
        SetWindowPos(m_outputHwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        m_prevForeground = GetForegroundWindow();
        SetForegroundWindow(m_outputHwnd);
    } else {
        LONG_PTR ex = GetWindowLongPtrW(m_outputHwnd, GWL_EXSTYLE);
        if (!m_shakeInteractive && !m_welcomeVisible)
            ex |= static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
        SetWindowLongPtrW(m_outputHwnd, GWL_EXSTYLE, ex);
        SetWindowPos(m_outputHwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        if (m_prevForeground && IsWindow(m_prevForeground))
            SetForegroundWindow(m_prevForeground);
        m_prevForeground = nullptr;
    }
}

// ---------------------------------------------------------------------------
// RebuildOrderedPresets
// ---------------------------------------------------------------------------

void App::RefreshAudioDevices() {
    m_audioDevices.clear();
    m_audioDevices.push_back({ "__none__", "No Audio" });
    m_audioDevices.push_back({ "",         "Default"  });

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return;

    Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)))
        return;

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IMMDevice> dev;
        collection->Item(i, &dev);

        // Endpoint ID
        LPWSTR wid = nullptr;
        dev->GetId(&wid);
        int idLen = WideCharToMultiByte(CP_UTF8, 0, wid, -1, nullptr, 0, nullptr, nullptr);
        std::string id(idLen > 0 ? idLen - 1 : 0, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wid, -1, id.data(), idLen, nullptr, nullptr);
        CoTaskMemFree(wid);

        // Friendly name
        std::string name = id;
        Microsoft::WRL::ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                int nLen = WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                name.assign(nLen > 0 ? nLen - 1 : 0, '\0');
                WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, name.data(), nLen, nullptr, nullptr);
            }
            PropVariantClear(&pv);
        }

        m_audioDevices.push_back({ std::move(id), std::move(name) });
    }
}

void App::OnTrayNotify(WPARAM /*wParam*/, LPARAM lParam) {
    // TrackPopupMenu blocks Tick(), so restore the cursor before the menu
    // appears and let Tick() re-hide it on the next frame after it closes.
    if (m_cursorHidden) {
        SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, 0);
        m_cursorHidden = false;
    }

    UINT cmd = m_trayIcon.HandleNotify(m_outputHwnd, lParam,
                                        m_orderedPresets,
                                        m_overlayPaused,
                                        m_settings.cursorAutoHide);
    if      (cmd == TrayIcon::CMD_SETTINGS) {
        ToggleMenu();
    } else if (cmd == TrayIcon::CMD_PAUSE) {
        m_overlayPaused = !m_overlayPaused;
        if (m_overlayPaused) {
            ShowWindow(m_outputHwnd, SW_HIDE);
            if (m_cursorHidden) {
                SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, 0);
                m_cursorHidden = false;
            }
        } else {
            ShowWindow(m_outputHwnd, SW_SHOWNOACTIVATE);
            // Tick()'s per-frame cursor check will re-hide on the next frame.
        }
    } else if (cmd == TrayIcon::CMD_MOUSE) {
        m_settings.cursorAutoHide = !m_settings.cursorAutoHide;
        Settings::Save(m_settingsPath, m_settings);
    } else if (cmd == TrayIcon::CMD_QUIT) {
        PostQuitMessage(0);
    } else if (cmd >= TrayIcon::CMD_PRESET) {
        int idx = static_cast<int>(cmd - TrayIcon::CMD_PRESET);
        if (idx >= 0 && idx < (int)m_orderedPresets.size())
            Presets::Apply(m_settings, m_orderedPresets[idx].settings);
    }
}

void App::AddMonitorOverlay(HMONITOR hmon) {
    if (!hmon || hmon == m_selectedMonitor) return;
    for (auto& s : m_secondaryOverlays)
        if (s->hmon == hmon) return;

    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, &mi)) return;
    const RECT& r = mi.rcMonitor;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        L"OverlayV2_Secondary", L"OverlayV2",
        WS_POPUP | WS_VISIBLE,
        r.left, r.top, r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return;

    auto inst = std::make_unique<SecondaryOverlay>();
    inst->hwnd = hwnd;
    inst->hmon = hmon;

    try {
        if (!inst->renderer.Init(hwnd, m_device.Get(), m_context.Get())) {
            DestroyWindow(hwnd);
            return;
        }
    } catch (...) {
        DestroyWindow(hwnd);
        return;
    }
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

    if (!inst->capture.Init(m_device.Get())) {
        DestroyWindow(hwnd);
        return;
    }
    if (!inst->capture.StartCaptureMonitor(hmon)) {
        DestroyWindow(hwnd);
        return;
    }

    m_secondaryOverlays.push_back(std::move(inst));
}

void App::RemoveMonitorOverlay(HMONITOR hmon) {
    auto it = std::find_if(m_secondaryOverlays.begin(), m_secondaryOverlays.end(),
        [hmon](const auto& s) { return s->hmon == hmon; });
    if (it == m_secondaryOverlays.end()) return;
    (*it)->capture.Stop();
    (*it)->currentFrame = nullptr;
    DestroyWindow((*it)->hwnd);
    m_secondaryOverlays.erase(it);
}

void App::OnSecondaryResize(HWND hwnd, int w, int h) {
    for (auto& s : m_secondaryOverlays)
        if (s->hwnd == hwnd) { s->renderer.Resize(w, h); return; }
}

void App::RebuildOrderedPresets() {
    m_orderedPresets = Presets::BuildOrderedList(m_presetOrder, m_userPresets);
}

// ---------------------------------------------------------------------------
// MakeChildrenTransparent — safety net for any DXGI-created child windows.
// ---------------------------------------------------------------------------

void App::MakeChildrenTransparent() {
    if (!m_outputHwnd) return;

    static auto applyTransparent = [](HWND hwnd) {
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TRANSPARENT) return;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        OutputDebugStringA("App: applied WS_EX_TRANSPARENT to DXGI proxy window\n");
    };

    EnumChildWindows(m_outputHwnd, [](HWND child, LPARAM) -> BOOL {
        applyTransparent(child);
        return TRUE;
    }, 0);

    struct Ctx { DWORD pid; HWND skip[2]; };
    Ctx ctx{ GetCurrentProcessId(), { m_outputHwnd, m_streamHwnd } };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto& c = *reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != c.pid) return TRUE;
        for (HWND known : c.skip) if (hwnd == known) return TRUE;
        applyTransparent(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void App::Tick() {
    if (m_overlayPaused) return;

    m_settings.time = std::chrono::duration<float>(Clock::now() - m_startTime).count();

    {
        auto f = m_capture.TryGetFrame();
        if (f) m_currentFrame = std::move(f);
    }
    if (m_streamCapture.IsCapturing()) {
        auto f = m_streamCapture.TryGetFrame();
        if (f) m_streamCurrentFrame = std::move(f);
    }

    // -----------------------------------------------------------------------
    // ImGui frame — runs every tick so the custom cursor is always rendered
    // through the CRT filter, regardless of whether the settings panel is open.
    // -----------------------------------------------------------------------
    ImDrawData* imguiDrawData = nullptr;
    {
        BeginImGuiFrame();

        if (m_settings.menuVisible) {
            std::vector<HMONITOR> secondaryMons;
            for (auto& s : m_secondaryOverlays) secondaryMons.push_back(s->hmon);

            std::vector<SettingsPanel::AudioDevEntry> spDevs;
            spDevs.reserve(m_audioDevices.size());
            for (const auto& d : m_audioDevices)
                spDevs.push_back({ d.id, d.name });
            auto result = m_settingsPanel.Draw(
                m_settings,
                m_monitorPicker.GetMonitors(),
                m_selectedMonitor,
                secondaryMons,
                m_streamOpen,
                m_orderedPresets,
                spDevs);

            if (result.addMonitor)    AddMonitorOverlay(result.addMonitor);
            if (result.removeMonitor) {
                if (result.removeMonitor == m_selectedMonitor) {
                    // Disabling the primary: promote the nearest secondary, vacate this monitor.
                    if (!m_secondaryOverlays.empty()) {
                        POINT cursor = {};
                        GetCursorPos(&cursor);
                        auto monCenter = [](HMONITOR hm) -> POINT {
                            MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
                            GetMonitorInfoW(hm, &mi);
                            return { (mi.rcMonitor.left + mi.rcMonitor.right) / 2,
                                     (mi.rcMonitor.top  + mi.rcMonitor.bottom) / 2 };
                        };
                        auto dist2 = [](POINT a, POINT b) -> long long {
                            long long dx = a.x - b.x, dy = a.y - b.y;
                            return dx*dx + dy*dy;
                        };
                        auto best = m_secondaryOverlays.begin();
                        long long bestD = dist2(cursor, monCenter((*best)->hmon));
                        for (auto it = std::next(best); it != m_secondaryOverlays.end(); ++it) {
                            long long d = dist2(cursor, monCenter((*it)->hmon));
                            if (d < bestD) { bestD = d; best = it; }
                        }
                        HMONITOR newPrimary = (*best)->hmon;
                        (*best)->capture.Stop();
                        (*best)->currentFrame = nullptr;
                        DestroyWindow((*best)->hwnd);
                        m_secondaryOverlays.erase(best);
                        SetMonitorCaptureTarget(newPrimary);
                    }
                } else {
                    RemoveMonitorOverlay(result.removeMonitor);
                }
            }
            if (result.openStreamPicker) m_streamPicker.Open(m_settings.hiddenSources);
            if (result.openStream)       OpenStreamWindow();
            if (result.closeStream)      CloseStreamWindow();

            if (result.selectAudioDevice) {
                m_settings.audioCaptureDeviceId = result.audioDeviceId;
                Settings::Save(m_settingsPath, m_settings);
                if (m_streamOpen) {
                    m_audioLoopback.Stop();
                    if (m_settings.audioCaptureDeviceId != "__none__")
                        m_audioLoopback.Start(m_settings.audioCaptureDeviceId);
                }
            }

            // Preset: apply a built-in or user preset.
            if (result.applyPreset)
                Presets::Apply(m_settings, result.presetSettings);

            // Preset: save current settings as a named preset (update existing if name matches).
            if (result.savePreset && !result.savePresetName.empty()) {
                auto it = std::find_if(m_userPresets.begin(), m_userPresets.end(),
                    [&](const UserPreset& p) { return p.name == result.savePresetName; });
                if (it != m_userPresets.end()) {
                    it->settings = m_settings; // overwrite existing
                } else {
                    m_userPresets.push_back({ result.savePresetName, m_settings });
                    m_presetOrder.push_back(result.savePresetName);
                }
                Presets::Save(m_presetsPath, m_userPresets, m_presetOrder);
                RebuildOrderedPresets();
            }

            // Preset: manage operations (reorder, rename, delete).
            using ManageOp = SettingsPanel::DrawResult::ManageOp;
            if (result.manageOp != ManageOp::None && result.manageIdx >= 0
                    && result.manageIdx < (int)m_orderedPresets.size()) {
                const std::string& opName = m_orderedPresets[result.manageIdx].name;

                if (result.manageOp == ManageOp::MoveUp && result.manageIdx > 0) {
                    // Find in order and swap with predecessor.
                    auto it = std::find(m_presetOrder.begin(), m_presetOrder.end(), opName);
                    if (it != m_presetOrder.begin() && it != m_presetOrder.end())
                        std::iter_swap(it, std::prev(it));
                    Presets::Save(m_presetsPath, m_userPresets, m_presetOrder);
                    RebuildOrderedPresets();

                } else if (result.manageOp == ManageOp::MoveDown
                           && result.manageIdx < (int)m_orderedPresets.size() - 1) {
                    auto it = std::find(m_presetOrder.begin(), m_presetOrder.end(), opName);
                    if (it != m_presetOrder.end() && std::next(it) != m_presetOrder.end())
                        std::iter_swap(it, std::next(it));
                    Presets::Save(m_presetsPath, m_userPresets, m_presetOrder);
                    RebuildOrderedPresets();

                } else if (result.manageOp == ManageOp::Delete
                           && !m_orderedPresets[result.manageIdx].isBuiltIn) {
                    // Remove from user presets and order.
                    m_userPresets.erase(std::remove_if(m_userPresets.begin(), m_userPresets.end(),
                        [&](const UserPreset& p) { return p.name == opName; }), m_userPresets.end());
                    m_presetOrder.erase(std::remove(m_presetOrder.begin(), m_presetOrder.end(), opName),
                        m_presetOrder.end());
                    Presets::Save(m_presetsPath, m_userPresets, m_presetOrder);
                    RebuildOrderedPresets();

                } else if (result.manageOp == ManageOp::Rename
                           && !m_orderedPresets[result.manageIdx].isBuiltIn
                           && !result.manageName.empty()) {
                    // Rename in user presets and order.
                    for (auto& p : m_userPresets)
                        if (p.name == opName) { p.name = result.manageName; break; }
                    for (auto& s : m_presetOrder)
                        if (s == opName) { s = result.manageName; break; }
                    Presets::Save(m_presetsPath, m_userPresets, m_presetOrder);
                    RebuildOrderedPresets();
                }
            }

            // Close button inside Draw() set menuVisible = false.
            // Apply the WS_EX_TRANSPARENT change WITHOUT calling ToggleMenu()
            // (which would re-flip the flag back to true).
            if (!m_settings.menuVisible) {
                LONG_PTR ex = GetWindowLongPtrW(m_outputHwnd, GWL_EXSTYLE);
                if (!m_shakeInteractive)
                    ex |= static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
                SetWindowLongPtrW(m_outputHwnd, GWL_EXSTYLE, ex);
                SetWindowPos(m_outputHwnd, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                // Return keyboard focus to whatever had it before settings opened.
                if (m_prevForeground && IsWindow(m_prevForeground))
                    SetForegroundWindow(m_prevForeground);
                m_prevForeground = nullptr;
            }

            if (m_streamPicker.IsOpen()) {
                if (m_streamPicker.Draw()) {
                    HMONITOR selMon = m_streamPicker.GetSelectedMonitor();
                    HWND     selWnd = m_streamPicker.GetSelected();
                    if (selMon)      SetStreamCaptureMonitor(selMon);
                    else if (selWnd) SetStreamCaptureTarget(selWnd);
                }
            }
        }

        // Welcome popup — shown once on launch, dismissed by the user.
        if (m_welcomeVisible) {
            constexpr float kPW = 390.f;
            ImVec2 disp = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(
                ImVec2((disp.x - kPW) * 0.5f, disp.y * 0.28f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(kPW, 0.f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.92f);
            ImGuiWindowFlags wf =
                ImGuiWindowFlags_NoMove          | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_AlwaysAutoResize;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.f, 14.f));
            if (ImGui::Begin("##welcome", nullptr, wf)) {
                // Track actual popup bounds for the per-frame hover check.
                {
                    ImVec2 wp = ImGui::GetWindowPos();
                    ImVec2 ws = ImGui::GetWindowSize();
                    m_welcomeRect = { (LONG)wp.x, (LONG)wp.y,
                                      (LONG)(wp.x + ws.x), (LONG)(wp.y + ws.y) };
                }

                ImGui::TextColored(ImVec4(0.f, 0.85f, 1.f, 1.f), "CRT Monitor Overlay");
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Text("Ctrl + Shift + C   Open / close settings");
                ImGui::Text("Ctrl + Shift + Q   Quit");
                ImGui::Spacing();
                ImGui::TextWrapped(
                    "Shake the mouse vigorously to reveal a quick-access "
                    "button that opens the settings menu.");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                if (ImGui::Button("Got it", ImVec2(-1.f, 0.f))) {
                    m_welcomeVisible = false;
                    // Restore click-through — hover check no longer runs next frame.
                    LONG_PTR ex = GetWindowLongPtrW(m_outputHwnd, GWL_EXSTYLE);
                    ex |= static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
                    SetWindowLongPtrW(m_outputHwnd, GWL_EXSTYLE, ex);
                    SetWindowPos(m_outputHwnd, nullptr, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                        SWP_NOACTIVATE | SWP_FRAMECHANGED);
                }
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }

        // Shake button: restore click-through once the timer expires.
        if (m_shakeInteractive && m_cursorShowTimer <= 0.f && !m_settings.menuVisible) {
            m_shakeInteractive = false;
            LONG_PTR ex = GetWindowLongPtrW(m_outputHwnd, GWL_EXSTYLE);
            ex |= static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
            SetWindowLongPtrW(m_outputHwnd, GWL_EXSTYLE, ex);
            SetWindowPos(m_outputHwnd, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
        }

        // Shake popup button — appears near the top center of the screen for a few seconds after a
        // shake gesture. Uses barrel-compensated coords so hit-testing is accurate.
        if (m_shakeInteractive && !m_settings.menuVisible && m_cursorShowTimer > 0.f) {
            const float W = static_cast<float>(m_renderer.GetWidth());
            const float bx = (W - kShakeBtnW) * 0.5f;
            const float by = 20.f;
            // const float alpha = std::min(1.f, m_cursorShowTimer / 0.5f) * 0.85f;
            float alpha = m_cursorShowTimer / 0.5f;
            if (alpha > 1.f) alpha = 1.f;
            alpha = alpha * 0.85f;

            ImGui::SetNextWindowPos(ImVec2(bx, by), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(kShakeBtnW, kShakeBtnH), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(alpha * 0.75f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(2.f, 2.f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
            constexpr ImGuiWindowFlags kShakeFlags =
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;
            if (ImGui::Begin("##shakeMenuBtn", nullptr, kShakeFlags)) {
                if (ImGui::Button("[ CRT ]", ImGui::GetContentRegionAvail()))
                    ToggleMenu();
            }
            ImGui::End();
            ImGui::PopStyleVar(2);
        }

        // Virtual cursor — system cursor is globally blanked in Init(), so this
        // is the only cursor the user sees.  cursorAutoHide lets them opt into
        // hiding it when the menu is closed (shake still reveals it briefly).
        const bool showCursor = !m_settings.cursorAutoHide
                             || m_settings.menuVisible
                             || m_cursorShowTimer > 0.f;
        if (showCursor)
        {
            const auto& cs = m_settings;
            auto toU32 = [](float r, float g, float b) -> ImU32 {
                return IM_COL32(
                    static_cast<int>(r * 255.f + 0.5f),
                    static_cast<int>(g * 255.f + 0.5f),
                    static_cast<int>(b * 255.f + 0.5f),
                    255);
            };
            DrawVirtualCursor(
                ImGui::GetForegroundDrawList(),
                ImVec2(m_rawCursorX, m_rawCursorY),   // raw position → barrel-distorted by CRT
                (ImTextureID)((m_cursorSRV && !cs.cursorPlain) ? (void*)m_cursorSRV.Get() : nullptr),
                m_cursorTexW, m_cursorTexH,
                cs.cursorScale,
                cs.cursorThickness,
                toU32(cs.cursorR, cs.cursorG, cs.cursorB),
                cs.cursorStroke);
        }

        ImGui::Render();
        imguiDrawData = ImGui::GetDrawData();
    }

    // -----------------------------------------------------------------------
    // Resolve captured textures and render.
    // -----------------------------------------------------------------------
    ID3D11Texture2D* tex       = nullptr;
    ID3D11Texture2D* streamTex = nullptr;

    if (m_currentFrame)
        tex = m_capture.GetFrameTexture(m_currentFrame);
    if (m_streamCurrentFrame)
        streamTex = m_streamCapture.GetFrameTexture(m_streamCurrentFrame);

    m_renderer.RenderFrame(tex, m_settings, streamTex, imguiDrawData);

    // Cursor state for secondary overlays — same visibility rules as primary.
    const bool showSecondaryCursor = m_cursorSRV &&
        !m_settings.cursorPlain &&
        (!m_settings.cursorAutoHide || m_settings.menuVisible || m_cursorShowTimer > 0.f);

    POINT globalCursor = {};
    if (showSecondaryCursor) GetCursorPos(&globalCursor);

    for (auto& s : m_secondaryOverlays) {
        auto f = s->capture.TryGetFrame();
        if (f) s->currentFrame = std::move(f);
        ID3D11Texture2D* secTex = nullptr;
        if (s->currentFrame)
            secTex = s->capture.GetFrameTexture(s->currentFrame);

        // Draw cursor sprite if the mouse is on this monitor.
        ID3D11ShaderResourceView* secCursorSRV = nullptr;
        float secCursorX = 0.f, secCursorY = 0.f;
        if (showSecondaryCursor &&
            MonitorFromPoint(globalCursor, MONITOR_DEFAULTTONEAREST) == s->hmon) {
            MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
            GetMonitorInfoW(s->hmon, &mi);
            secCursorSRV = m_cursorSRV.Get();
            secCursorX   = static_cast<float>(globalCursor.x - mi.rcMonitor.left);
            secCursorY   = static_cast<float>(globalCursor.y - mi.rcMonitor.top);
        }

        s->renderer.RenderFrame(secTex, m_settings, nullptr, nullptr,
            secCursorSRV, secCursorX, secCursorY,
            m_cursorTexW, m_cursorTexH, m_settings.cursorScale);
    }
}

// ---------------------------------------------------------------------------
// BeginImGuiFrame
// ---------------------------------------------------------------------------

void App::BeginImGuiFrame() {
    switch (m_settings.menuStyle) {
        case 1:  ApplyDefaultStyle();    break;
        case 2:  ApplyN64Style();        break;
        case 3:  ApplyCamcorderStyle();  break;
        case 4:  ApplyDOSStyle();        break;
        case 5:  ApplyVCRStyle();        break;
        default: ApplyCRTOSDStyle();     break;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame(); // sets io.MousePos from raw GetCursorPos

    // ---------------------------------------------------------------------------
    // Virtual-cursor coordinate: apply the shader's forward barrel distortion to
    // the real screen position to get the intermediate-RT position that will
    // appear at the correct screen location after the CRT pass.
    //
    // Shader forward pass:  output_uv  →  sample_uv = BarrelDistort(output_uv)
    //   c  = output_uv * 2 - 1
    //   r² = dot(c, c)
    //   c' = c * (1 + curvature * r²) / zoom
    //   sample_uv = c' * 0.5 + 0.5
    //
    // Drawing the virtual cursor at sample_uv * (W, H) in the intermediate RT
    // makes it appear at (screen_x, screen_y) after barrel distortion.
    //
    // Overriding io.MousePos with the same value aligns ImGui's hit-testing with
    // the visually distorted positions of all control widgets.
    // ---------------------------------------------------------------------------
    {
        POINT screenPt = {};
        GetCursorPos(&screenPt);
        POINT pt = screenPt;
        // Convert virtual-desktop coords → window-client coords so the
        // positions are correct for non-primary monitors and always match
        // what ImGui_ImplWin32_NewFrame uses internally.
        ScreenToClient(m_outputHwnd, &pt);

        // Raw window position — draw the virtual cursor HERE so the CRT shader's
        // barrel distortion is applied to it naturally, making it appear to sit
        // on the curved CRT surface rather than at the flat real-screen position.
        m_rawCursorX = static_cast<float>(pt.x);
        m_rawCursorY = static_cast<float>(pt.y);

        // ImGui renders into the intermediate RT *before* the CRT pass, so both
        // the UI widgets and the virtual cursor are in the same raw intermediate-RT
        // coordinate space.  Barrel distortion is applied equally to both by the
        // CRT shader, so they stay visually aligned without any coordinate mapping.
        // Push raw coords so ImGui hit-testing is pixel-accurate.
        // (ImGui_ImplWin32_NewFrame skips the update when the overlay is not the
        // foreground window, e.g. during the shake-button phase, so we push here
        // as a reliable fallback that works in all cases.)
        ImGui::GetIO().AddMousePosEvent(m_rawCursorX, m_rawCursorY);

        // -----------------------------------------------------------------------
        // Shake-to-show cursor detection.
        // -----------------------------------------------------------------------
        {
            const float dx   = static_cast<float>(pt.x - m_lastMousePt.x);
            const float dy   = static_cast<float>(pt.y - m_lastMousePt.y);
            m_lastMousePt    = pt;
            m_shakeAccum     = m_shakeAccum * 0.72f + sqrtf(dx*dx + dy*dy);

            const float dt   = m_settings.time - m_prevTime;
            m_prevTime       = m_settings.time;
            if (m_cursorShowTimer > 0.f)
                m_cursorShowTimer -= dt;

            constexpr float kThreshold    = 420.f;
            constexpr float kShowDuration =   2.5f;
            if (!m_settings.menuVisible && m_shakeAccum > kThreshold) {
                m_cursorShowTimer = kShowDuration;
                m_shakeAccum      = 0.f;
                if (!m_shakeInteractive)
                    m_shakeInteractive = true;
                    // WS_EX_TRANSPARENT stays on — the per-frame hover check below
                    // removes it only when the cursor is actually inside the button.
            }
        }

        // -----------------------------------------------------------------------
        // Hover-based transparency — toggle WS_EX_TRANSPARENT per-frame so
        // clicks only reach the overlay when the cursor is over an interactive
        // element.  Welcome and shake are mutually exclusive (welcome wins).
        // -----------------------------------------------------------------------
        if (!m_settings.menuVisible) {
            bool over = false;
            if (m_welcomeVisible) {
                over = m_welcomeRect.right > m_welcomeRect.left
                    && m_rawCursorX >= m_welcomeRect.left
                    && m_rawCursorX <= m_welcomeRect.right
                    && m_rawCursorY >= m_welcomeRect.top
                    && m_rawCursorY <= m_welcomeRect.bottom;
            } else if (m_shakeInteractive && m_cursorShowTimer > 0.f) {
                over = IsInShakeButtonRect({static_cast<LONG>(m_rawCursorX),
                                            static_cast<LONG>(m_rawCursorY)});
            }

            LONG_PTR ex = GetWindowLongPtrW(m_outputHwnd, GWL_EXSTYLE);
            const bool isTransparent = (ex & WS_EX_TRANSPARENT) != 0;
            if (over == isTransparent) {
                if (over)
                    ex &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
                else
                    ex |= static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
                SetWindowLongPtrW(m_outputHwnd, GWL_EXSTYLE, ex);
                SetWindowPos(m_outputHwnd, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
            }
        }

        // -----------------------------------------------------------------------
        // System cursor visibility.
        // Hide on the captured monitor unless a topmost system window (taskbar,
        // start menu, tray popup) is under the cursor — those need the real cursor.
        // Restore on other monitors so the user can interact normally.
        // -----------------------------------------------------------------------
        if (m_selectedMonitor) {
            HMONITOR cursorMon = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST);
            bool onCapturedMonitor = (cursorMon == m_selectedMonitor);
            if (!onCapturedMonitor) {
                for (auto& s : m_secondaryOverlays)
                    if (cursorMon == s->hmon) { onCapturedMonitor = true; break; }
            }

            bool shouldHide = false;
            if (onCapturedMonitor) {
                HWND underCursor = WindowFromPoint(screenPt);
                bool isOurs = (underCursor == m_outputHwnd || underCursor == m_streamHwnd);
                if (!isOurs) {
                    for (auto& s : m_secondaryOverlays)
                        if (underCursor == s->hwnd) { isOurs = true; break; }
                }
                if (underCursor && !isOurs) {
                    LONG_PTR ex = GetWindowLongPtrW(underCursor, GWL_EXSTYLE);
                    shouldHide = !(ex & WS_EX_TOPMOST);
                } else {
                    shouldHide = true;
                }
            }

            if (shouldHide != m_cursorHidden) {
                if (shouldHide) HideSystemCursors();
                else SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, 0);
                m_cursorHidden = shouldHide;
            }
        }
    }

    ImGui::NewFrame();
}

// ---------------------------------------------------------------------------
// SetMonitorCaptureTarget
// ---------------------------------------------------------------------------

void App::SetMonitorCaptureTarget(HMONITOR hmon) {
    if (!hmon) return;
    m_selectedMonitor = hmon;

    // Move the overlay window to cover the newly selected monitor.
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hmon, &mi)) {
        const RECT& r = mi.rcMonitor;
        SetWindowPos(m_outputHwnd, HWND_TOPMOST,
                     r.left, r.top,
                     r.right  - r.left,
                     r.bottom - r.top,
                     SWP_NOACTIVATE);
        // WM_SIZE fires automatically and calls OnResize → Renderer::Resize.
    }

    m_currentFrame = nullptr;
    m_capture.Stop();
    if (!m_capture.StartCaptureMonitor(hmon))
        OutputDebugStringA("App: WGCCapture::StartCaptureMonitor failed\n");
    m_currentFrame = nullptr;
}

// ---------------------------------------------------------------------------
// SetStreamCaptureTarget
// ---------------------------------------------------------------------------

void App::SetStreamCaptureTarget(HWND target) {
    if (!target) return;
    m_streamCurrentFrame = nullptr;
    m_streamCapture.Stop();
    if (!m_streamCapture.StartCapture(target))
        OutputDebugStringA("App: stream WGCCapture::StartCapture failed\n");
    m_streamCurrentFrame = nullptr;
}

void App::SetStreamCaptureMonitor(HMONITOR hmon) {
    if (!hmon) return;
    m_streamCurrentFrame = nullptr;
    m_streamCapture.Stop();
    if (!m_streamCapture.StartCaptureMonitor(hmon))
        OutputDebugStringA("App: stream WGCCapture::StartCaptureMonitor failed\n");
    m_streamCurrentFrame = nullptr;
}

// ---------------------------------------------------------------------------
// OpenStreamWindow / CloseStreamWindow
// ---------------------------------------------------------------------------

void App::OpenStreamWindow() {
    if (m_streamHwnd) return;

    m_streamHwnd = CreateWindowExW(
        0,
        L"OverlayV2_Stream",
        L"CRT Overlay \u2014 Stream Output",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!m_streamHwnd) {
        OutputDebugStringA("App: failed to create stream window\n");
        return;
    }

    if (!m_renderer.InitStream(m_streamHwnd)) {
        OutputDebugStringA("App: D3DRenderer::InitStream failed\n");
        DestroyWindow(m_streamHwnd);
        m_streamHwnd = nullptr;
        return;
    }

    m_streamOpen = true;
    if (m_settings.audioCaptureDeviceId != "__none__")
        m_audioLoopback.Start(m_settings.audioCaptureDeviceId);
}

void App::CloseStreamWindow() {
    if (!m_streamHwnd) return;
    m_audioLoopback.Stop();
    m_renderer.ShutdownStream();
    m_streamCapture.Stop();
    m_streamCurrentFrame = nullptr;
    m_streamOpen = false;
    HWND hwnd = m_streamHwnd;
    m_streamHwnd = nullptr;
    DestroyWindow(hwnd);
}

// ---------------------------------------------------------------------------
// OnResize
// ---------------------------------------------------------------------------

void App::OnResize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    ImGui_ImplDX11_InvalidateDeviceObjects();
    m_renderer.Resize(w, h);
    ImGui_ImplDX11_CreateDeviceObjects();
}

void App::OnStreamResize(int w, int h) {
    m_renderer.ResizeStream(w, h);
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void App::Shutdown() {
    if (m_shutdownCalled) return;
    m_shutdownCalled = true;

    // Restore all system cursor images to their defaults.
    SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, 0);

    m_trayIcon.Destroy();
    Settings::Save(m_settingsPath, m_settings);

    for (auto& s : m_secondaryOverlays) {
        s->capture.Stop();
        s->currentFrame = nullptr;
        if (s->hwnd) { DestroyWindow(s->hwnd); s->hwnd = nullptr; }
    }
    m_secondaryOverlays.clear();

    CloseStreamWindow();

    m_capture.Stop();
    m_currentFrame       = nullptr;
    m_streamCurrentFrame = nullptr;

    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}
