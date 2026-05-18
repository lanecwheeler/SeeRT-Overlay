#pragma once
#include <string>
#include <vector>

// CRTSettings is the single source-of-truth for all shader + post-process parameters.
// Packed to 16-byte boundary to match the HLSL constant buffer layout exactly.
struct CRTSettings {
    float curvatureX        = 0.1f;   // 0–0.5  barrel/pincushion X
    float curvatureY        = 0.1f;   // 0–0.5  barrel/pincushion Y
    float zoom              = 1.05f;  // 1.0–2.0 compensate for distortion crop
    float scanlineIntensity = 0.35f;  // 0–1    darkness of scanline gaps

    float scanlineCount     = 500.0f; // 200–1200 number of scanline pairs
    float vignetteStrength  = 0.4f;   // 0–1    radial edge darkening
    float glowStrength      = 0.3f;   // 0–2    phosphor bloom
    float brightness        = 1.0f;   // 0.5–2.0

    float contrast          = 1.0f;   // 0.5–2.0
    float saturation        = 1.1f;   // 0–2
    float flickerIntensity  = 0.0f;   // 0–1    strength of brightness flicker
    float flickerSpeed      = 8.0f;   // 1–30   oscillation rate in Hz

    float time              = 0.0f;   // seconds since app start — written each frame by App
    float flickerRandomness = 0.0f;  // 0 = smooth sines, 1 = pure random noise
    float humBarIntensity   = 0.18f; // 0–1   strength of rolling hum-bar bands
    float humBarSpeed       = 0.10f; // 0–2   screen heights per second (upward scroll)

    float diffuseStrength   = 0.0f;  // 0–2   brightness-weighted bloom spread
    float diffuseThreshold  = 0.4f;  // 0–1   luminance below which pixels don't spread
    float phosphorR         = 1.0f;  // screen phosphor tint R (1,1,1 = no tint)
    float phosphorG         = 1.0f;  // screen phosphor tint G

    float phosphorB           = 1.0f;  // screen phosphor tint B
    float staticIntensity     = 0.04f; // 0–1   TV static grain intensity
    float staticSpeed         = 12.0f; // 1–60  grain pattern flips per second
    float shadowMaskIntensity = 0.15f; // 0–1   sub-pixel mask strength

    float shadowMaskScale     = 1.5f;  // 0.5–4 pattern scale (screen pixels per cell)
    float shadowMaskType      = 0.0f;  // 0=aperture grille, 1=shadow mask, 2=slot mask
    float humBarFrequency     = 3.0f;  // 0.5–20  number of visible hum bands on screen
    float convergence         = 0.0f;  // 0–1     RGB horizontal misalignment (chromatic aberration)

    // UI-only — not uploaded to the GPU constant buffer.
    // 0 = CRT OSD  1 = Simple  2 = N64  3 = Camcorder  4 = DOS Terminal  5 = VCR
    int   menuStyle         = 0;
    bool  menuVisible       = false;  // toggle button hides/shows the settings panel
    float toggleX           = 20.f;   // toggle button screen position (persisted)
    float toggleY           = 20.f;

    // Cursor appearance — not uploaded to the GPU constant buffer.
    float cursorScale       = 0.44f; // render scale (fraction of source pixel size)
    float cursorThickness   = 0.f;   // dilation radius in px; 0 = no extra passes
    float cursorR           = 1.f;   // tint RGB (0–1 each); white = no tint change
    float cursorG           = 1.f;
    float cursorB           = 1.f;
    bool  cursorAutoHide    = false; // hide cursor when menu closed; shake mouse to reveal
    bool  cursorStroke      = false; // draw a black shadow offset (+5, +5) behind
    bool  cursorPlain       = false; // use fallback cursor

    // Window picker — title substrings the user has chosen to hide.
    std::vector<std::string> hiddenSources;

    // Audio capture — endpoint ID of the render device to loopback-capture.
    // "__none__" = disabled (no audio streamed). "" = Windows default render device.
    std::string audioCaptureDeviceId = "__none__";
};

namespace Settings {
    // Returns the default %APPDATA%\OverlayV2\settings.json path.
    std::string DefaultPath();

    // Loads settings from JSON at path. Returns defaults if the file does not exist.
    CRTSettings Load(const std::string& path);

    // Serialises settings to JSON at path. Creates parent directories if needed.
    void Save(const std::string& path, const CRTSettings& s);
}
