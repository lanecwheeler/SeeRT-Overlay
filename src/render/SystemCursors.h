#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <Windows.h>
#include <vector>

// The user's Windows pointer scheme as textures, so the overlay can draw the
// real cursors through the CRT filter: the arrow, I-beam, link hand, resize
// arrows and the rest, animated ones (the busy spinner) frame by frame.
//
// Load() must run while the system cursors are visible (not replaced by the
// overlay's blank ones): cursors the scheme leaves to Windows' built-ins are
// copied from the live system cursors.
class SystemCursors {
public:
    struct Frame {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        int   w = 0, h = 0;
        float hotX = 0.f, hotY = 0.f;   // click point, in texture pixels
    };

    // The OCR_ ids of every cursor loaded — the ones to replace with blanks.
    static const std::vector<int>& Ids();

    // Loads the scheme at sizePx (the user's pointer size, in screen pixels).
    void Load(ID3D11Device* device, int sizePx);
    bool Loaded() const { return !m_cursors.empty(); }

    // The frame to show at `time` (seconds) for the system cursor `shown`, as
    // GetCursorInfo reports it; null if it isn't one of the scheme's cursors
    // (an app's own cursor, which Windows draws itself).
    const Frame* ForHandle(HCURSOR shown, float time) const;
    // The same for an OCR_ id (OCR_NORMAL falls back to the first loaded cursor).
    const Frame* ForId(int id, float time) const;

    // The user's pointer size: Settings > Accessibility > Mouse pointer size.
    static int SchemeSize();

private:
    struct Cursor {
        int                id = 0;
        HCURSOR            system = nullptr;   // the shared system handle (stable while we blank it)
        std::vector<Frame> frames;             // one per animation step
        std::vector<float> ends;               // each step's end time within the loop, seconds
    };
    const Frame* Pick(const Cursor& c, float time) const;

    std::vector<Cursor> m_cursors;
};
