//=============================================================================
// TAA_CS.hlsl
//   Temporal antialiasing for raytracing mode.
//
//   Raytracing mode is deterministic: one primary ray, one shadow ray, one
//   sharp mirror bounce. That makes it clean frame to frame but leaves it with
//   no antialiasing whatsoever - and a sharp mirror reflection is exactly the
//   kind of high-frequency signal a pixel grid cannot represent, so highlights
//   sparkle as the camera moves. DLSS fixes that, but its network is a fixed
//   cost set by the OUTPUT resolution, which is a bad trade against a mode
//   that only takes a few milliseconds to render in the first place.
//
//   So: jitter the primary ray, reproject the previous frame along the motion
//   vectors, and blend. Same idea DLSS uses, a fraction of a millisecond.
//
//   History is validated by COLOUR, not geometry - the neighbourhood clamp
//   below - rather than by the plane test the path-tracing denoiser uses. On a
//   silhouette the jittered primary ray hits the surface some frames and the
//   background others, so a geometric test rejects history exactly where the
//   antialiasing is needed most. Clamping to the local colour range keeps the
//   history wherever it still agrees with the current frame and drops it
//   smoothly where it does not.
//
//   Bindings deliberately mirror Denoise_CS.hlsl so both share one root
//   signature and descriptor table; only the pipeline state differs.
//=============================================================================

Texture2D<float4>   gInColor   : register(t0); // this frame's linear HDR radiance
Texture2D<float4>   gInGeo     : register(t1); // normal.xyz + hit distance.w
Texture2D<float4>   gInAlbedo  : register(t2); // bound, unused (see below)
Texture2D<float4>   gHistColor : register(t3); // last frame: resolved colour.rgb
Texture2D<float4>   gHistGeo   : register(t4); // last frame: world position.xyz
Texture2D<float2>   gMotion    : register(t5); // pixel-space motion (prev - cur)
RWTexture2D<float4> gOut          : register(u0); // LDR output, render resolution
RWTexture2D<float4> gHistColorOut : register(u1);
RWTexture2D<float4> gHistGeoOut   : register(u2);

cbuffer DenoiseCB : register(b0)
{
    uint2  gDim;
    uint   gSampleCount;
    int    gRadius;

    float  gExposure;
    float  gNormalPower;
    float  gDepthSigma;
    float  gTemporalAlpha;  // blend toward the current frame once settled

    float4 gCamRight;
    float4 gCamUp;
    float4 gCamFwd;
    float4 gCameraPos;

    int    gTemporalValid;
    float  gPad0, gPad1, gPad2;
};

float3 tonemap(float3 c)
{
    c *= gExposure;
    const float a = 2.51f, b = 0.03f, cc = 2.43f, d = 0.59f, e = 0.14f;
    c = saturate((c * (a * c + b)) / (c * (cc * c + d) + e));
    return pow(c, 1.0f / 2.2f);
}

float3 worldPosOf(int2 p, float depth)
{
    float2 uv  = (float2(p) + 0.5f) / float2(gDim);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float3 rayDir = normalize(gCamFwd.xyz
                            + gCamRight.xyz * (ndc.x * gCamRight.w)
                            + gCamUp.xyz    * (ndc.y * gCamUp.w));
    return gCameraPos.xyz + rayDir * depth;
}

// Clamping in RGB lets a hue shift survive as long as each channel is in
// range, which shows up as coloured fringing on moving edges. The luma /
// chroma split of YCoCg puts most of the signal in one axis, so the box is
// tighter where it matters and looser where the eye does not notice.
float3 rgbToYCoCg(float3 c)
{
    return float3(0.25f * c.r + 0.5f * c.g + 0.25f * c.b,
                  0.5f  * c.r              - 0.5f  * c.b,
                 -0.25f * c.r + 0.5f * c.g - 0.25f * c.b);
}

float3 yCoCgToRgb(float3 c)
{
    float t = c.x - c.z;
    return float3(t + c.y, c.x + c.z, t - c.y);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gDim.x || tid.y >= gDim.y) return;
    int2 p = int2(tid.xy);

    float3 cur = rgbToYCoCg(gInColor[p].rgb);

    // ---- neighbourhood statistics ------------------------------------------
    // Mean and standard deviation over the 3x3 window. Clipping to mu +- k*sigma
    // rather than to the min/max ignores a single outlying neighbour, which
    // matters here because one pixel of a sharp specular highlight can be
    // orders of magnitude brighter than the eight around it.
    float3 m1 = 0.0f, m2 = 0.0f;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 q = clamp(p + int2(dx, dy), int2(0, 0), int2(gDim) - 1);
            float3 c = rgbToYCoCg(gInColor[q].rgb);
            m1 += c;
            m2 += c * c;
        }
    }
    const float invN = 1.0f / 9.0f;
    float3 mu    = m1 * invN;
    float3 sigma = sqrt(max(m2 * invN - mu * mu, 0.0f));
    float3 lo    = mu - 1.25f * sigma;
    float3 hi    = mu + 1.25f * sigma;

    // ---- reproject ----------------------------------------------------------
    float4 cGeo = gInGeo[p];
    float  cD   = cGeo.w;

    float3 outCol = cur;
    float  newLen = 1.0f;

    if (gTemporalValid != 0)
    {
        float2 prevPix = (float2(p) + 0.5f) + gMotion[p];
        float2 base = floor(prevPix - 0.5f);
        float2 f    = (prevPix - 0.5f) - base;
        const float cornerW[4] = {
            (1.0f - f.x) * (1.0f - f.y),
            f.x * (1.0f - f.y),
            (1.0f - f.x) * f.y,
            f.x * f.y
        };
        const int2 cornerO[4] = { int2(0,0), int2(1,0), int2(0,1), int2(1,1) };

        float3 hCol = 0.0f;
        float  hLen = 0.0f, hW = 0.0f;
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            int2 hp = int2(base) + cornerO[i];
            if (hp.x < 0 || hp.y < 0 || hp.x >= (int)gDim.x || hp.y >= (int)gDim.y)
                continue;   // reprojected off screen: nothing to reuse

            float w = cornerW[i];
            float4 hc = gHistColor[hp];
            hCol += hc.rgb * w;
            hLen += hc.a * w;
            hW   += w;
        }

        if (hW > 0.05f)
        {
            float3 hist = clamp(hCol / hW, lo, hi);
            float  len  = hLen / hW;

            // Ramp in over the first frames so a fresh history converges fast
            // instead of showing the single-sample aliasing at full strength,
            // then settle at gTemporalAlpha.
            float alpha = max(gTemporalAlpha, 1.0f / (len + 1.0f));
            outCol = lerp(hist, cur, alpha);
            newLen = min(len + 1.0f, 64.0f);
        }
    }

    float3 rgb = max(yCoCgToRgb(outCol), 0.0f);

    gHistColorOut[p] = float4(outCol, newLen);   // keep history in YCoCg
    gHistGeoOut[p]   = (cD > 0.0f)
                     ? float4(worldPosOf(p, cD), 1.0f)
                     : float4(0.0f, 0.0f, 0.0f, 0.0f);
    gOut[p] = float4(tonemap(rgb), 1.0f);
}
