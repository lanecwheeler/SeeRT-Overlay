// CRT Monitor Overlay — combined vertex + pixel shader
// Compiled as VS_5_0 (entry: VS_Main) and PS_5_0 (entry: PS_Main).
//
// Techniques adapted from the RetroArch / libretro shader library:
//   • Beam scanlines in curved picture space: each output pixel blends the two
//     nearest lines, each a Gaussian beam that widens with brightness
//     (crt-geom, crt-guest-advanced), normalised to unit area so scanlines
//     redistribute light rather than remove it.
//   • Optional per-line colour resolve, so each line carries one colour from
//     its band of the picture like a low-resolution signal (crt-easymode /
//     crt-geom sample per source line).
//   • Separate CRT and display gamma (crt-geom: 2.4 in, 2.2 out).
//   • Pixel-aligned phosphor masks with lit-phosphor brightness compensation
//     (crt-lottes maskDark/maskLight), including a staggered slot mask
//     (crt-guest-advanced) and the 2-pixel magenta/green mask.
//   • Rounded, anti-aliased tube corners (crt-geom cornersize).
//   • Convergence error that grows toward the screen edges (guest dctype).
//   • Glow and halation from separable Gaussian blurs of a downsampled,
//     linearised picture (crt-guest-advanced's glow / bloom passes).
//   • Composite colour bleed: per-component YIQ bandwidth limits (GTU).
//   • Phosphor persistence (guest afterglow) and raster bloom (guest BLOOM).
//
// Entry points: VS_Main, VS_Sprite, PS_Main (CRT), PS_Blit, and the
// pre-passes PS_Composite, PS_Persist, PS_GlowDown, PS_HaloDown, PS_Blur,
// PS_LumDown, PS_LumSmooth.

// ---------------------------------------------------------------------------
// Constant buffer — must match the CRTSettings C++ struct exactly.
// 44 floats = 176 bytes (11 × 16).
// ---------------------------------------------------------------------------
cbuffer CRTParams : register(b0) {
    float curvatureX;       // 0–0.5
    float curvatureY;       // 0–0.5
    float zoom;             // 1.0–2.0
    float scanlineIntensity;// 0–1  flat picture → full beam profile

    float scanlineCount;    // 200–1200 lines across the picture height
    float vignetteStrength; // 0–1
    float glowStrength;     // 0–2
    float brightness;       // 0.5–2.0

    float contrast;         // 0.5–2.0
    float saturation;       // 0–2
    float flickerIntensity; // 0–1
    float flickerSpeed;     // 1–30 Hz

    float time;             // seconds since app start
    float flickerRandomness;// 0–1  smooth sines → random noise
    float humBarIntensity;  // 0–1
    float humBarSpeed;      // 0–2 screen heights/s

    float diffuseStrength;  // 0–2
    float diffuseThreshold; // 0–1  gamma-space luminance cutoff
    float phosphorR;        // phosphor tint (1,1,1 = no tint)
    float phosphorG;

    float phosphorB;
    float staticIntensity;    // 0–1
    float staticSpeed;        // 1–60 fps
    float shadowMaskIntensity;// 0–1

    float shadowMaskScale;    // 1–4 px per phosphor stripe (rounded)
    float shadowMaskType;     // 0=aperture grille, 1=shadow mask, 2=slot mask, 3=magenta/green
    float humBarFrequency;    // 0.5–20 visible bands
    float convergence;        // 0–1  RGB horizontal misalignment

    float crtGamma;           // 1.8–3.0 CRT decoding gamma
    float displayGamma;       // 1.8–3.0 output encoding gamma
    float beamWidth;          // 0.1–0.5 beam sigma of dark lines, in line heights
    float beamDynamics;       // 0–1  how much brighter lines widen

    float lineResolve;        // 0–1  per-line colour resolve
    float maskCompensation;   // 0–1  lit-phosphor brightness compensation
    float cornerRadius;       // 0–0.25 rounded corners, fraction of picture height
    float chromaBleed;        // 0–1  composite chroma bandwidth limit

    float signalSoftness;     // 0–1  luma bandwidth limit
    float glowRadius;         // 1–30   px at 1080p (used on the CPU to size the blur)
    float halationRadius;     // 10–150 px at 1080p (used on the CPU to size the blur)
    float persistence;        // 0–1  phosphor afterglow, 0–250 ms decay time

    float rasterBloom;        // 0–1  picture grows with scene brightness
    float frameDelta;         // seconds since the previous frame
    float shaderPad0;
    float shaderPad1;
};

// ---------------------------------------------------------------------------
// Textures + sampler
//   t0 — the picture (gamma-encoded): the capture, or the composite-filtered
//        signal.  Also the input of the pre-passes below.
//   t1 — settings UI + cursor, premultiplied alpha (gamma-encoded).  Kept
//        separate so per-line resolve never smears the menu text.  Unbound
//        (reads as zero) for the stream output.
//   t2 — glow: linear light, 1/4 size, Gaussian-blurred.   Zero when off.
//   t3 — halation: linear light above the threshold, 1/8 size, wide blur.
//   t4 — 1×1 smoothed average scene luminance (raster bloom).  Zero when off.
//   t5 — previous frame's history, read by the persistence / luminance
//        pre-passes only.
// ---------------------------------------------------------------------------
Texture2D    gCaptureTexture : register(t0);
Texture2D    gUITexture      : register(t1);
Texture2D    gGlowTexture    : register(t2);
Texture2D    gHaloTexture    : register(t3);
Texture2D    gAvgLumTexture  : register(t4);
Texture2D    gHistoryTexture : register(t5);
SamplerState gLinearClamp    : register(s0);

// Size of the luminance-averaging buffer (its last mip is 1×1).  Must match
// kLumSize in D3DRenderer.cpp.
static const float kLumSize     = 64.0;
static const float kLumLastMip  = 6.0;

// Per-pass parameters for PS_Blur.
cbuffer BlurParams : register(b2) {
    float2 blurStep;   // UV offset between taps, along the blur direction
    float  blurSigma;  // Gaussian sigma, measured in taps
    float  blurPad;
};

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// Scale of the glow / halation light added at full slider strength.
static const float kGlowGain = 0.25;
static const float kHaloGain = 0.6;

// ---------------------------------------------------------------------------
// Vertex shader — SV_VertexID full-screen triangle, no vertex buffer needed.
// ---------------------------------------------------------------------------
struct VS_Output {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VS_Output VS_Main(uint id : SV_VertexID) {
    float2 positions[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    float2 uvs[3]       = { float2( 0.0,  1.0), float2( 0.0,-1.0), float2(2.0,  1.0) };

    VS_Output o;
    o.pos = float4(positions[id], 0.0, 1.0);
    o.uv  = uvs[id];
    return o;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
float2 TexSize() {
    float w, h;
    gCaptureTexture.GetDimensions(w, h);
    return float2(w, h);
}

// Cubic B-spline sampling from four bilinear taps (GPU Gems 2, ch. 20).
// Used to enlarge the low-resolution glow and halation buffers: plain
// bilinear enlargement creases at every source texel, which shows as a faint
// grid / stair-steps in smooth glow; the B-spline is smooth across texels.
float3 SampleBSpline(Texture2D tex, float2 uv) {
    float2 size;
    tex.GetDimensions(size.x, size.y);
    float2 st = uv * size - 0.5;
    float2 i  = floor(st);
    float2 f  = st - i;
    float2 f2 = f * f;
    float2 f3 = f2 * f;

    float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    float2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    float2 w3 = f3 / 6.0;

    // Each bilinear tap blends two texels in the right ratio.
    float2 g0 = w0 + w1;
    float2 g1 = w2 + w3;
    float2 p0 = (i - 1.0 + w1 / g0 + 0.5) / size;
    float2 p1 = (i + 1.0 + w3 / g1 + 0.5) / size;

    return g0.y * (g0.x * tex.Sample(gLinearClamp, float2(p0.x, p0.y)).rgb +
                   g1.x * tex.Sample(gLinearClamp, float2(p1.x, p0.y)).rgb)
         + g1.y * (g0.x * tex.Sample(gLinearClamp, float2(p0.x, p1.y)).rgb +
                   g1.x * tex.Sample(gLinearClamp, float2(p1.x, p1.y)).rgb);
}

// Signal → light with the tube's gamma; light → signal with the monitor's.
float3 ToLinear(float3 c) { return pow(max(c, 0.0001), crtGamma); }
float3 ToGamma (float3 c) { return pow(max(c, 0.0001), 1.0 / displayGamma); }

// ---------------------------------------------------------------------------
// Raster bloom — a CRT's high-voltage supply sags on bright scenes, weakening
// the beam's stiffness so the deflection (and the picture) grows slightly.
// Up to 5% larger on an all-white screen at full strength.
// ---------------------------------------------------------------------------
float RasterScale() {
    if (rasterBloom < 0.001) return 1.0;
    float avgLum = gAvgLumTexture.Load(int3(0, 0, 0)).r;
    return 1.0 + rasterBloom * 0.05 * saturate(avgLum);
}

// ---------------------------------------------------------------------------
// Barrel / pincushion distortion.
// ---------------------------------------------------------------------------
float2 BarrelDistort(float2 uv) {
    float2 c = uv * 2.0 - 1.0;
    float r2 = c.x * c.x + c.y * c.y;
    c.x *= 1.0 + curvatureX * r2;
    c.y *= 1.0 + curvatureY * r2;
    c /= zoom * RasterScale();
    return c * 0.5 + 0.5;
}

// ---------------------------------------------------------------------------
// Signed distance to the rounded edge of the picture, in picture heights
// (negative inside).  Rounded-box SDF in aspect-corrected space so corners
// are circular on any monitor shape.
// ---------------------------------------------------------------------------
float CornerDistance(float2 uv) {
    float2 sz     = TexSize();
    float  aspect = sz.x / sz.y;
    float  r      = clamp(cornerRadius, 0.0, 0.5);
    float2 p      = abs(uv - 0.5) * float2(aspect, 1.0);
    float2 q      = p - (float2(0.5 * aspect, 0.5) - r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// ---------------------------------------------------------------------------
// Sample the picture (gamma-encoded) with optional RGB convergence error.
// Misconvergence on a real tube is worst at the edges, so the offset grows
// from 40% at the centre to 100% at the sides.
// ---------------------------------------------------------------------------
float3 SampleSource(float2 uv) {
    float3 c;
    if (convergence < 0.001) {
        c = gCaptureTexture.Sample(gLinearClamp, uv).rgb;
    } else {
        float  edge = 0.4 + 0.6 * abs(uv.x * 2.0 - 1.0);
        float2 off  = float2(convergence * 0.003 * edge, 0.0);
        c.r = gCaptureTexture.Sample(gLinearClamp, uv + off).r; // red shifts right
        c.g = gCaptureTexture.Sample(gLinearClamp, uv      ).g;
        c.b = gCaptureTexture.Sample(gLinearClamp, uv - off).b; // blue shifts left
    }
    return c;
}

// Average of the picture across one line's band — the single colour a
// low-resolution signal would carry on that line.  Three bilinear taps.
float3 SampleBand(float2 uv, float bandH) {
    float dy = bandH / 3.0;
    return (SampleSource(uv - float2(0.0, dy)) +
            SampleSource(uv) +
            SampleSource(uv + float2(0.0, dy))) / 3.0;
}

// ---------------------------------------------------------------------------
// Colour grade.
// Contrast is a power curve pivoting on linear mid-grey (0.18), so blacks stay
// black.  Brightness, saturation and phosphor tint are linear, so GradeLight
// can equally be applied to summed light (bloom / diffusion).
// ---------------------------------------------------------------------------
float3 Contrast(float3 c) { return 0.18 * pow(max(c, 0.0) / 0.18, contrast); }

float3 GradeLight(float3 c) {
    c *= brightness;
    float lum = dot(c, kLuma);
    c = lerp(float3(lum, lum, lum), c, saturation);
    return c * float3(phosphorR, phosphorG, phosphorB);
}

float3 Grade(float3 c) { return GradeLight(Contrast(c)); }

// Picture signal for one line: optionally resolved toward the line's band
// colour, with the UI layer composited on top (premultiplied alpha, in signal
// space — as if the menu were part of the picture), then decoded and graded.
float3 LineColor(float3 nativeG, float2 lineUV, float bandH, float4 ui) {
    float3 g = nativeG;
    if (lineResolve > 0.001)
        g = lerp(nativeG, SampleBand(lineUV, bandH), lineResolve);
    g = g * (1.0 - ui.a) + ui.rgb;
    return Grade(ToLinear(g));
}

// ---------------------------------------------------------------------------
// Beam scanlines, in picture (curved) space.
//
// y runs in line units with integers at line centres.  The pixel lies between
// line n and n+1 (f = 0 at n, 1 at n+1); each line contributes a Gaussian
// beam whose sigma widens with that line's brightness (higher beam current).
// Each beam is normalised to unit area, so the average light of a flat field
// is preserved — dark gaps are paid for by brighter beam centres.
//
// lineDensity (lines per output pixel) fades the scanlines out as they
// approach the pixel grid, where they can only alias into moiré.
// ---------------------------------------------------------------------------
float3 Beam(float2 uv, float3 nativeG, float4 ui, float lineDensity) {
    float strength = scanlineIntensity * saturate((0.6 - lineDensity) / 0.25);

    if (strength < 0.001 && lineResolve < 0.001)
        return Grade(ToLinear(nativeG * (1.0 - ui.a) + ui.rgb));

    float lines = max(scanlineCount, 1.0);
    float bandH = 1.0 / lines;
    float y     = uv.y * lines - 0.5;
    float n     = floor(y);
    float f     = y - n;

    float3 c0 = LineColor(nativeG, float2(uv.x, (n + 0.5) * bandH), bandH, ui);
    float3 c1 = LineColor(nativeG, float2(uv.x, (n + 1.5) * bandH), bandH, ui);

    // No scanlines: neighbouring lines blend smoothly into each other.
    float3 flat = lerp(c0, c1, smoothstep(0.0, 1.0, f));
    if (strength < 0.001) return flat;

    const float kInvSqrt2Pi = 0.39894228;
    float3 s0 = min(beamWidth * (1.0 + beamDynamics * saturate(c0)), 0.5);
    float3 s1 = min(beamWidth * (1.0 + beamDynamics * saturate(c1)), 0.5);
    float3 w0 = exp(-0.5 * f * f / (s0 * s0)) * kInvSqrt2Pi / s0;
    float3 w1 = exp(-0.5 * (1.0 - f) * (1.0 - f) / (s1 * s1)) * kInvSqrt2Pi / s1;

    return lerp(flat, c0 * w0 + c1 * w1, strength);
}

// ---------------------------------------------------------------------------
// Vignette — radial edge darkening.
// ---------------------------------------------------------------------------
float Vignette(float2 uv) {
    float2 d = uv - 0.5;
    return 1.0 - dot(d, d) * vignetteStrength * 4.0;
}

// ===========================================================================
// Pre-passes — run before the CRT pass, each reading t0.
// ===========================================================================

// ---------------------------------------------------------------------------
// Composite signal (after GTU): convert to YIQ, then low-pass each component
// horizontally with its own bandwidth — luma lightly, chroma heavily (NTSC
// gives I and Q far less bandwidth than Y).  Colours smear sideways while
// detail stays readable.  Bandwidths are in samples per picture width, at
// full strength: Y 256, I 83, Q 25 (GTU's defaults).  Operates in signal
// (gamma) space, as the real signal does.
// ---------------------------------------------------------------------------
static const float3x3 kRGBtoYIQ = {
    0.299,     0.587,     0.114,
    0.595716, -0.274453, -0.321263,
    0.211456, -0.522591,  0.311135,
};
static const float3x3 kYIQtoRGB = {
    1.0,  0.9563,  0.6210,
    1.0, -0.2721, -0.6474,
    1.0, -1.1070,  1.7046,
};

// Horizontal Gaussian of the YIQ signal with the given sigma (pixels).
// Nine taps spaced at sigma/2 cover ±2 sigma.
float3 FilterYIQ(float2 uv, float sigmaPx, float texelX) {
    float3 yiq;
    if (sigmaPx < 0.5) {
        yiq = mul(kRGBtoYIQ, gCaptureTexture.Sample(gLinearClamp, uv).rgb);
    } else {
        float  spacing = sigmaPx * 0.5;
        float3 acc     = float3(0, 0, 0);
        float  wsum    = 0.0;
        [unroll]
        for (int i = -4; i <= 4; i++) {
            float w  = exp(-0.5 * (i * spacing) * (i * spacing) / (sigmaPx * sigmaPx));
            float3 c = gCaptureTexture.Sample(gLinearClamp, uv + float2(i * spacing * texelX, 0)).rgb;
            acc  += mul(kRGBtoYIQ, c) * w;
            wsum += w;
        }
        yiq = acc / wsum;
    }
    return yiq;
}

float4 PS_Composite(VS_Output input) : SV_TARGET {
    float width = TexSize().x;
    // Gaussian sigma ≈ half the width of one signal sample.
    float sigY = signalSoftness * 0.5 * width / 256.0;
    float sigI = chromaBleed    * 0.5 * width / 83.0;
    float sigQ = chromaBleed    * 0.5 * width / 25.0;

    float3 yiq;
    yiq.x = FilterYIQ(input.uv, sigY, 1.0 / width).x;
    yiq.y = FilterYIQ(input.uv, sigI, 1.0 / width).y;
    yiq.z = FilterYIQ(input.uv, sigQ, 1.0 / width).z;
    return float4(saturate(mul(kYIQtoRGB, yiq)), 1.0);
}

// ---------------------------------------------------------------------------
// Phosphor persistence (after crt-guest-advanced's afterglow): each pixel
// keeps the brighter of its new value and its previous value decayed
// exponentially over time, so bright things leave fading trails as they
// move while still images are unaffected.  The decay is timed by frameDelta,
// so trails last the same at any refresh rate.  History is half-float, so
// trails fade all the way out instead of sticking at a quantised residue.
//
// Values stay in signal space: decaying light by d is the same as scaling
// the signal by d^(1/gamma), and max() is the same in either space.
// ---------------------------------------------------------------------------
float4 PS_Persist(VS_Output input) : SV_TARGET {
    int3   p     = int3(input.pos.xy, 0);
    float3 cur   = gCaptureTexture.Load(p).rgb;
    float3 prev  = gHistoryTexture.Load(p).rgb;
    float  tau   = max(persistence * 0.25, 0.0001);            // seconds
    float  decay = pow(exp(-frameDelta / tau), 1.0 / crtGamma);
    return float4(max(cur, prev * decay), 1.0);
}

// ---------------------------------------------------------------------------
// Scene luminance for raster bloom: a 64×64 grid of linear-light luminance
// samples (four per cell); the renderer then mip-maps it down to 1×1.
// ---------------------------------------------------------------------------
float4 PS_LumDown(VS_Output input) : SV_TARGET {
    float2 sz  = TexSize();
    float  lum = 0.0;
    [unroll]
    for (int y = 0; y < 2; y++) {
        [unroll]
        for (int x = 0; x < 2; x++) {
            float2 uv = (floor(input.pos.xy) + (float2(x, y) + 0.5) * 0.5) / kLumSize;
            int2   p  = min(int2(uv * sz), int2(sz) - 1);
            lum += dot(ToLinear(gCaptureTexture.Load(int3(p, 0)).rgb), kLuma);
        }
    }
    lum *= 0.25;
    return float4(lum, lum, lum, 1.0);
}

// Temporal smoothing of the average (1×1 target): the supply responds over
// ~150 ms, so the picture breathes rather than jitters.
float4 PS_LumSmooth(VS_Output input) : SV_TARGET {
    float cur   = gCaptureTexture.SampleLevel(gLinearClamp, float2(0.5, 0.5), kLumLastMip).r;
    float prev  = gHistoryTexture.Load(int3(0, 0, 0)).r;
    float alpha = 1.0 - exp(-frameDelta / 0.15);
    float lum   = lerp(prev, cur, alpha);
    return float4(lum, lum, lum, 1.0);
}

// ---------------------------------------------------------------------------
// Glow source: decode to linear light and box-filter to 1/4 size.
//
// Each of the 16 texels is decoded *before* averaging.  Bilinear taps would
// average in signal space first, and because gamma is non-linear that loses
// light unevenly — a small bright detail glowed up to ~7× less when it
// straddled tap footprints than when it fit inside one, showing the 4-pixel
// block grid in the glow.
// ---------------------------------------------------------------------------
float4 PS_GlowDown(VS_Output input) : SV_TARGET {
    int2   maxTexel = int2(TexSize()) - 1;
    int2   base     = int2(input.pos.xy) * 4;   // this 1/4-size pixel's 4×4 block
    float3 c        = float3(0, 0, 0);
    [unroll]
    for (int y = 0; y < 4; y++) {
        [unroll]
        for (int x = 0; x < 4; x++)
            c += ToLinear(gCaptureTexture.Load(int3(min(base + int2(x, y), maxTexel), 0)).rgb);
    }
    return float4(c / 16.0, 1.0);
}

// ---------------------------------------------------------------------------
// Halation source: from the 1/4-size glow source to 1/8 size (one bilinear
// tap = 2×2 box), keeping only light above the threshold.  The threshold is
// set in signal space (as on the slider) and decoded with the CRT gamma.
// ---------------------------------------------------------------------------
float4 PS_HaloDown(VS_Output input) : SV_TARGET {
    float3 c         = gCaptureTexture.Sample(gLinearClamp, input.uv).rgb;
    float  linThresh = pow(max(diffuseThreshold, 0.0001), crtGamma);
    float  gate      = saturate((dot(c, kLuma) - linThresh) / max(1.0 - linThresh, 0.001));
    return float4(c * gate, 1.0);
}

// ---------------------------------------------------------------------------
// Separable Gaussian blur (one direction per pass).  13 taps, ±3 sigma.
// ---------------------------------------------------------------------------
float4 PS_Blur(VS_Output input) : SV_TARGET {
    float3 acc  = float3(0, 0, 0);
    float  wsum = 0.0;
    [unroll]
    for (int i = -6; i <= 6; i++) {
        float w = exp(-0.5 * (i * i) / (blurSigma * blurSigma));
        acc  += gCaptureTexture.Sample(gLinearClamp, input.uv + blurStep * i).rgb * w;
        wsum += w;
    }
    return float4(acc / wsum, 1.0);
}

// ---------------------------------------------------------------------------
// Flicker — three incommensurate sines + optional hash noise.
// ---------------------------------------------------------------------------
float Flicker() {
    if (flickerIntensity < 0.001) return 1.0;
    float t = time * flickerSpeed;

    float smooth = sin(t * 6.28318530)                * 0.60
                 + sin(t * 6.28318530 * 1.41421 + 1.1) * 0.25
                 + sin(t * 6.28318530 * 0.61803 + 2.3) * 0.15;

    float n   = floor(t);
    float rnd = frac(sin(n * 127.1 + 311.7) * 43758.5453) * 2.0 - 1.0;

    float f = lerp(smooth, rnd, flickerRandomness);
    return 1.0 + flickerIntensity * f * 0.15;
}

// ---------------------------------------------------------------------------
// Hum bar — mains-frequency interference, scrolling upward over time.
// ---------------------------------------------------------------------------
float HumBar(float uvY) {
    if (humBarIntensity < 0.001) return 1.0;

    float phase = frac(uvY + time * humBarSpeed);
    float wave  = sin(phase * 6.28318530 * humBarFrequency)
                + sin(phase * 6.28318530 * humBarFrequency * 2.0 + 1.3) * 0.35;

    return 1.0 + wave * humBarIntensity * 0.12;
}

// ---------------------------------------------------------------------------
// Phosphor mask — per-channel gain at screen pixel px.
//
// Patterns are aligned to whole screen pixels (crt-lottes, guest), so each
// phosphor stripe lands exactly on pixels instead of smearing into grey.
// Unlit phosphors drop to (1 - intensity).  Lit ones are boosted by up to
// the amount that keeps the pattern's average at 1 (maskCompensation), the
// way crt-lottes' maskLight > 1 offsets maskDark < 1.
// ---------------------------------------------------------------------------
float3 ShadowMask(float2 px) {
    if (shadowMaskIntensity < 0.001) return float3(1, 1, 1);

    float  s       = shadowMaskIntensity;
    float  size    = max(1.0, round(shadowMaskScale));
    int    type    = (int)round(shadowMaskType);
    float3 lit;            // 1 where this channel's phosphor covers the pixel
    float  litFrac;        // share of the pattern each channel is lit in
    float  rowGain = 1.0;  // slot-mask breaks

    if (type == 3) {
        // Magenta / green — a 2-stripe aperture grille; the sharpest pattern
        // at 1080p because it needs only two pixels per triad.
        lit     = (fmod(floor(px.x / size), 2.0) < 1.0) ? float3(1, 0, 1) : float3(0, 1, 0);
        litFrac = 0.5;
    } else {
        float x = px.x;
        if (type == 1 && fmod(floor(px.y / size), 2.0) >= 1.0)
            x += round(1.5 * size);    // shadow mask: odd rows shift half a triad
        float i = fmod(floor(x / size), 3.0);
        lit     = (i < 1.0) ? float3(1, 0, 0) : ((i < 2.0) ? float3(0, 1, 0) : float3(0, 0, 1));
        litFrac = 1.0 / 3.0;

        if (type == 2) {
            // Slot mask — stripes broken into slots, with neighbouring
            // triads' breaks staggered by half a slot.
            float slotH   = 4.0 * size;
            float triad   = floor(px.x / (3.0 * size));
            float y       = px.y + fmod(triad, 2.0) * slotH * 0.5;
            float gapFrac = size / slotH;
            rowGain = (fmod(floor(y), slotH) < size)
                ? 1.0 - s
                : lerp(1.0, 1.0 + s * gapFrac / (1.0 - gapFrac), maskCompensation);
        }
    }

    float unlit   = 1.0 - s;
    float litGain = lerp(1.0, 1.0 + s * (1.0 - litFrac) / litFrac, maskCompensation);
    return lerp(float3(unlit, unlit, unlit), float3(litGain, litGain, litGain), lit) * rowGain;
}

// ---------------------------------------------------------------------------
// TV static — per-pixel additive grain that flips at staticSpeed Hz.
// Applied in signal space after re-encoding, like real CRT noise on the display.
// ---------------------------------------------------------------------------
float TVStatic(float2 screenPos) {
    if (staticIntensity < 0.001) return 0.0;
    float  t    = floor(time * staticSpeed);
    float2 seed = screenPos + float2(t * 17.3, t * 31.7);
    float  noise = frac(sin(dot(seed, float2(127.1, 311.7))) * 43758.5453);
    return (noise * 2.0 - 1.0) * staticIntensity * 0.4;
}

// ---------------------------------------------------------------------------
// Blit pixel shader — pass-through, used in pass 1 to copy the capture.
// ---------------------------------------------------------------------------
float4 PS_Blit(VS_Output input) : SV_TARGET {
    return gCaptureTexture.Sample(gLinearClamp, input.uv);
}

// ---------------------------------------------------------------------------
// Sprite vertex shader — positions a textured quad via b1 constant buffer.
// ---------------------------------------------------------------------------
cbuffer SpriteParams : register(b1) {
    float2 spriteTL; // NDC top-left     (min-x, max-y)
    float2 spriteBR; // NDC bottom-right (max-x, min-y)
};

VS_Output VS_Sprite(uint id : SV_VertexID) {
    float2 pos[6] = {
        float2(spriteTL.x, spriteTL.y),
        float2(spriteBR.x, spriteTL.y),
        float2(spriteTL.x, spriteBR.y),
        float2(spriteBR.x, spriteTL.y),
        float2(spriteBR.x, spriteBR.y),
        float2(spriteTL.x, spriteBR.y),
    };
    float2 uv[6] = {
        float2(0,0), float2(1,0), float2(0,1),
        float2(1,0), float2(1,1), float2(0,1),
    };
    VS_Output o;
    o.pos = float4(pos[id], 0, 1);
    o.uv  = uv[id];
    return o;
}

// ---------------------------------------------------------------------------
// Main CRT pixel shader
// Pipeline (linear light between decode and re-encode):
//   distort → corner mask → beam scanlines (decode + grade per line, UI
//   composited) → + glow/halation → phosphor mask → hum → vignette →
//   flicker → re-encode → static
// ---------------------------------------------------------------------------
float4 PS_Main(VS_Output input) : SV_TARGET {
    // 1. Barrel distortion.
    float2 uv = BarrelDistort(input.uv);

    // Screen-space derivatives first, while control flow is still uniform.
    float edgeDist    = CornerDistance(uv);
    float edgeAA      = max(fwidth(edgeDist), 1e-5);
    float lineDensity = fwidth(uv.y) * max(scanlineCount, 1.0);

    // 2. Rounded, anti-aliased picture edge.
    float edge = saturate(0.5 - edgeDist / edgeAA);
    if (edge <= 0.0)
        return float4(0.0, 0.0, 0.0, 1.0);

    // 3. Beam scanlines — produces graded linear light, UI included.
    float3 nativeG = SampleSource(uv);
    float4 ui      = gUITexture.Sample(gLinearClamp, uv);
    float3 col     = Beam(uv, nativeG, ui, lineDensity);

    // 4. Glow and halation (pre-blurred linear light) fill the gaps between
    //    lines before the mask is applied.
    float3 light = float3(0, 0, 0);
    if (glowStrength > 0.001)
        light += SampleBSpline(gGlowTexture, uv) * glowStrength * kGlowGain;
    if (diffuseStrength > 0.001)
        light += SampleBSpline(gHaloTexture, uv) * diffuseStrength * kHaloGain;
    col += GradeLight(light);

    // 5. Phosphor mask — sub-pixel phosphor geometry.
    col *= ShadowMask(input.pos.xy);

    // 6. Analogue interference and lens effects.
    col *= HumBar(uv.y);
    col *= Vignette(uv);
    col *= Flicker();
    col *= edge;

    // 7. Re-encode for the monitor.
    col = ToGamma(col);

    // 8. TV static — additive noise in signal space.
    col += TVStatic(input.pos.xy);

    return float4(saturate(col), 1.0);
}
