// Prismatic diamond crystal: multi-faceted rainbow refraction and sparkle.
//   param0.x   refraction in pixels (default 55)
//   param0.y   rainbow dispersion (default 0.35)
//   param0.z   facet scale (default 3.0)
//   param1.xyz crystal tint (default ice diamond)
#include "weaponfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float px    = g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 55.0;
    float disp  = g_fxParam[0].y > 0.0 ? g_fxParam[0].y : 0.35;
    float scale = g_fxParam[0].z > 0.0 ? g_fxParam[0].z : 3.0;
    float3 tint = dot(g_fxParam[1].xyz, 1.0) > 0.0 ? g_fxParam[1].xyz : float3(0.88, 0.95, 1.0);

    float3 cell = floor(i.pos * scale);
    float3 facetN = normalize(Normal(i) + (Hash33(cell) - 0.5) * 0.45);
    float2 off = float2(dot(facetN, CameraRight()), -dot(facetN, CameraUp())) * px;

    float3 seen;
    seen.r = SceneAtSafe(i, off * (1.0 + disp)).r;
    seen.g = SceneAtSafe(i, off).g;
    seen.b = SceneAtSafe(i, off * (1.0 - disp)).b;

    float fr = Fresnel(i, 3.0);
    float phase = dot(facetN, ViewDir(i)) * 6.28318 + Time() * 1.5;
    float3 rainbow = 0.5 + 0.5 * cos(phase + float3(0.0, 2.094, 4.188));
    float3 refl = min(Environment(reflect(ViewDir(i), facetN), 0.0, 0.0), 1.6);

    float sparkle = pow(saturate(dot(reflect(ViewDir(i), facetN), normalize(float3(0.3, 0.5, 0.8)))), 32.0);
    float3 col = seen * tint + lerp(refl, rainbow, 0.55) * fr * 0.85 + sparkle * rainbow * 1.4;
    return float4(col, 1.0);
}
