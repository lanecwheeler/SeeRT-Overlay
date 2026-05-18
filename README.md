# SeeRT Overlay

A Windows CRT monitor overlay that applies a real-time shader filter over any window or monitor. The filter runs in a transparent, click-through DirectComposition overlay — the target application sees no performance impact and requires no modification.

## Features

**Shader pipeline** (RetroArch-inspired, gamma-correct)
- Barrel/pincushion distortion with zoom compensation
- Gaussian beam scanlines with luminance-dependent spread
- Shadow mask — aperture grille, shadow mask dots, and slot mask patterns (anti-aliased)
- Phosphor bloom and diffusion
- RGB convergence error (chromatic aberration)
- Vignette, hum bar, flicker, and TV static
- Phosphor tint (R/G/B) for monochrome terminal looks
- Colour grading — brightness, contrast, saturation

**Overlay**
- Captures any window or monitor via Windows Graphics Capture
- Click-through when settings panel is closed; receives input when open
- Multi-monitor: add secondary overlays to additional monitors
- Custom CRT cursor rendered inside the shader (barrel-distorted with the image)
- Shake mouse to reveal cursor and settings toggle button
- System tray icon

**Settings**
- ImGui settings panel with multiple UI themes
- 11 built-in presets + save/load/reorder user presets
- Settings persist to `%APPDATA%\SeeRT Overlay\settings.json`
- Audio loopback capture (optional)

## Built-in Presets

| Preset | Character |
|---|---|
| Default | Balanced starting point |
| Early 2000s Laptop | Subtle, LCD-era hint of CRT |
| Arcade | Bold cabinet look — heavy scanlines, strong mask |
| Green Phosphor | P31 monochrome terminal |
| Amber Phosphor | P3 amber/orange terminal |
| Heavy CRT | Degraded 1980s boxy TV |
| crt-geom | Classic RetroArch reference implementation |
| crt-lottes | Tim Lottes' shader — crisp, fine slot mask |
| Sony PVM | Professional reference monitor, minimal artifacts |
| Trinitron | Flat aperture-grille tube, SNES/PS1 era |
| NTSC Composite | Degraded consumer signal with convergence error |

## Requirements

- Windows 10 or later
- [Visual Studio 2022+ Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022) with the **Desktop development with C++** workload
- [Windows SDK 10.0.26100+](https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/)
- [vcpkg](https://github.com/microsoft/vcpkg) installed and available (e.g. `C:/vcpkg`)

## Building

1. Clone the repo and open a terminal in the project root.

2. If vcpkg is not at `C:/vcpkg`, edit `CMakePresets.json` and update `toolchainFile` to point to your vcpkg installation.

3. Configure and build:
   ```bash
   cmake --preset default
   cmake --build --preset default
   ```

4. The executable is at `build/bin/Release/SeeRT Overlay.exe`. Shader `.cso` files and font assets are copied next to it automatically.

Dependencies (`imgui`, `nlohmann-json`) are installed automatically from `vcpkg.json` during the configure step — no manual `vcpkg install` needed.

## Tech Stack

| Layer | Technology |
|---|---|
| Language | C++20 |
| Renderer | Direct3D 11 |
| Shaders | HLSL 5.0 (compiled via fxc.exe) |
| Screen capture | Windows Graphics Capture (C++/WinRT) |
| UI | Dear ImGui |
| Composition | DirectComposition |
| JSON | nlohmann/json |
| Package manager | vcpkg (manifest mode) |
