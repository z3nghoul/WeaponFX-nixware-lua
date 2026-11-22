// Cyberpunk 3D neon wireframe grid with a travelling laser scan wave.
//   param0.x   grid density (default 2.5)
//   param0.y   scan speed (default 1.5)
//   param0.z   line thickness (default 0.08)
//   param1.xyz neon grid colour (default cyan)
//   param2.xyz scan pulse colour (default magenta)
#include "weaponfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float scale = g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 2.5;
    float speed = g_fxParam[0].y > 0.0 ? g_fxParam[0].y : 1.5;
    float thick = g_fxParam[0].z > 0.0 ? g_fxParam[0].z : 0.08;
    float3 cGrid = dot(g_fxParam[1].xyz, 1.0) > 0.0 ? g_fxParam[1].xyz : float3(0.0, 0.95, 1.0);
    float3 cScan = dot(g_fxParam[2].xyz, 1.0) > 0.0 ? g_fxParam[2].xyz : float3(1.0, 0.1, 0.75);

    float3 p = i.pos * scale;
    float3 g = abs(frac(p - 0.5) - 0.5) / max(fwidth(p), 1e-4);
    float d = min(min(g.x, g.y), g.z);
    float lineMask = 1.0 - smoothstep(0.0, 1.2 + thick * 10.0, d);

    float t = Time() * speed;
    float wave = sin(dot(i.pos, float3(0.45, 0.35, 0.25)) - t * 2.5);
    float pulse = pow(saturate(wave * 0.5 + 0.5), 8.0);

    float rim = Fresnel(i, 2.5);
    float3 base = float3(0.015, 0.02, 0.04) + rim * lerp(cGrid, cScan, 0.5) * 0.7;
    float3 gridCol = lerp(cGrid, cScan * 2.2, pulse);
    float3 col = base + lineMask * gridCol * (1.1 + pulse * 1.5) + pulse * cScan * 0.25;
    return float4(col, 1.0);
}
