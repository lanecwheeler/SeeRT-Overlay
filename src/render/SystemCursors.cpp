#define OEMRESOURCE   // OCR_* cursor ids
#include "SystemCursors.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace {

// Each system cursor and its value under HKCU\Control Panel\Cursors.
struct CursorSlot { int id; const wchar_t* value; };
constexpr CursorSlot kSlots[] = {
    { OCR_NORMAL,      L"Arrow"       }, { OCR_IBEAM,       L"IBeam"     },
    { OCR_WAIT,        L"Wait"        }, { OCR_CROSS,       L"Crosshair" },
    { OCR_UP,          L"UpArrow"     }, { OCR_SIZENWSE,    L"SizeNWSE"  },
    { OCR_SIZENESW,    L"SizeNESW"    }, { OCR_SIZEWE,      L"SizeWE"    },
    { OCR_SIZENS,      L"SizeNS"      }, { OCR_SIZEALL,     L"SizeAll"   },
    { OCR_NO,          L"No"          }, { OCR_HAND,        L"Hand"      },
    { OCR_APPSTARTING, L"AppStarting" }, { 32651,           L"Help"      },   // OCR_HELP
    { 32631,           L"NWPen"       }, { 32671,           L"Pin"       },   // pen, pin
    { 32672,           L"Person"      },
};

constexpr const wchar_t* kCursorsKey = L"Control Panel\\Cursors";

// The scheme's file for a cursor, environment variables expanded; empty
// when the scheme leaves it to Windows' built-in cursor.
std::wstring SchemeFile(const wchar_t* value) {
    wchar_t raw[MAX_PATH] = {};
    DWORD   bytes = sizeof(raw);
    if (RegGetValueW(HKEY_CURRENT_USER, kCursorsKey, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                     nullptr, raw, &bytes) != ERROR_SUCCESS || !raw[0])
        return {};
    wchar_t path[MAX_PATH] = {};
    if (!ExpandEnvironmentStringsW(raw, path, MAX_PATH)) return {};
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return {};
    return path;
}

uint32_t ReadU32(const std::vector<uint8_t>& b, size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}

// An animated cursor's timing, from its .ani file: one duration (seconds)
// per step.  Empty if it isn't an animated cursor.
std::vector<float> AniTiming(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "ACON", 4)) return {};

    uint32_t steps = 0, frames = 0, jifRate = 0;
    std::vector<uint32_t> rates;
    for (size_t at = 12; at + 8 <= b.size();) {
        const uint32_t size = ReadU32(b, at + 4);
        const size_t   body = at + 8;
        if (body + size > b.size()) break;
        if (!std::memcmp(b.data() + at, "anih", 4) && size >= 36) {
            frames  = ReadU32(b, body + 4);
            steps   = ReadU32(b, body + 8);
            jifRate = ReadU32(b, body + 28);
        } else if (!std::memcmp(b.data() + at, "rate", 4)) {
            for (size_t i = 0; i + 4 <= size; i += 4) rates.push_back(ReadU32(b, body + i));
        }
        at = body + size + (size & 1);   // chunks are word-aligned
    }
    if (!steps) steps = frames;
    if (steps < 2 || steps > 512) return {};

    std::vector<float> out(steps);
    for (uint32_t i = 0; i < steps; i++) {
        const uint32_t jiffies = i < rates.size() ? rates[i] : jifRate;
        out[i] = std::max(1u, jiffies) / 60.f;   // a jiffy is 1/60 s
    }
    return out;
}

// One step of a cursor as a straight-alpha BGRA texture.  The cursor is drawn
// on black and on white, which gives each pixel's alpha and colour whatever
// format the cursor is (32-bit alpha, or the old AND/XOR masks).  Pixels that
// invert the screen — which an overlay can't do — come out black with a white
// outline, the way Windows' own high-contrast cursors look.
bool Rasterise(ID3D11Device* device, HCURSOR cursor, UINT step, int w, int h,
               SystemCursors::Frame& out) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC     screen = GetDC(nullptr);
    HDC     dc     = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    void*   bits[2] = {};
    HBITMAP bmp[2]  = { CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits[0], nullptr, 0),
                        CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits[1], nullptr, 0) };
    bool ok = bmp[0] && bmp[1];
    for (int i = 0; ok && i < 2; i++) {
        HGDIOBJ old = SelectObject(dc, bmp[i]);
        std::memset(bits[i], i ? 0xFF : 0x00, static_cast<size_t>(w) * h * 4);   // black, then white
        ok = DrawIconEx(dc, 0, 0, cursor, w, h, step, nullptr, DI_NORMAL) != FALSE;
        GdiFlush();
        SelectObject(dc, old);
    }

    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0);
    std::vector<uint8_t> inverted(static_cast<size_t>(w) * h, 0);
    if (ok) {
        const uint8_t* onBlack = static_cast<const uint8_t*>(bits[0]);
        const uint8_t* onWhite = static_cast<const uint8_t*>(bits[1]);
        for (int i = 0; i < w * h; i++) {
            const uint8_t* b  = onBlack + i * 4;
            const uint8_t* wh = onWhite + i * 4;
            const int diff = ((wh[0] - b[0]) + (wh[1] - b[1]) + (wh[2] - b[2])) / 3;   // 255 * (1 - alpha)
            if (diff < -64) { inverted[i] = 1; continue; }
            const int a = std::clamp(255 - diff, 0, 255);
            uint8_t* p = &px[i * 4];
            if (a > 0)
                for (int c = 0; c < 3; c++) p[c] = static_cast<uint8_t>(std::min(255, b[c] * 255 / a));
            p[3] = static_cast<uint8_t>(a);
        }
        // Inverting pixels: black, with a white outline where they meet transparency.
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const int i = y * w + x;
                if (inverted[i]) { uint8_t* p = &px[i * 4]; p[0] = p[1] = p[2] = 0; p[3] = 255; continue; }
                if (px[i * 4 + 3]) continue;
                bool edge = false;
                for (int dy = -1; dy <= 1 && !edge; dy++)
                    for (int dx = -1; dx <= 1 && !edge; dx++) {
                        const int nx = x + dx, ny = y + dy;
                        edge = nx >= 0 && ny >= 0 && nx < w && ny < h && inverted[ny * w + nx];
                    }
                if (edge) { uint8_t* p = &px[i * 4]; p[0] = p[1] = p[2] = 255; p[3] = 255; }
            }
    }
    DeleteObject(bmp[0]);
    DeleteObject(bmp[1]);
    DeleteDC(dc);
    if (!ok) return false;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width      = static_cast<UINT>(w);
    td.Height     = static_cast<UINT>(h);
    td.MipLevels  = 1;
    td.ArraySize  = 1;
    td.Format     = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc = { 1, 0 };
    td.Usage      = D3D11_USAGE_IMMUTABLE;
    td.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd = { px.data(), static_cast<UINT>(w) * 4, 0 };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&td, &srd, &tex)) ||
        FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &out.srv)))
        return false;
    out.w = w;
    out.h = h;
    return true;
}

}   // namespace

const std::vector<int>& SystemCursors::Ids() {
    static const std::vector<int> ids = [] {
        std::vector<int> v;
        for (const CursorSlot& s : kSlots) v.push_back(s.id);
        return v;
    }();
    return ids;
}

int SystemCursors::SchemeSize() {
    DWORD size = 0, bytes = sizeof(size);
    if (RegGetValueW(HKEY_CURRENT_USER, kCursorsKey, L"CursorBaseSize", RRF_RT_REG_DWORD,
                     nullptr, &size, &bytes) != ERROR_SUCCESS || size == 0)
        size = static_cast<DWORD>(GetSystemMetrics(SM_CXCURSOR));
    return std::clamp(static_cast<int>(size), 16, 256);
}

void SystemCursors::Load(ID3D11Device* device, int sizePx) {
    m_cursors.clear();
    for (const CursorSlot& slot : kSlots) {
        Cursor c;
        c.id     = slot.id;
        c.system = LoadCursorW(nullptr, MAKEINTRESOURCEW(slot.id));
        if (!c.system) continue;

        // The scheme's own file where it names one (animated .ani included);
        // otherwise a copy of Windows' built-in, at the pointer size.
        const std::wstring file = SchemeFile(slot.value);
        HCURSOR h = nullptr;
        bool    fromFile = false;
        if (!file.empty()) {
            h = static_cast<HCURSOR>(LoadImageW(nullptr, file.c_str(), IMAGE_CURSOR, sizePx, sizePx, LR_LOADFROMFILE));
            fromFile = h != nullptr;
        }
        if (!h) h = static_cast<HCURSOR>(CopyImage(c.system, IMAGE_CURSOR, sizePx, sizePx, LR_COPYFROMRESOURCE));
        if (!h) h = static_cast<HCURSOR>(CopyImage(c.system, IMAGE_CURSOR, sizePx, sizePx, 0));
        if (!h) continue;

        // Size and click point from the cursor itself (its hotspot scales with it).
        ICONINFO info = {};
        int w = sizePx, ht = sizePx;
        float hotX = 0.f, hotY = 0.f;
        if (GetIconInfo(h, &info)) {
            BITMAP bm = {};
            if (info.hbmColor && GetObject(info.hbmColor, sizeof(bm), &bm)) { w = bm.bmWidth; ht = bm.bmHeight; }
            else if (info.hbmMask && GetObject(info.hbmMask, sizeof(bm), &bm)) { w = bm.bmWidth; ht = bm.bmHeight / 2; }
            hotX = static_cast<float>(info.xHotspot);
            hotY = static_cast<float>(info.yHotspot);
            if (info.hbmColor) DeleteObject(info.hbmColor);
            if (info.hbmMask)  DeleteObject(info.hbmMask);
        }

        const std::vector<float> timing = fromFile ? AniTiming(file) : std::vector<float>{};
        const UINT steps = timing.empty() ? 1u : static_cast<UINT>(timing.size());
        float end = 0.f;
        for (UINT step = 0; step < steps; step++) {
            Frame f;
            if (!Rasterise(device, h, step, w, ht, f)) break;
            f.hotX = hotX;
            f.hotY = hotY;
            c.frames.push_back(std::move(f));
            end += timing.empty() ? 1.f : timing[step];
            c.ends.push_back(end);
        }
        DestroyCursor(h);
        if (!c.frames.empty()) m_cursors.push_back(std::move(c));
    }
}

const SystemCursors::Frame* SystemCursors::Pick(const Cursor& c, float time) const {
    if (c.frames.size() == 1) return &c.frames[0];
    const float t = std::fmod(std::max(time, 0.f), c.ends.back());
    const size_t i = std::upper_bound(c.ends.begin(), c.ends.end(), t) - c.ends.begin();
    return &c.frames[std::min(i, c.frames.size() - 1)];
}

const SystemCursors::Frame* SystemCursors::ForHandle(HCURSOR shown, float time) const {
    if (!shown) return nullptr;
    for (const Cursor& c : m_cursors)
        if (c.system == shown) return Pick(c, time);
    return nullptr;
}

const SystemCursors::Frame* SystemCursors::ForId(int id, float time) const {
    for (const Cursor& c : m_cursors)
        if (c.id == id) return Pick(c, time);
    if (id == OCR_NORMAL && !m_cursors.empty()) return Pick(m_cursors.front(), time);
    return nullptr;
}
