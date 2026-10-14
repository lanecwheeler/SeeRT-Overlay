#pragma once
#include <imgui.h>

// Custom per-style fonts loaded once in App::InitImGui().
// nullptr if the TTF file was not found next to the executable.
namespace StyleFonts {
    inline ImFont* cherryBomb  = nullptr;  // 64 Adventure — CherryBombOne-Regular.ttf, 22 px
    inline ImFont* chakraPetch = nullptr;  // Camcorder — ChakraPetch-Regular.ttf,   14 px
    inline ImFont* metalMania  = nullptr;  // Sword & Sorcery titles — MetalMania-Regular.ttf
    inline ImFont* cinzel      = nullptr;  // Sword & Sorcery text   — CinzelDecorative-Bold.ttf
    inline ImFont* bedstead    = nullptr;  // Teletext       — Bedstead.otf (SAA5050 recreation)
    inline ImFont* shareTech   = nullptr;  // Vector Monitor — ShareTechMono-Regular.ttf
    inline ImFont* pressStart  = nullptr;  // Arcade Service — PressStart2P-Regular.ttf
    inline ImFont* shortStack  = nullptr;  // Computer Lab   — ShortStack-Regular.ttf
    // Web 1.0 uses the fonts every 90s homepage used, loaded from Windows'
    // own Fonts folder (they're not ours to ship).
    inline ImFont* times       = nullptr;  // Times New Roman — times.ttf
    inline ImFont* comic       = nullptr;  // Comic Sans MS, bold — comicbd.ttf
    inline ImFont* msSans      = nullptr;  // Microsoft Sans Serif — micross.ttf (browser chrome)
}
