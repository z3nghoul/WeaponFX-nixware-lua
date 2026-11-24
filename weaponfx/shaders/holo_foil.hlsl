// Holographic foil: thin-film rainbow that shifts with the viewing angle, over a
// dark mirror base, with glittering flakes. The foil's pattern is static on the
// weapon's UV; the colours roll as the weapon is turned or inspected, as on a
// real holo sticker.
//   param0.x   rainbow density (default 3.0)
//   param0.y   foil strength 0..1 (default 0.85)
//   param0.z   glitter 0..1 (default 0.6)
//   param0.w   hue shift 0..1 (default 0.0)
//   param1.xyz base tint (default 0.07 0.07 0.09)
#include "weaponfx.hlsli"

float3 FoilPalette(float h) {
    return lerp(0.5 + 0.5 * cos(6.28318 * (h + float3(0.00, 0.33, 0.67))), 0.75, 0.22);
}

float4 main(PSIn i) : SV_Target {
    float dens  = g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 3.0;
    float foilK = saturate(g_fxParam[0].y > 0.0 ? g_fxParam[0].y : 0.85);
    float glitter = saturate(g_fxParam[0].z > 0.0 ? g_fxParam[0].z : 0.6);
    float hueShift = g_fxParam[0].w;
    float3 cBase = dot(g_fxParam[1].xyz, 1.0) > 0.0 ? g_fxParam[1].xyz : float3(0.07, 0.07, 0.09);

    float3 n = Normal(i);
    float3 v = ViewDir(i);
    float3 r = reflect(v, n);
    float ndv = saturate(dot(n, -v));

    // Film thickness varies slowly over the surface (UV, static); the viewing
    // angle then picks the colour, so the hue rolls as the view changes.
    float thick = Fbm3(float3(i.uv.xy * 2.2, 3.7));
    float h = (1.0 - ndv) * dens * 0.55 + thick * 0.9 + dot(i.uv.xy, float2(0.25, 0.4)) + hueShift;
    float3 film = FoilPalette(h);

    // A mirror base with the film laid over it.
    float3 env = min(Environment(r, 0.0, 1.5), 2.2);
    float3 col = cBase + env * 0.25;
    col = lerp(col, col * 0.35 + film * (0.10 + 0.20 * env), foilK);

    // Flakes: each cell has its own little normal, so it flashes at its own angle.
    float2 gp = i.uv.xy * 150.0;
    float2 cell = floor(gp);
    float3 rnd = Hash33(float3(cell, 1.7)) * 2.0 - 1.0;
    float3 fn = normalize(n + rnd * 0.55);
    float3 fr = reflect(v, fn);
    float3 L = normalize(float3(0.45, 0.55, 0.70));
    float flake = pow(saturate(dot(fr, L)), 60.0);
    float inCell = smoothstep(0.5, 0.15, length(frac(gp) - 0.5));
    col += FoilPalette(h + Hash13(float3(cell, 4.0))) * flake * inCell * glitter * 2.2;

    col += pow(1.0 - ndv, 4.0) * film * 0.22 * foilK;
    return float4(col, 1.0);
}
