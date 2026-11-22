// High-voltage electric lightning arcs dancing across the surface.
//   param0.x   arc speed (default 1.8)
//   param0.y   branch density (default 2.2)
//   param0.z   glow intensity (default 1.5)
//   param1.xyz lightning colour (default electric blue)
//   param2.xyz core flash colour (default white-violet)
#include "weaponfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float speed = g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 1.8;
    float scale = g_fxParam[0].y > 0.0 ? g_fxParam[0].y : 2.2;
    float glow  = g_fxParam[0].z > 0.0 ? g_fxParam[0].z : 1.5;
    float3 cArc  = dot(g_fxParam[1].xyz, 1.0) > 0.0 ? g_fxParam[1].xyz : float3(0.2, 0.55, 1.0);
    float3 cCore = dot(g_fxParam[2].xyz, 1.0) > 0.0 ? g_fxParam[2].xyz : float3(0.85, 0.9, 1.0);

    float t = Time() * speed;
    float3 p = i.pos * (0.28 * scale);
    float n1 = Fbm3(p + float3(t * 0.7, -t * 0.4, t * 0.5));
    float n2 = Fbm3(p * 1.9 + float3(-t * 1.1, t * 0.8, -t * 0.6) + n1 * 1.8);

    float bolt1 = pow(saturate(1.0 - abs(n1 - 0.5) * 9.0), 5.0);
    float bolt2 = pow(saturate(1.0 - abs(n2 - 0.5) * 12.0), 6.0);
    float flicker = 0.75 + 0.25 * sin(t * 19.0 + n1 * 12.0);

    float rim = Fresnel(i, 3.0);
    float3 env = min(Environment(reflect(ViewDir(i), Normal(i)), 0.0, 2.0), 1.0) * 0.15;
    float3 col = float3(0.01, 0.015, 0.035) + env;
    col += (bolt1 * cArc * 2.0 + bolt2 * cCore * 3.2) * glow * flicker;
    col += rim * cArc * (0.6 + bolt1 * 1.4);
    return float4(col, 1.0);
}
