// A window into space: starfield and nebula seen through the weapon.
//   param0.x   nebula strength (0 = default 1)
//   param0.y   rim glow strength (0 = default 1)
//   param1.xyz nebula colour 1 (default purple)
//   param2.xyz nebula colour 2 (default blue)
//   param3.xyz rim colour (default blue)
#include "weaponfx.hlsli"
#include "effects.hlsli"

// An unset colour (0 0 0: the menu's "own colour" switch is off) is the default.
float3 OrDefault(float3 c, float3 d) { return dot(c, c) > 0.0 ? c : d; }

float4 main(PSIn i) : SV_Target {
    float nebulaK = g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 1.0;
    float rimK    = g_fxParam[0].y > 0.0 ? g_fxParam[0].y : 1.0;
    float3 nebA = OrDefault(g_fxParam[1].xyz, float3(0.55, 0.12, 0.70));
    float3 nebB = OrDefault(g_fxParam[2].xyz, float3(0.05, 0.30, 0.65));
    float3 rimC = OrDefault(g_fxParam[3].xyz, float3(0.35, 0.55, 1.0));
    return float4(Galaxy(i, nebulaK, rimK, nebA, nebB, rimC), 1.0);
}
