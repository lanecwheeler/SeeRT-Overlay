#include "Degauss.h"
#include <Windows.h>
#include <mmsystem.h>   // PlaySound
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace {

constexpr float kPi         = 3.14159265f;
constexpr float kDuration   = 2.0f;    // seconds the effect lasts
constexpr float kDecay      = 0.38f;   // seconds — how fast the coil current dies away
constexpr float kWipe       = 1.0f;    // seconds over which the magnetisation is shaken out
constexpr float kCooldown   = 4.f;     // seconds for the coil's thermistor to cool
constexpr float kMaxField   = 1.25f;   // magnetisation saturates

float    s_field[Degauss::kFieldW * Degauss::kFieldH * 2] = {};
uint64_t s_version   = 1;
bool     s_magnetised = false;

float s_time      = 0.f;     // seconds since the last degauss started
float s_strength  = 0.f;     // that degauss's strength, 0–1
bool  s_running   = false;
float s_sinceLast = kCooldown;   // seconds since the last press (the coil starts cool)

// The playing sound.  Never freed, so quitting mid-hum can't pull it out
// from under the sound player.
std::vector<char>& s_wave = *new std::vector<char>;

// Coil current envelope: switches on hard, then dies away.
float Envelope(float t) {
    if (t < 0.03f) return t / 0.03f;
    return std::exp(-(t - 0.03f) / kDecay);
}

// "Thunk ... BWOMMMmmm" — the relay, then mains hum and its harmonics
// rattling the coil and the mask, fading with the current.  Mono 16-bit WAV.
void PlayHum(float strength) {
    PlaySoundW(nullptr, nullptr, 0);   // stop anything still playing before replacing it

    constexpr int   kRate    = 44100;
    constexpr float kSeconds = 1.7f;
    const int n = static_cast<int>(kRate * kSeconds);
    std::vector<int16_t> pcm(n);

    uint32_t noise = 0x1234567u;
    for (int i = 0; i < n; i++) {
        const float t = static_cast<float>(i) / kRate;

        // Relay: a click and a low thump.
        noise = noise * 1664525u + 1013904223u;
        const float white = static_cast<float>(noise >> 9) / static_cast<float>(1u << 23) * 2.f - 1.f;
        float s = white * std::exp(-t / 0.004f) * 0.5f
                + std::sin(2.f * kPi * 70.f * t) * std::exp(-t / 0.05f) * 0.6f;

        // Hum: 60 Hz and harmonics (small speakers only play the harmonics),
        // with a little wobble as the coil shakes, driven into a soft buzz.
        const float env  = (t < 0.06f) ? 0.f : Envelope(t - 0.06f);
        const float wob  = 1.f + 0.004f * std::sin(2.f * kPi * 5.f * t);
        const float ph   = 2.f * kPi * 60.f * wob * t;
        float hum = std::sin(ph)        * 0.9f + std::sin(ph * 2.f) * 0.8f
                  + std::sin(ph * 3.f)  * 0.6f + std::sin(ph * 4.f) * 0.45f
                  + std::sin(ph * 6.f)  * 0.3f + std::sin(ph * 8.f) * 0.18f;
        hum = std::tanh(hum * 1.4f) * 0.75f;
        // The mask ringing as it's shaken.
        hum += std::sin(2.f * kPi * 310.f * t) * 0.12f * std::exp(-t / 0.5f);
        s += hum * env;

        // Fade in and out over a few ms, so it never clicks.
        const float edge = std::min(1.f, std::min(t, kSeconds - t) / 0.003f);
        pcm[i] = static_cast<int16_t>(std::clamp(s * strength * edge * 0.42f, -1.f, 1.f) * 32767.f);
    }

    // WAV header + data.
    const uint32_t dataBytes = static_cast<uint32_t>(n * sizeof(int16_t));
    s_wave.assign(44 + dataBytes, 0);
    char* p = s_wave.data();
    auto u32 = [&](size_t at, uint32_t v) { std::memcpy(p + at, &v, 4); };
    auto u16 = [&](size_t at, uint16_t v) { std::memcpy(p + at, &v, 2); };
    std::memcpy(p, "RIFF", 4);      u32(4, 36 + dataBytes);
    std::memcpy(p + 8, "WAVEfmt ", 8);
    u32(16, 16); u16(20, 1); u16(22, 1);                 // PCM, mono
    u32(24, kRate); u32(28, kRate * 2); u16(32, 2); u16(34, 16);
    std::memcpy(p + 36, "data", 4); u32(40, dataBytes);
    std::memcpy(p + 44, pcm.data(), dataBytes);

    PlaySoundW(reinterpret_cast<LPCWSTR>(s_wave.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

}   // namespace

namespace Degauss {

void Magnetise(float u, float v, float aspect, float dt) {
    // The magnet's field leaves a rainbow ring around it: the direction of
    // each cell's magnetisation points away from the magnet, twisted with
    // distance into a spiral, so the colour turns as you go round and out.
    constexpr float kRadius = 0.11f;    // picture heights
    constexpr float kRate   = 4.0f;     // per second, at the centre
    bool changed = false;
    for (int y = 0; y < kFieldH; y++) {
        for (int x = 0; x < kFieldW; x++) {
            const float cu = (x + 0.5f) / kFieldW, cv = (y + 0.5f) / kFieldH;
            const float dx = (cu - u) * aspect, dy = cv - v;
            const float d2 = dx * dx + dy * dy;
            const float w  = std::exp(-d2 / (kRadius * kRadius));
            if (w < 0.01f) continue;
            const float d     = std::sqrt(d2);
            const float angle = std::atan2(dy, dx) + d * 28.f;
            float* f = &s_field[(y * kFieldW + x) * 2];
            f[0] += std::cos(angle) * w * kRate * dt;
            f[1] += std::sin(angle) * w * kRate * dt;
            const float len = std::sqrt(f[0] * f[0] + f[1] * f[1]);
            if (len > kMaxField) { f[0] *= kMaxField / len; f[1] *= kMaxField / len; }
            changed = true;
        }
    }
    if (changed) { s_version++; s_magnetised = true; }
}

void Start(bool sound) {
    // A coil that hasn't cooled since the last press only gives a weak pulse.
    const float recovered = std::clamp(s_sinceLast / kCooldown, 0.f, 1.f);
    s_strength  = std::max(0.08f, recovered * recovered);
    s_sinceLast = 0.f;
    s_time      = 0.f;
    s_running   = true;
    if (sound) PlayHum(s_strength);
}

void Update(float dt) {
    s_sinceLast += dt;
    if (!s_running) return;
    const float before = s_time;
    s_time += dt;

    // The fading coil current shakes the mask's magnetisation out over the
    // first second.  A full degauss clears the grid; a weak one leaves most of it.
    if (s_magnetised && before < kWipe) {
        const float step = std::min(s_time, kWipe) - before;
        const float keep = std::pow(1.f - s_strength * 0.999f, step / kWipe);
        for (float& f : s_field) f *= keep;
        if (s_time >= kWipe && s_strength > 0.95f) {
            std::fill(std::begin(s_field), std::end(s_field), 0.f);
            s_magnetised = false;
        }
        s_version++;
    }
    if (s_time > kDuration) s_running = false;
}

const float* Field()      { return s_field; }
uint64_t     Version()    { return s_version; }
bool         Magnetised() { return s_magnetised; }

float Amount() { return s_running ? s_strength * Envelope(s_time) : 0.f; }
float Time()   { return s_time; }

}   // namespace Degauss
