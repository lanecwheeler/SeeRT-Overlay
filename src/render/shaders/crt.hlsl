// CRT Monitor Overlay — combined vertex + pixel shader
// Compiled as VS_5_0 (entry: VS_Main) and PS_5_0 (entry: PS_Main).
//
// Improvements ported from RetroArch shader library (crt-geom, crt-lottes,
// crt-royale techniques) adapted to HLSL 5.0:
//   • Gamma-correct pipeline: linearize on input, re-encode on output.
//   • Gaussian beam scanlines: physically-based profile that widens for
//     brighter pixels (crt-geom luminance-dependent sigma).
//   • RGB convergence: per-channel horizontal UV offset (crt-lottes).
//   • Anti-aliased shadow mask: fwidth-based smoothstep dissolve at cell edges.

// ---------------------------------------------------------------------------
// Constant buffer — must match the CRTSettings C++ struct exactly.
// 27 user floats + convergence (formerly _pad4) = 28 floats = 112 bytes (7 × 16).
// ---------------------------------------------------------------------------
cbuffer CRTParams : register(b0) {
    float curvatureX;       // 0–0.5
    float curvatureY;       // 0–0.5
    float zoom;             // 1.0–2.0
    float scanlineIntensity;// 0–1

    float scanlineCount;    // 200–1200
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

    float shadowMaskScale;    // 0.5–4 px/cell
    float shadowMaskType;     // 0=aperture grille, 1=shadow mask, 2=slot mask
    float humBarFrequency;    // 0.5–20 visible bands
    float convergence;        // 0–1  RGB horizontal misalignment
};

// ---------------------------------------------------------------------------
// Texture + sampler
// ---------------------------------------------------------------------------
Texture2D    gCaptureTexture : register(t0);
SamplerState gLinearClamp    : register(s0);

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
// Gamma helpers — pow(2.2) approximation, accurate enough for CRT simulation.
// All CRT light math runs in linear space; these bracket the pipeline.
// ---------------------------------------------------------------------------
float3 ToLinear(float3 c) { return pow(max(c, 0.0001), 2.2); }
float3 ToGamma (float3 c) { return pow(max(c, 0.0001), 1.0 / 2.2); }

// ---------------------------------------------------------------------------
// Barrel / pincushion distortion.
// ---------------------------------------------------------------------------
float2 BarrelDistort(float2 uv) {
    float2 c = uv * 2.0 - 1.0;
    float r2 = c.x * c.x + c.y * c.y;
    c.x *= 1.0 + curvatureX * r2;
    c.y *= 1.0 + curvatureY * r2;
    c /= zoom;
    return c * 0.5 + 0.5;
}

// ---------------------------------------------------------------------------
// Sample source with optional horizontal RGB convergence offset (crt-lottes).
// Returned values are still gamma-encoded; caller linearizes afterwards.
// ---------------------------------------------------------------------------
float3 SampleSource(float2 uv) {
    if (convergence < 0.001)
        return gCaptureTexture.Sample(gLinearClamp, uv).rgb;

    // Red shifts right, blue shifts left — classic lateral chromatic aberration.
    float2 off = float2(convergence * 0.003, 0.0);
    float r = gCaptureTexture.Sample(gLinearClamp, uv + off).r;
    float g = gCaptureTexture.Sample(gLinearClamp, uv      ).g;
    float b = gCaptureTexture.Sample(gLinearClamp, uv - off).b;
    return float3(r, g, b);
}

// ---------------------------------------------------------------------------
// Gaussian beam scanlines (crt-geom / crt-lottes technique).
//
// Replaces the old sine-wave approximation with a proper Gaussian that:
//   • operates in linear light space for physically correct darkening
//   • widens for brighter pixels (higher beam current → wider excitation area)
//   • blends smoothly to no-effect at scanlineIntensity = 0
// ---------------------------------------------------------------------------
float3 GaussianScanline(float3 col, float screenY) {
    if (scanlineIntensity < 0.001) return col;

    float lineHeight = 1080.0 / scanlineCount;
    // t: −0.5 = gap centre, 0 = beam centre, +0.5 = gap centre
    float t = frac(screenY / lineHeight) - 0.5;

    // Base beam sigma; bright phosphors spread wider (crt-geom luminance trick).
    float sigma = 0.25;
    float lum   = dot(col, float3(0.2126, 0.7152, 0.0722));
    float adjustedSigma = sigma * (1.0 + 0.40 * lum);

    float gaussian = exp(-t * t / (2.0 * adjustedSigma * adjustedSigma));
    // lerp lets scanlineIntensity 0→1 fade from flat to full Gaussian profile.
    return col * lerp(1.0, gaussian, scanlineIntensity);
}

// ---------------------------------------------------------------------------
// Vignette — radial edge darkening.
// ---------------------------------------------------------------------------
float Vignette(float2 uv) {
    float2 d = uv - 0.5;
    return 1.0 - dot(d, d) * vignetteStrength * 4.0;
}

// ---------------------------------------------------------------------------
// Bloom — 8-tap ring phosphor halation, accumulated in linear space.
// Linearising each tap means bright pixels contribute quadratically more glow,
// matching how phosphor halation actually works.
// ---------------------------------------------------------------------------
float3 Bloom(float2 uv) {
    if (glowStrength < 0.001) return float3(0, 0, 0);

    float r   = 0.004 * glowStrength;
    float3 acc = float3(0, 0, 0);

    acc += ToLinear(gCaptureTexture.Sample(gLinearClamp, uv + float2( r,  0)).rgb);
    acc += ToLinear(gCaptureTexture.Sample(gLinearClamp, uv + float2(-r,  0)).rgb);
    acc += ToLinear(gCaptureTexture.Sample(gLinearClamp, uv + float2( 0,  r)).rgb);
    acc += ToLinear(gCaptureTexture.Sample(gLinearClamp, uv + float2( 0, -r)).rgb);
    float d = r * 0.707;
    acc += ToLinear(gCaptureTexture.Sample(gLinearClamp, uv + float2( d,  d)).rgb);
    acc += ToLinear(gCaptureTexture.Sample(gLinearClamp, uv + float2(-d,  d)).rgb);
    acc += ToLinear(gCaptureTexture.Sample(gLinearClamp, uv + float2( d, -d)).rgb);
    acc += ToLinear(gCaptureTexture.Sample(gLinearClamp, uv + float2(-d, -d)).rgb);

    return (acc / 8.0) * glowStrength * 0.25;
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
// Diffusion bloom — luminance-gated glow, taps linearized for correct blending.
// diffuseThreshold lives in gamma space (as shown on sliders), so it is
// linearized here before comparing against linear-space tap luminance.
// ---------------------------------------------------------------------------
float3 Diffusion(float2 uv) {
    if (diffuseStrength < 0.001) return float3(0, 0, 0);

    float3 acc = float3(0, 0, 0);
    float  r1  = 0.004 * (1.0 + diffuseStrength);
    float  r2  = 0.011 * (1.0 + diffuseStrength);
    float  linThresh = pow(max(diffuseThreshold, 0.0001), 2.2);

    [unroll]
    for (int i = 0; i < 8; i++) {
        float  a   = i * 0.78539816;
        float2 dir = float2(cos(a), sin(a));

        float3 s1   = ToLinear(gCaptureTexture.Sample(gLinearClamp, uv + dir * r1).rgb);
        float  lum1 = dot(s1, float3(0.2126, 0.7152, 0.0722));
        float  g1   = max(0.0, lum1 - linThresh);
        acc += s1 * (g1 * g1) * 0.55;

        float3 s2   = ToLinear(gCaptureTexture.Sample(gLinearClamp, uv + dir * r2).rgb);
        float  lum2 = dot(s2, float3(0.2126, 0.7152, 0.0722));
        float  g2   = max(0.0, lum2 - linThresh);
        acc += s2 * (g2 * g2) * 0.25;
    }

    return acc * diffuseStrength * 0.55;
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
// Shadow mask — three patterns, all with fwidth-based anti-aliasing.
//
// The AA works by replacing hard if/fmod branches with smoothstep transitions
// whose width is set by fwidth(p.x) — the rate of change of the pattern
// coordinate across one screen pixel. When the mask is fine (scale < ~2 px),
// the transitions overlap and the colours average toward neutral gray, exactly
// matching how a real display would look at that scale. (crt-aperture technique)
// ---------------------------------------------------------------------------
float3 ShadowMask(float2 screenPos) {
    if (shadowMaskIntensity < 0.001) return float3(1, 1, 1);

    float2 p  = screenPos / max(shadowMaskScale, 0.1);
    int type  = (int)round(shadowMaskType);
    float3 mask;
    float fw  = fwidth(p.x) * 0.5; // AA half-width in pattern-space units

    // Per-column weights: 1 at column centre, 0 elsewhere, smooth transitions.
    // mask = wr * rCol + wg * gCol + wb * bCol  with each col = (1, .15, .15) etc.
    // Simplified: each channel gets 1.0 inside its column, 0.15 outside.

    if (type == 0) {
        // Aperture grille — vertical RGB stripes (Trinitron-style)
        float xf = fmod(p.x, 3.0);
        float wr = smoothstep(0.0 - fw, 0.0 + fw, xf) * (1.0 - smoothstep(1.0 - fw, 1.0 + fw, xf));
        float wg = smoothstep(1.0 - fw, 1.0 + fw, xf) * (1.0 - smoothstep(2.0 - fw, 2.0 + fw, xf));
        float wb = smoothstep(2.0 - fw, 2.0 + fw, xf) * (1.0 - smoothstep(3.0 - fw, 3.0 + fw, xf));
        mask = float3(wr + (1.0 - wr) * 0.15,
                      wg + (1.0 - wg) * 0.15,
                      wb + (1.0 - wb) * 0.15);

    } else if (type == 1) {
        // Shadow mask — offset dot triads with smooth circular dot falloff
        float2 q  = p;
        if (fmod(floor(q.y), 2.0) >= 1.0) q.x += 1.5;
        float fw2 = fwidth(q.x) * 0.5;
        float xf  = fmod(q.x, 3.0);
        float wr  = smoothstep(0.0 - fw2, 0.0 + fw2, xf) * (1.0 - smoothstep(1.0 - fw2, 1.0 + fw2, xf));
        float wg  = smoothstep(1.0 - fw2, 1.0 + fw2, xf) * (1.0 - smoothstep(2.0 - fw2, 2.0 + fw2, xf));
        float wb  = smoothstep(2.0 - fw2, 2.0 + fw2, xf) * (1.0 - smoothstep(3.0 - fw2, 3.0 + fw2, xf));
        mask = float3(wr + (1.0 - wr) * 0.15,
                      wg + (1.0 - wg) * 0.15,
                      wb + (1.0 - wb) * 0.15);
        // Smooth circular phosphor dot shape within each cell
        float2 cell = frac(q) * 2.0 - 1.0;
        mask *= saturate(1.3 - dot(cell, cell) * 1.8);

    } else {
        // Slot mask — RGB stripes with a smooth horizontal gap every 2 rows
        float xf = fmod(p.x, 3.0);
        float wr = smoothstep(0.0 - fw, 0.0 + fw, xf) * (1.0 - smoothstep(1.0 - fw, 1.0 + fw, xf));
        float wg = smoothstep(1.0 - fw, 1.0 + fw, xf) * (1.0 - smoothstep(2.0 - fw, 2.0 + fw, xf));
        float wb = smoothstep(2.0 - fw, 2.0 + fw, xf) * (1.0 - smoothstep(3.0 - fw, 3.0 + fw, xf));
        mask = float3(wr + (1.0 - wr) * 0.15,
                      wg + (1.0 - wg) * 0.15,
                      wb + (1.0 - wb) * 0.15);
        // Smooth row gap: rows 0 bright, rows 1 dark, with AA at the boundary
        float yf   = frac(p.y * 0.5);
        float fw_y = fwidth(p.y * 0.5);
        mask *= 1.0 - 0.9 * smoothstep(0.5 - fw_y, 0.5 + fw_y, yf);
    }

    return lerp(float3(1, 1, 1), mask, shadowMaskIntensity);
}

// ---------------------------------------------------------------------------
// TV static — per-pixel additive grain that flips at staticSpeed Hz.
// Applied in gamma space after re-encoding, like real CRT noise on the display.
// ---------------------------------------------------------------------------
float TVStatic(float2 screenPos) {
    if (staticIntensity < 0.001) return 0.0;
    float  t    = floor(time * staticSpeed);
    float2 seed = screenPos + float2(t * 17.3, t * 31.7);
    float  noise = frac(sin(dot(seed, float2(127.1, 311.7))) * 43758.5453);
    return (noise * 2.0 - 1.0) * staticIntensity * 0.4;
}

// ---------------------------------------------------------------------------
// Colour grade — brightness / contrast / saturation / phosphor tint.
// Runs in linear space for physically correct light math.
// ---------------------------------------------------------------------------
float3 ColorGrade(float3 col) {
    col *= brightness;
    col  = (col - 0.5) * contrast + 0.5;
    float lum = dot(col, float3(0.2126, 0.7152, 0.0722));
    col  = lerp(float3(lum, lum, lum), col, saturation);
    col *= float3(phosphorR, phosphorG, phosphorB);
    return col;
}

// ---------------------------------------------------------------------------
// Blit pixel shader — pass-through, used in pass 1 to composite ImGui.
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
// Pipeline order (gamma-correct throughout):
//   distort → sample+convergence → linearize → bloom → diffuse → grade →
//   gaussian scanlines → shadow mask → hum → vignette → flicker →
//   re-encode → static
// ---------------------------------------------------------------------------
float4 PS_Main(VS_Output input) : SV_TARGET {
    // 1. Barrel distortion.
    float2 uv = BarrelDistort(input.uv);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return float4(0.0, 0.0, 0.0, 1.0);

    // 2. Sample with RGB convergence offset, then convert to linear light.
    float3 col = ToLinear(SampleSource(uv));

    // 3. Bloom and diffusion accumulate in linear space — bright pixels
    //    contribute quadratically more glow (physically correct).
    col += Bloom(uv);
    col += Diffusion(uv);

    // 4. Colour grade in linear space.
    col = ColorGrade(col);

    // 5. Gaussian scanlines in linear space — luminance-dependent beam width
    //    means bright areas merge scanlines naturally (crt-geom behaviour).
    col = GaussianScanline(col, input.pos.y);

    // 6. Shadow mask — sub-pixel phosphor geometry.
    col *= ShadowMask(input.pos.xy);

    // 7. Analogue interference and lens effects.
    col *= HumBar(uv.y);
    col *= Vignette(uv);
    col *= Flicker();

    // 8. Re-encode to gamma before output.
    col = ToGamma(col);

    // 9. TV static — additive noise in display (gamma) space.
    col += TVStatic(input.pos.xy);

    return float4(saturate(col), 1.0);
}
