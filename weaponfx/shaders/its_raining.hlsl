// It's raining: rain drops and trails on frosted glass, anchored to the weapon's
// 3D surface (moves and catches 3D reflections/refraction when inspecting).
// Based on Heartfelt by Martijn Steinrucken (BigWings) 2017.
//   param0.x   rain speed (default 1.0)
//   param0.y   rain amount 0..1 (default 0.80; <0 = animated sine cycle)
//   param0.z   drop scale on weapon UV (default 3.0)
//   param0.w   fog blur radius per focus unit, px (default 2.4)
//   param1.x   3D refraction strength in pixels (default 75.0)
//   param1.y   3D drop bump / dome height (default 24.0)
//   param1.z   environment reflection strength (default 0.65; <0 = off)
//   param1.w   frost / condensation opacity 0..1 (default 0.55)
//   param2.x   lightning flash strength (default 1.0; <0 = off)
//   param3.xyz glass tint colour: the fog, reflections and highlights take this colour (default 0.84 0.91 0.98)
#include "weaponfx.hlsli"

#define S(a, b, t) smoothstep(a, b, t)

float3 N13(float p) {
    float3 p3 = frac(float3(p, p, p) * float3(0.1031, 0.11369, 0.13787));
    p3 += dot(p3, p3.yzx + 19.19);
    return frac(float3(
        (p3.x + p3.y) * p3.z,
        (p3.x + p3.z) * p3.y,
        (p3.y + p3.z) * p3.x
    ));
}

float N(float t) {
    return frac(sin(t * 12345.564) * 7658.76);
}

float Saw(float b, float t) {
    return S(0.0, b, t) * S(1.0, b, t);
}

float2 DropLayer2(float2 uv, float t) {
    float2 UV = uv;

    uv.y += t * 0.75;
    float2 a = float2(6.0, 1.0);
    float2 grid = a * 2.0;
    float2 id = floor(uv * grid);

    float colShift = N(id.x);
    uv.y += colShift;

    id = floor(uv * grid);
    float3 n = N13(id.x * 35.2 + id.y * 2376.1);
    float2 st = frac(uv * grid) - float2(0.5, 0.0);

    float x = n.x - 0.5;

    float y = UV.y * 20.0;
    float wiggle = sin(y + sin(y));
    x += wiggle * (0.5 - abs(x)) * (n.z - 0.5);
    x *= 0.7;
    float ti = frac(t + n.z);
    y = (Saw(0.85, ti) - 0.5) * 0.9 + 0.5;
    float2 p = float2(x, y);

    float d = length((st - p) * a.yx);
    float mainDrop = S(0.4, 0.0, d);

    float r = sqrt(S(1.0, y, st.y));
    float cd = abs(st.x - x);
    float trail = S(0.23 * r, 0.15 * r * r, cd);
    float trailFront = S(-0.02, 0.02, st.y - y);
    trail *= trailFront * r * r;

    y = UV.y;
    y = frac(y * 10.0) + (st.y - 0.5);
    float dd = length(st - float2(x, y));
    float droplets = S(0.3, 0.0, dd);
    float m = mainDrop + droplets * r * trailFront;

    return float2(m, trail);
}

float StaticDrops(float2 uv, float t) {
    uv *= 40.0;

    float2 id = floor(uv);
    uv = frac(uv) - 0.5;
    float3 n = N13(id.x * 107.45 + id.y * 3543.654);
    float2 p = (n.xy - 0.5) * 0.7;
    float d = length(uv - p);

    float fade = Saw(0.025, frac(t + n.z));
    return S(0.3, 0.0, d) * frac(n.z * 10.0) * fade;
}

float2 Drops(float2 uv, float t, float l0, float l1, float l2) {
    float s = StaticDrops(uv, t) * l0;
    float2 m1 = DropLayer2(uv, t) * l1;
    float2 m2 = DropLayer2(uv * 1.85, t) * l2;

    float c = s + m1.x + m2.x;
    c = S(0.3, 1.0, c);

    return float2(c, max(m1.y * l0, m2.y * l1));
}

// Screen-space cotangent frame: maps 2D UV drop gradients into 3D world space
// so every droplet is a real 3D dome that rotates with the weapon on inspect.
void SurfaceBasis(PSIn i, float2 uv, float3 nrm, out float3 T, out float3 B) {
    float3 dp1 = ddx(i.pos);
    float3 dp2 = ddy(i.pos);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);

    float3 dp2perp = cross(dp2, nrm);
    float3 dp1perp = cross(nrm, dp1);
    float3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 b = dp2perp * duv1.y + dp1perp * duv2.y;

    float invmax = rsqrt(max(dot(t, t), dot(b, b)) + 1e-8);
    T = t * invmax;
    B = b * invmax;
}

// 8-tap disk blur over the live scene behind the weapon (or tex0 if bound).
float3 SampleFoggyBg(PSIn i, float2 offsetPx, float blurPx) {
    static const float2 kDisk[8] = {
        float2( 1.000,  0.000),
        float2( 0.707,  0.707),
        float2( 0.000,  1.000),
        float2(-0.707,  0.707),
        float2(-1.000,  0.000),
        float2(-0.707, -0.707),
        float2( 0.000, -1.000),
        float2( 0.707, -0.707)
    };

    float3 res = 0.0;
    if (HasTex0()) {
        float2 uv = ScreenUV(i) + offsetPx * g_fxScreen.zw;
        float3 acc = g_fxTex0.SampleLevel(g_linear, uv, 0.0).rgb;
        if (blurPx > 0.25) {
            float2 stepUV = blurPx * g_fxScreen.zw;
            [unroll] for (int k = 0; k < 8; ++k) {
                acc += g_fxTex0.SampleLevel(g_linear, uv + kDisk[k] * stepUV, 0.0).rgb;
            }
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

float4 main(PSIn i) : SV_Target {
    float speed     = g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 1.0;
    float rainCfg   = g_fxParam[0].y;
    float scale     = g_fxParam[0].z > 0.0 ? g_fxParam[0].z : 3.0;
    float blurMul   = g_fxParam[0].w > 0.0 ? g_fxParam[0].w : 2.4;
    float refractPx = g_fxParam[1].x > 0.0 ? g_fxParam[1].x : 75.0;
    float bump      = g_fxParam[1].y > 0.0 ? g_fxParam[1].y : 24.0;
    float reflK     = g_fxParam[1].z == 0.0 ? 0.65 : max(g_fxParam[1].z, 0.0);
    float frostK    = g_fxParam[1].w > 0.0 ? saturate(g_fxParam[1].w) : 0.55;
    float3 tint     = dot(g_fxParam[3].xyz, 1.0) > 0.0 ? saturate(g_fxParam[3].xyz) : float3(0.84, 0.91, 0.98);
    float lightK    = g_fxParam[2].x == 0.0 ? 1.0 : max(g_fxParam[2].x, 0.0);

    float T = Time() * speed;
    float t = T * 0.2;
    float rainAmount = rainCfg > 0.0 ? saturate(rainCfg) : (rainCfg < 0.0 ? sin(T * 0.05) * 0.3 + 0.7 : 0.80);

    float staticDrops = S(-0.5, 1.0, rainAmount) * 2.0;
    float layer1 = S(0.25, 0.75, rainAmount);
    float layer2 = S(0.0, 0.5, rainAmount);

    // Surface-anchored UV (sticks to the weapon mesh and moves on inspect)
    float2 surfUV = float2(i.uv.x, 1.0 - i.uv.y) * scale;
    float3 Nrm = Normal(i);
    float3 V   = ViewDir(i); // eye -> pixel

    float3 Tvec, Bvec;
    SurfaceBasis(i, surfUV, Nrm, Tvec, Bvec);

    // Tangent-space view parallax so droplets have real 3D height when tilting the gun
    float3 Veye = -V;
    float ndv = max(dot(Nrm, Veye), 0.25);
    float2 viewTS = float2(dot(Veye, Tvec), dot(Veye, Bvec)) / ndv;

    float2 c0 = Drops(surfUV, t, staticDrops, layer1, layer2);
    float2 uvP = surfUV - viewTS * c0.x * 0.008;

    float2 c = Drops(uvP, t, staticDrops, layer1, layer2);
    float2 e = float2(0.0018, 0.0);
    float cx = Drops(uvP + e, t, staticDrops, layer1, layer2).x;
    float cy = Drops(uvP + e.yx, t, staticDrops, layer1, layer2).x;
    float2 grad = float2(cx - c.x, cy - c.x);

    // Outward 3D dome normal for each droplet in world space
    float3 dropN = normalize(Nrm - (Tvec * grad.x + Bvec * grad.y) * bump);

    // Clear wet spots (drops + trails) vs foggy frosted glass
    float dropMask = S(0.08, 0.28, c.x);
    float wetness  = saturate(dropMask + c.y * 0.75);
    float maxBlur  = lerp(2.5, 5.5, rainAmount);
    float focus    = lerp(maxBlur, 0.0, wetness);

    // Physical 3D refraction through curved glass + water domes (no flat greenscreen)
    float3 refrDir = refract(V, dropN, 1.0 / 1.333);
    float3 bend    = refrDir - V;
    float2 offsetPx = float2(dot(bend, CameraRight()), -dot(bend, CameraUp())) * refractPx;

    float3 bg = SampleFoggyBg(i, offsetPx, focus * blurMul);

    // Subtle frosted-glass condensation tint on dry/foggy areas, crystal clear inside drops
    float3 frostTint = bg * tint + tint * 0.06;
    float3 col = lerp(frostTint, bg, lerp(1.0 - frostK, 1.0, wetness));

    // 3D HDR environment cubemap reflection: glossy on drops, soft/rough on fog
    if (reflK > 0.0) {
        float3 reflDir = reflect(V, dropN);
        float reflMip  = lerp(4.5, 0.4, wetness);
        float3 env     = min(Environment(reflDir, 0.0, reflMip), 3.0);
        float fres     = pow(1.0 - saturate(dot(dropN, Veye)), 4.0);
        float reflAmt  = saturate(fres * 0.70 + dropMask * 0.16) * reflK;
        col = lerp(col, env * tint, reflAmt);
    }

    // 3D specular highlights on individual water domes (glints shift as you inspect)
    float3 L1 = normalize(CameraUp() * 0.75 + CameraRight() * 0.45 - V * 0.45);
    float3 H1 = normalize(L1 + Veye);
    float spec = pow(saturate(dot(dropN, H1)), lerp(18.0, 96.0, wetness));
    col += spec * (0.08 + 0.95 * dropMask) * lerp(float3(1.0, 1.0, 1.0), tint, 0.8);

    // Subtle internal caustic bright crescent on the lower edge of each droplet
    float caustic = saturate(-grad.y * 12.0) * dropMask;
    col += caustic * 0.22 * tint;

    // Crisp weapon silhouette rim
    col += Fresnel(i, 4.5) * 0.20 * tint;

    // Lightning flicker + flash on the glass and the 3D drops
    if (lightK > 0.0) {
        float lt = (T + 3.0) * 0.5;
        float lightning = sin(lt * sin(lt * 10.0));
        lightning *= pow(max(0.0, sin(lt + sin(lt))), 10.0);
        float flash = max(0.0, lightning) * lightK;
        col *= 1.0 + flash * 1.1;
        col += flash * (0.18 + 0.55 * dropMask + 0.35 * spec) * float3(0.78, 0.88, 1.0);
    }

    return float4(col, 1.0);
}
