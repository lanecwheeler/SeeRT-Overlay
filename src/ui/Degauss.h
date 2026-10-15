#pragma once
#include <cstdint>

// CRT OSD's easter egg: a fridge magnet that leaves colour blotches on the
// picture, and a Degauss button that wobbles them away.
//
// The tube's magnetisation is a small grid of 2D vectors over the picture
// (picture UV space).  The CRT shader reads it as a texture: the direction
// picks the colour the beams land on and the length how strongly.  A degauss
// shakes the picture, swirls colour through it and fades the grid out, with
// the hum of the degaussing coil.
namespace Degauss {
    constexpr int kFieldW = 64;
    constexpr int kFieldH = 36;

    // Magnet held at (u, v) in picture UV for dt seconds.  aspect = width / height.
    void Magnetise(float u, float v, float aspect, float dt);

    // Presses the degauss button.  Pressed again before the coil has cooled
    // (a few seconds) it only manages a weak one.  sound: play the hum.
    void Start(bool sound);

    // Advance by dt seconds.  Call once a frame.
    void Update(float dt);

    // The grid, kFieldW * kFieldH RG pairs, row by row; Version() changes
    // whenever it does.
    const float* Field();
    uint64_t     Version();
    bool         Magnetised();   // anything on the grid

    // For the shader: how strong the degauss effect is now (0 when idle),
    // and seconds since it started.
    float Amount();
    float Time();
}
