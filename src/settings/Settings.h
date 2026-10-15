#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>

// CRTSettings is the single source-of-truth for all shader + post-process parameters.
// Packed to 16-byte boundary to match the HLSL constant buffer layout exactly.
// A signature in Web 1.0's guestbook.
struct GuestbookEntry {
    std::string name;
    std::string message;
    std::string date;   // YYYY-MM-DD
};

struct CRTSettings {
    float curvatureX        = 0.1f;   // 0–0.5  barrel/pincushion X
    float curvatureY        = 0.1f;   // 0–0.5  barrel/pincushion Y
    float zoom              = 1.05f;  // 1.0–2.0 compensate for distortion crop
    float scanlineIntensity = 0.35f;  // 0–1    flat picture → full beam profile

    float scanlineCount     = 500.0f; // 200–1200 lines across the picture height
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

    float shadowMaskScale     = 1.0f;  // 1–4   screen pixels per phosphor stripe (rounded)
    float shadowMaskType      = 0.0f;  // 0=aperture grille, 1=shadow mask, 2=slot mask, 3=magenta/green
    float humBarFrequency     = 3.0f;  // 0.5–20  number of visible hum bands on screen
    float convergence         = 0.0f;  // 0–1     RGB horizontal misalignment, growing toward the edges

    float crtGamma            = 2.4f;  // 1.8–3.0 CRT decoding gamma (signal → light)
    float displayGamma        = 2.2f;  // 1.8–3.0 gamma used to re-encode for this monitor
    float beamWidth           = 0.25f; // 0.1–0.5 beam width of dark lines, in line heights
    float beamDynamics        = 0.6f;  // 0–1     how much brighter lines widen

    float lineResolve         = 0.0f;  // 0–1     0 = full detail under the lines, 1 = one colour per line
    float maskCompensation    = 0.75f; // 0–1     brighten lit phosphors to offset the mask's light loss
    float cornerRadius        = 0.03f; // 0–0.25  rounded tube corners, fraction of picture height
    float chromaBleed         = 0.0f;  // 0–1     composite colour bleed (chroma bandwidth limit)

    float signalSoftness      = 0.0f;  // 0–1     horizontal softness of the luma signal
    float glowRadius          = 6.0f;  // 1–30    glow spread, in pixels at 1080p
    float halationRadius      = 40.0f; // 10–150  halation spread, in pixels at 1080p
    float persistence         = 0.0f;  // 0–1     phosphor afterglow (0–250 ms decay)

    float rasterBloom         = 0.0f;  // 0–1     picture grows with scene brightness (up to 5%)
    float frameDelta          = 0.0f;  // seconds since the previous frame — written each frame by App
    float degauss             = 0.0f;  // 0–1  strength of the degauss wobble now — written each frame by App
    float degaussTime         = 0.0f;  // seconds since that degauss started — written each frame by App

    // UI-only — not uploaded to the GPU constant buffer.
    // 0 = CRT OSD  1 = Simple  3 = Camcorder  4 = DOS Terminal  5 = VCR
    // (2 was the old N64 style — retired, loads as 64 Adventure)
    // 6 = 64 Adventure  7 = Sword & Sorcery  8 = Teletext  9 = Vector Monitor
    // 10 = Arcade Service  11 = Computer Lab  12 = Web 1.0.  Ids never change; the pickers' order is kStyles
    // in SettingsPanel.cpp.
    int   menuStyle         = 8;      // Teletext
    bool  menuVisible       = false;  // toggle button hides/shows the settings panel
    float toggleX           = 20.f;   // toggle button screen position (persisted)
    float toggleY           = 20.f;

    // Settings panel size — multipliers on each menu style's own layout.
    float menuScale         = 1.f;    // 0.5–2  text, padding and control sizes
    float menuWidth         = 1.f;    // 0.5–2  panel width
    float menuHeight        = 1.f;    // 0.5–2  panel height

    // Cursor appearance — not uploaded to the GPU constant buffer.
    float cursorScale       = 0.44f; // render scale (fraction of source pixel size)
    float cursorThickness   = 0.f;   // dilation radius in px; 0 = no extra passes
    float cursorR           = 1.f;   // tint RGB (0–1 each); white = no tint change
    float cursorG           = 1.f;
    float cursorB           = 1.f;
    bool  cursorAutoHide    = false; // hide cursor when menu closed; shake mouse to reveal
    bool  cursorStroke      = false; // draw a black shadow offset (+5, +5) behind
    bool  cursorPlain       = false; // use fallback cursor
    bool  cursorShowHotspot = false; // draw a small dot at the exact click point

    // Custom cursor image — UTF-8 path to the user's copy in
    // %APPDATA%\SeeRT Overlay\cursors.  Empty = built-in cursor.png.
    // Machine-specific, so deliberately not part of presets.
    std::string customCursorPath;
    int   customCursorHotX  = 0;     // hotspot (click point) in source-image pixels
    int   customCursorHotY  = 0;

    // Updates — ask GitHub for a newer release at launch (toggled from the
    // tray menu), and the newest version already announced, so each release
    // only pops a notification once.
    bool  checkForUpdates   = true;
    bool  autoUpdate        = true;   // download a newer release in the background (installing still asks)
    bool  installWithoutAsking = false;   // install a downloaded update when the app quits, without asking
    std::string lastNotifiedUpdate;
    std::string lastRunVersion;      // to show the patch notes once after an update

    // Easter eggs in the game-style menus — progress and high scores, kept
    // between launches (and, like the rest of the menu state, not in presets).
    int   adv64RedCoins     = 0;      // 64 Adventure: a bit per red coin found
    bool  adv64Gold         = false;  // 64 Adventure: gold buttons (once all 8 are found)
    int   vectorHighScore   = 0;      // Vector Monitor's Asteroids
    std::string vectorHighName = "AAA";
    int   arcadeHighScore   = 0;      // Arcade Service's Breakout
    int   webHits           = 0;      // Web 1.0's visitor counter: times the menu was opened
    std::vector<GuestbookEntry> guestbook;   // Web 1.0's guestbook, oldest first

    // Window picker — title substrings the user has chosen to hide.
    std::vector<std::string> hiddenSources;

    // Stream window — also show the settings menu (and cursor, while it's
    // open) in the stream output, so viewers can see the menu.
    bool  streamShowMenu    = false;

    // CRT OSD's Degauss button plays the coil's hum.
    bool  degaussSound      = true;

    // Audio capture — endpoint ID of the render device to loopback-capture.
    // "__none__" = disabled (no audio streamed). "" = Windows default render device.
    std::string audioCaptureDeviceId = "__none__";
};

namespace Settings {
    // Returns %APPDATA%\SeeRT Overlay — where settings, presets and user
    // assets live.  Empty (i.e. the current directory) if APPDATA is unavailable.
    // Paths are kept as std::filesystem::path (UTF-16 on Windows) so profile
    // directories with non-ASCII characters open correctly.
    std::filesystem::path DataDir();

    // Returns the default %APPDATA%\SeeRT Overlay\settings.json path.
    std::filesystem::path DefaultPath();

    // Loads settings from JSON at path. Returns defaults if the file does not exist.
    CRTSettings Load(const std::filesystem::path& path);

    // Serialises settings to JSON at path. Creates parent directories if needed.
    void Save(const std::filesystem::path& path, const CRTSettings& s);

    // Read/write the visual (shader + cursor) fields — the set a preset
    // captures.  Shared by settings.json and presets.json so the field lists
    // can never drift apart.  Missing or mistyped keys leave the field as-is.
    void VisualToJson(const CRTSettings& s, nlohmann::json& j);
    void VisualFromJson(const nlohmann::json& j, CRTSettings& s);
}
