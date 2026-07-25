//=============================================================================
// Denoise_CS.hlsl
//   SVGF-style denoiser for the path tracer:
//     1. Demodulate albedo so texture detail is preserved.
//     2. Edge-aware spatial bilateral blur (normal + depth guided) to clean the
//        current frame *first* -- this is what the temporal step then trusts.
//     3. Temporal accumulation: reproject through the previous frame's view-proj
//        and accept the history only when its stored WORLD POSITION matches
//        (geometry-based rejection, robust to camera motion -- unlike a colour
//        clamp, it doesn't re-inject the current frame's noise into history).
//     4. Re-modulate the albedo and tonemap to the LDR output.
//=============================================================================

Texture2D<float4>   gInColor   : register(t0); // HDR accumulated sum (gAccum)
Texture2D<float4>   gInGeo     : register(t1); // normal.xyz + depth.w (hitT)
Texture2D<float4>   gInAlbedo  : register(t2); // primary-hit base color
Texture2D<float4>   gHistColor : register(t3); // last frame: lighting.rgb + len.a
Texture2D<float4>   gHistGeo   : register(t4); // last frame: world position.xyz
RWTexture2D<float4> gOut       : register(u0); // LDR output (back-buffer format)
RWTexture2D<float4> gHistColorOut : register(u1); // this frame: lighting + len
RWTexture2D<float4> gHistGeoOut   : register(u2); // this frame: world position

cbuffer DenoiseCB : register(b0)
{
    uint2  gDim;          // image size
    uint   gSampleCount;  // accumulated samples (acc + 1), to average the sum
    int    gRadius;       // filter radius in pixels (0 = passthrough)

    float  gExposure;     // tonemap exposure
    float  gNormalPower;  // normal edge sharpness
    float  gDepthSigma;   // relative depth tolerance
    float  gTemporalAlpha;// min blend toward the current frame (0 = full history)

    row_major float4x4 gInvViewProj;  // current: reconstruct world pos from depth
    row_major float4x4 gPrevViewProj; // previous frame: reproject to its screen
    float4 gCameraPos;                 // current camera position (world)

    int    gTemporalValid; // 1 = history is usable this frame
    float  gPad0, gPad1, gPad2;
};

float3 tonemap(float3 c)
{
    c *= gExposure;
    const float a = 2.51f, b = 0.03f, cc = 2.43f, d = 0.59f, e = 0.14f;
    c = saturate((c * (a * c + b)) / (c * (cc * c + d) + e));
    return pow(c, 1.0f / 2.2f);
}

// Reconstruct a pixel's world position from its primary-ray hit distance.
float3 worldPosOf(int2 p, float depth)
{
    float2 uv  = (float2(p) + 0.5f) / float2(gDim);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 farW = mul(float4(ndc, 1.0f, 1.0f), gInvViewProj);
    farW /= farW.w;
    float3 rayDir = normalize(farW.xyz - gCameraPos.xyz);
    return gCameraPos.xyz + rayDir * depth;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gDim.x || tid.y >= gDim.y) return;
    int2 p = int2(tid.xy);

    float  invS = 1.0f / max((float)gSampleCount, 1.0f);
    float4 cGeo = gInGeo[p];
    float3 cN   = cGeo.xyz;
    float  cD   = cGeo.w;

    // Background: no denoise, just tonemap the environment colour.
    if (cD <= 0.0f)
    {
        gOut[p]          = float4(tonemap(gInColor[p].rgb * invS), 1.0f);
        gHistColorOut[p] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        gHistGeoOut[p]   = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // Albedo demodulation: filter only the *lighting* (colour / albedo) so the
    // texture detail (albedo) is preserved, then modulate back.
    float3 cAlbedo = max(gInAlbedo[p].rgb, 0.04f);
    float3 center  = (gInColor[p].rgb * invS) / cAlbedo; // demodulated lighting

    // ---- (2) spatial bilateral blur: clean the current frame ---------------
    float3 sum  = center;
    float  wsum = 1.0f;
    if (gRadius > 0)
    {
        for (int dy = -gRadius; dy <= gRadius; ++dy)
        for (int dx = -gRadius; dx <= gRadius; ++dx)
        {
            if (dx == 0 && dy == 0) continue;
            int2 q = p + int2(dx, dy);
            if (q.x < 0 || q.y < 0 || q.x >= (int)gDim.x || q.y >= (int)gDim.y) continue;

            float4 qGeo = gInGeo[q];
            if (qGeo.w <= 0.0f) continue; // skip background neighbours

            float3 qAlbedo = max(gInAlbedo[q].rgb, 0.04f);
            float3 qLight  = (gInColor[q].rgb * invS) / qAlbedo;

            float wN = pow(saturate(dot(cN, qGeo.xyz)), gNormalPower);
            float wD = exp(-abs(cD - qGeo.w) / max(gDepthSigma * cD, 1e-3f));
            float wS = exp(-(float)(dx * dx + dy * dy) /
                           (2.0f * (float)(gRadius * gRadius)));
            float w = wN * wD * wS;

            sum  += qLight * w;
            wsum += w;
        }
    }
    float3 curLight = sum / max(wsum, 1e-4f);

    // ---- (3) temporal accumulation with geometry-based rejection -----------
    float3 worldPos = worldPosOf(p, cD);

    float3 histLight = curLight;
    float  histLen   = 0.0f;
    bool   accepted  = false;

    if (gTemporalValid != 0)
    {
        float4 pc = mul(float4(worldPos, 1.0f), gPrevViewProj);
        if (pc.w > 0.0f)
        {
            float2 pndc = pc.xy / pc.w;
            float2 puv  = float2(pndc.x * 0.5f + 0.5f, 0.5f - pndc.y * 0.5f);
            if (puv.x >= 0.0f && puv.x <= 1.0f && puv.y >= 0.0f && puv.y <= 1.0f)
            {
                int2 hp = clamp(int2(puv * float2(gDim)),
                                int2(0, 0), int2(gDim) - int2(1, 1));

                float3 hPos = gHistGeo[hp].xyz;
                // Accept only if the reprojected pixel is the same surface point.
                float  thresh = max(0.02f * cD, 0.5f);
                if (length(hPos - worldPos) < thresh)
                {
                    float4 hc = gHistColor[hp];
                    histLight = hc.rgb;
                    histLen   = hc.a;
                    accepted  = true;
                }
            }
        }
    }

    // On acceptance, blend as an exponential moving average (trust current more
    // early on, settle to a small blend once history is long). On rejection
    // (disocclusion / first frame), start fresh from the current frame.
    float  alpha    = accepted ? max(gTemporalAlpha, 1.0f / (histLen + 1.0f)) : 1.0f;
    float3 outLight = lerp(histLight, curLight, alpha);
    float  newLen   = accepted ? min(histLen + 1.0f, 64.0f) : 1.0f;

    gHistColorOut[p] = float4(outLight, newLen);
    gHistGeoOut[p]   = float4(worldPos, 1.0f);

    float3 col = outLight * cAlbedo; // modulate texture detail back in
    gOut[p]    = float4(tonemap(col), 1.0f);
}
