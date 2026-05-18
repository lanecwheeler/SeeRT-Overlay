#pragma once
#include <imgui.h>

// Custom per-style fonts loaded once in App::InitImGui().
// nullptr if the TTF file was not found next to the executable.
namespace StyleFonts {
    inline ImFont* cherryBomb  = nullptr;  // N64       — CherryBombOne-Regular.ttf, 22 px
    inline ImFont* chakraPetch = nullptr;  // Camcorder — ChakraPetch-Regular.ttf,   14 px
}
