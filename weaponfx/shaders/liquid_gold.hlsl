// Liquid mirror gold & chrome metal with rippling surface waves.
//   param0.x   ripple speed (default 1.2)
//   param0.y   ripple strength (default 0.35)
//   param0.z   roughness / blur (default 0.4)
//   param1.xyz primary metal tint (default gold)
//   param2.xyz secondary iridescence tint (default rose copper)
#include "weaponfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float speed  = g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 1.2;
    float ripple = g_fxParam[0].y > 0.0 ? g_fxParam[0].y : 0.35;
    float rough  = g_fxParam[0].z > 0.0 ? g_fxParam[0].z : 0.4;
    float3 cGold = dot(g_fxParam[1].xyz, 1.0) > 0.0 ? g_fxParam[1].xyz : float3(1.0, 0.78, 0.28);
    float3 cRose = dot(g_fxParam[2].xyz, 1.0) > 0.0 ? g_fxParam[2].xyz : float3(1.0, 0.42, 0.32);

    float t = Time() * speed;
    float3 p = i.pos * 0.45;
    float3 wave = float3(
        Noise3(p + float3(t, 0.0, 0.0)) - 0.5,
        Noise3(p + float3(0.0, t, 4.7)) - 0.5,
        Noise3(p + float3(3.1, 0.0, t)) - 0.5
    );
    float3 n = normalize(Normal(i) + wave * ripple);
    float3 v = ViewDir(i);
    float3 r = reflect(v, n);

    float3 env = min(Environment(r, 0.0, rough * 4.0), 2.2);
    float ndv = saturate(dot(n, -v));
    float3 metalTint = lerp(cRose, cGold, ndv);
    float spec = pow(saturate(dot(r, normalize(float3(0.4, 0.3, 0.85)))), 24.0);

    float3 col = env * metalTint * 1.35 + spec * cGold * 0.9 + pow(1.0 - ndv, 4.0) * cRose * 0.45;
    return float4(col, 1.0);
}
