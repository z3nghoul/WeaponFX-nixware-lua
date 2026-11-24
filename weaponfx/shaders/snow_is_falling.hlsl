// Snow is falling, behind frosted glass: a layered snowfall (up to 66 layers of
// parallax flakes with depth of field) over a window of fog and condensation. Everything is laid on the weapon's own UV, so the picture stays
// on the gun when it turns or is inspected (no screen-space "green screen").
//   Snow: "Just snow" https://www.shadertoy.com/view/ldsGDn, Copyright (c) 2013 Andrew
//   Baldwin (License: Attribution-NonCommercial-ShareAlike).
//   Glass: the fogged window of Its Raining (no drops), after "Heartfelt" by Martijn
//   Steinrucken (BigWings) 2017.
//   The ORIGINAL AUTHORS' LICENSES APPLY to this file: Shadertoy code is CC BY-NC-SA
//   3.0 unless its author says otherwise - keep this credit, no commercial use, and
//   share derivatives alike.
//   param0.x   time speed (default 1.0)
//   param0.y   snow layers, 1..66 (default 16.0)
//   param0.w   ice glints 0..1: tiny sparkles that flash as the weapon turns, e.g. on inspect (default 0.5)
//   param0.z   snow scale on the weapon UV: flake size is smaller the bigger this is (default 2.5)
//   param1.x   fog blur radius per focus unit, px (default 2.4)
//   param1.y   condensation / frost opacity 0..1 (default 0.55)
//   param2.x   snow brightness (default 1.0)
//   param2.y   blizzard 0..1: 0 the light snow, 1 the original's blizzard settings (default 0.0)
//   param2.z   environment reflection strength (default 0.5; <0 = off)
//   param2.w   snowdrifts 0..1: snow piles up along ledges and creases, where the surface bends (default 0.6)
//   param3.w   snow cap 0..1: snow settles on the upward-facing parts of the glass (default 0.5)
//   param3.xyz glass tint colour: the fog, reflections and highlights take this colour (default 0.84 0.91 0.98)
#include "weaponfx.hlsli"

#define S(a, b, t) smoothstep(a, b, t)

//=== snow: SnowIsFalling ===================================================

// "Just snow" https://www.shadertoy.com/view/ldsGDn
// Copyright (c) 2013 Andrew Baldwin (twitter: baldand, www: http://thndl.com)
// License = Attribution-NonCommercial-ShareAlike (http://creativecommons.org/licenses/by-nc-sa/3.0/deed.en_US)
// "Just snow" - Is it Christmas yet? Ported to HLSL; the mouse and resolution
// inputs are replaced by the weapon's UV and a fixed light / blizzard choice.
// LIGHT_SNOW: depth .5, width .3, speed .6.  Blizzard: depth .1, width .8, speed 1.5.
float snowing(float2 uv, float iTime, int layers, float blizzard) {
    // GLSL's mat3(...) fills columns, and p * m is that matrix times the vector.
    const float3 pc0 = float3(13.323122, 23.5112, 21.71123);
    const float3 pc1 = float3(21.1212, 28.7312, 11.9312);
    const float3 pc2 = float3(21.8112, 14.7212, 61.3934);
    float depth = lerp(0.5, 0.1, blizzard);
    float width = lerp(0.3, 0.8, blizzard);
    float speed = lerp(0.6, 1.5, blizzard);
    float acc = 0.0;
    float dof = 5.0 * sin(iTime * 0.1);
    [loop] for (int i = 0; i < layers; i++) {
        float fi = float(i);
        float2 q = uv * (1.0 + fi * depth);
        float w = width * frac(fi * 7.238917) - width * 0.1 * sin(iTime * 2.0 + fi);
        q += float2(q.y * w, speed * iTime / (1.0 + fi * depth * 0.03));
        float3 n = float3(floor(q), 31.189 + fi);
        float3 m = floor(n) * 0.00001 + frac(n);
        float3 pm = m.x * pc0 + m.y * pc1 + m.z * pc2;
        float3 mp = (31415.9 + m) / frac(pm);
        float3 r = frac(mp);
        float2 s = abs((q - floor(q)) - 0.5 + 0.9 * r.xy - 0.45);
        s += 0.01 * abs(2.0 * frac(10.0 * q.yx) - 1.0);
        float d = 0.6 * max(s.x - s.y, s.x + s.y) + max(s.x, s.y) - 0.01;
        float edge = 0.005 + 0.05 * min(0.5 * abs(fi - 5.0 - dof), 1.0);
        acc += smoothstep(edge, -edge, d) * (r.x / (1.0 + 0.02 * fi * depth));
    }
    return acc;
}

//=== glass: the fogged window of Its Raining (after Heartfelt) =============

// 8-tap disk blur over the live scene behind the weapon (or tex0 if bound).
float3 SampleFoggyBg(PSIn i, float2 offsetPx, float blurPx) {
    static const float2 kDisk[8] = {
        float2( 1.000,  0.000), float2( 0.707,  0.707), float2( 0.000,  1.000), float2(-0.707,  0.707),
        float2(-1.000,  0.000), float2(-0.707, -0.707), float2( 0.000, -1.000), float2( 0.707, -0.707)
    };
    float3 res = 0.0;
    if (HasTex0()) {
        float2 uv = ScreenUV(i) + offsetPx * g_fxScreen.zw;
        float3 acc = g_fxTex0.SampleLevel(g_linear, uv, 0.0).rgb;
        if (blurPx > 0.25) {
            float2 stepUV = blurPx * g_fxScreen.zw;
            [unroll] for (int k = 0; k < 8; ++k) acc += g_fxTex0.SampleLevel(g_linear, uv + kDisk[k] * stepUV, 0.0).rgb;
            acc /= 9.0;
        }
        res = acc;
    } else {
        float3 center = SceneAtSafe(i, offsetPx);
        if (blurPx <= 0.25) {
            res = center;
        } else {
            float3 acc = center;
            [unroll] for (int k = 0; k < 8; ++k) {
                float3 s = SceneAt(i, offsetPx + kDisk[k] * blurPx);
                acc += (dot(s, s) >= 1e-6) ? s : center;
            }
            res = acc / 9.0;
        }
    }
    return res;
}

//===========================================================================
float4 main(PSIn i) : SV_Target {
    float tSpeed   = g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 1.0;
    int layers     = (int)clamp(g_fxParam[0].y > 0.0 ? g_fxParam[0].y : 16.0, 1.0, 66.0);
    float snowSc   = g_fxParam[0].z > 0.0 ? g_fxParam[0].z : 2.5;
    float glintK   = g_fxParam[0].w > 0.0 ? saturate(g_fxParam[0].w) : 0.5;
    float blurMul  = g_fxParam[1].x > 0.0 ? g_fxParam[1].x : 2.4;
    float frostK   = g_fxParam[1].y > 0.0 ? saturate(g_fxParam[1].y) : 0.55;
    float snowGain = g_fxParam[2].x > 0.0 ? g_fxParam[2].x : 1.0;
    float blizzard   = saturate(g_fxParam[2].y);
    float3 tint    = dot(g_fxParam[3].xyz, 1.0) > 0.0 ? saturate(g_fxParam[3].xyz) : float3(0.84, 0.91, 0.98);
    float driftK   = g_fxParam[2].w > 0.0 ? saturate(g_fxParam[2].w) : 0.6;
    float capK     = g_fxParam[3].w > 0.0 ? saturate(g_fxParam[3].w) : 0.5;
    float reflK    = g_fxParam[2].z == 0.0 ? 0.5 : max(g_fxParam[2].z, 0.0);

    float T = Time() * tSpeed;

    float2 surfUV = float2(i.uv.x, 1.0 - i.uv.y);        // sticks to the weapon mesh
    float3 Nrm = Normal(i);
    float3 V   = ViewDir(i);                              // eye -> pixel
    float3 Veye = -V;
    float focus = 5.0;                                    // the whole pane is fogged and blurred
    float2 offsetPx = float2(0.0, 0.0);

    float3 bg = SampleFoggyBg(i, offsetPx, focus * blurMul);

    // Frosted condensation tint on the glass.
    float3 frostTint = bg * tint + tint * 0.06;
    // Frost is thicker toward the edges of the glass, so the pane reads as a pane.
    float rim = pow(1.0 - saturate(dot(Nrm, Veye)), 2.0);
    float3 col = lerp(bg, frostTint, saturate(frostK + rim * 0.3));

    // Reflection and highlights on the glass.
    if (reflK > 0.0) {
        float3 reflDir = reflect(V, Nrm);
        float3 env = min(Environment(reflDir, 0.0, 4.5), 3.0);
        float fres = pow(1.0 - saturate(dot(Nrm, Veye)), 4.0);
        col = lerp(col, env * tint, saturate(fres * 0.70) * reflK);
    }
    float3 L1 = normalize(CameraUp() * 0.75 + CameraRight() * 0.45 - V * 0.45);
    float3 H1 = normalize(L1 + Veye);
    col += pow(saturate(dot(Nrm, H1)), 18.0) * 0.08 * lerp(float3(1.0, 1.0, 1.0), tint, 0.8);
    col += Fresnel(i, 4.5) * 0.20 * tint;

    // Snow settled on the glass: a grainy white cap on the faces that look up.
    float up = dot(Nrm, CameraUp());
    float patch = Fbm3(float3(surfUV * 14.0, 4.3));
    float grain = 0.6 + 0.4 * Fbm3(float3(surfUV * 70.0, 1.7));
    float cap = smoothstep(0.55, 0.95, up) * smoothstep(0.45, 0.70, patch) * grain * capK;
    col = lerp(col, float3(0.86, 0.92, 1.0) * lerp(float3(1.0, 1.0, 1.0), tint, 0.5), saturate(cap) * 0.8);

    // Snowdrifts: snow piles up toward the edges of the shape that face up. Only smooth
    // quantities are used (the normal itself, never its derivatives, which are constant
    // per triangle and show the mesh), so the mounds have no visible facets.
    float edgeK = 1.0 - saturate(dot(Nrm, Veye));
    float mound = smoothstep(0.25, 0.85, edgeK) * smoothstep(0.0, 0.6, up);
    mound *= 0.45 + 0.55 * Fbm3(float3(surfUV * 24.0, 9.1));
    float drift = saturate(mound * driftK * 1.6);
    float3 driftCol = float3(0.93, 0.97, 1.0) * (0.75 + 0.25 * up) * lerp(float3(1.0, 1.0, 1.0), tint, 0.35);
    col = lerp(col, driftCol, drift * 0.9);

    // Ice glints: a fine grid of tiny facets on the glass. Each has its own angle, and
    // flashes when the surface normal turns past it, so the sparkle moves across the
    // glass as the weapon is turned or inspected. A faint time flicker keeps it alive.
    float2 gp = surfUV * 160.0;
    float2 gc = floor(gp);
    float gh = frac(sin(dot(gc, float2(12.9898, 78.233))) * 43758.5453);
    float gh2 = frac(gh * 17.31);
    float2 gf = frac(gp) - 0.5 - (float2(gh2, frac(gh * 91.7)) - 0.5) * 0.5;   // from the facet's own centre
    float gd = length(gf);
    float spark = smoothstep(0.17, 0.0, gd)                                      // a round point
                + 0.6 * smoothstep(0.035, 0.0, min(abs(gf.x), abs(gf.y))) * smoothstep(0.45, 0.0, gd);  // with a faint cross
    float turn = dot(Nrm, CameraRight()) * 9.0 + dot(Nrm, CameraUp()) * 7.0 + dot(Nrm, Veye) * 5.0;
    float flash = pow(saturate(sin(gh * 61.0 + turn)), 40.0);
    float glint = spark * flash * step(0.55, gh2) * (0.7 + 0.3 * sin(T * 4.0 + gh * 90.0));
    col += glint * glintK * 1.4 * lerp(float3(1.0, 1.0, 1.0), tint, 0.5);

    // The snow, on the weapon's UV.
    float snowOut = snowing(surfUV * snowSc, T, layers, blizzard);
    col += float3(snowOut, snowOut, snowOut) * snowGain;
    return float4(col, 1.0);
}
