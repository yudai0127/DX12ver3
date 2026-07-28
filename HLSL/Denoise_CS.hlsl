//=============================================================================
// Denoise_CS.hlsl
//   SVGF-style denoiser for the path tracer:
//     1. Demodulate albedo so texture detail is preserved.
//     2. Sparse three-ring bilateral blur (normal + depth guided). The rings
//        stride outward so the kernel covers a wide area with few taps, which
//        is what removes the low-frequency blotches a small dense kernel
//        leaves behind - blotches that shift every frame and read as the
//        surface crawling.
//     3. Temporal accumulation: follow the motion vector, fetch the history
//        bilinearly, and validate each of the four taps with a plane-distance
//        test. Distance *along the surface normal* ignores the in-plane and
//        along-ray jitter that made an absolute distance test flip between
//        accept and reject each frame (another source of crawl), while still
//        catching true disocclusion. Weights blend smoothly instead of
//        all-or-nothing.
//     4. Re-modulate the albedo and tonemap to the LDR output.
//=============================================================================

Texture2D<float4>   gInColor   : register(t0); // HDR accumulated sum (gAccum)
Texture2D<float4>   gInGeo     : register(t1); // normal.xyz + depth.w (hitT)
Texture2D<float4>   gInAlbedo  : register(t2); // primary-hit diffuse albedo
Texture2D<float4>   gHistColor : register(t3); // last frame: lighting.rgb + len.a
Texture2D<float4>   gHistGeo   : register(t4); // last frame: world position.xyz
Texture2D<float2>   gMotion    : register(t5); // pixel-space motion (prev - cur)
RWTexture2D<float4> gOut       : register(u0); // LDR output (back-buffer format)
RWTexture2D<float4> gHistColorOut : register(u1); // this frame: lighting + len
RWTexture2D<float4> gHistGeoOut   : register(u2); // this frame: world position

cbuffer DenoiseCB : register(b0)
{
    uint2  gDim;          // image size
    uint   gSampleCount;  // accumulated samples (acc + 1), to average the sum
    int    gRadius;       // ring stride scale (0 = denoiser disabled by C++)

    float  gExposure;     // tonemap exposure
    float  gNormalPower;  // normal edge sharpness
    float  gDepthSigma;   // relative depth tolerance
    float  gTemporalAlpha;// min blend toward the current frame (0 = full history)

    // Camera basis rather than an inverse matrix: reconstructing positions by
    // inverse-projecting the far plane is badly conditioned at this near/far
    // ratio, and the resulting wobble fed straight into the history's plane
    // test, flipping it between accept and reject each frame.
    float4 gCamRight;   // xyz = right, w = tan(fovY/2) * aspect
    float4 gCamUp;      // xyz = up,    w = tan(fovY/2)
    float4 gCamFwd;     // xyz = forward
    float4 gCameraPos;  // current camera position (world)

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
    float3 rayDir = normalize(gCamFwd.xyz
                            + gCamRight.xyz * (ndc.x * gCamRight.w)
                            + gCamUp.xyz    * (ndc.y * gCamUp.w));
    return gCameraPos.xyz + rayDir * depth;
}

// Demodulated lighting at a pixel. Colour, albedo and geometry buffers all
// hold running means over the accumulated frames, so they are used directly.
float3 lightAt(int2 q)
{
    return gInColor[q].rgb / max(gInAlbedo[q].rgb, 0.04f);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gDim.x || tid.y >= gDim.y) return;
    int2 p = int2(tid.xy);

    float4 cGeo = gInGeo[p];              // running means; renormalise the normal
    float3 cN   = normalize(cGeo.xyz);
    float  cD   = cGeo.w;

    // Background: no denoise, just tonemap the environment colour.
    if (cD <= 0.0f)
    {
        gOut[p]          = float4(tonemap(gInColor[p].rgb), 1.0f);
        gHistColorOut[p] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        gHistGeoOut[p]   = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 cAlbedo = max(gInAlbedo[p].rgb, 0.04f);
    float3 center  = lightAt(p);

    // ---- (2) sparse three-ring bilateral blur ------------------------------
    // Ring strides grow with gRadius, so radius 2 covers ~10 pixels while the
    // tap count stays at 25.
    static const int2 kRing[8] = {
        int2(-1, -1), int2(0, -1), int2(1, -1),
        int2(-1,  0),              int2(1,  0),
        int2(-1,  1), int2(0,  1), int2(1,  1)
    };
    const int   stride[3]     = { 1, 1 + gRadius, 1 + 2 * gRadius };
    const float ringWeight[3] = { 1.0f, 0.55f, 0.3f };

    float3 sum  = center;
    float  wsum = 1.0f;

    [unroll]
    for (int r = 0; r < 3; ++r)
    {
        [unroll]
        for (int k = 0; k < 8; ++k)
        {
            int2 q = p + kRing[k] * stride[r];
            if (q.x < 0 || q.y < 0 || q.x >= (int)gDim.x || q.y >= (int)gDim.y)
                continue;

            float4 qGeo = gInGeo[q];
            if (qGeo.w <= 0.0f) continue; // skip background neighbours
            float3 qN = normalize(qGeo.xyz);
            float  qD = qGeo.w;

            float wN = pow(saturate(dot(cN, qN)), gNormalPower);
            float wD = exp(-abs(cD - qD) / max(gDepthSigma * cD, 1e-3f));
            float w  = wN * wD * ringWeight[r];

            sum  += lightAt(q) * w;
            wsum += w;
        }
    }
    float3 curLight = sum / max(wsum, 1e-4f);

    // ---- (3) temporal accumulation ------------------------------------------
    float3 worldPos = worldPosOf(p, cD);

    float3 histLight = curLight;
    float  histLen   = 0.0f;

    if (gTemporalValid != 0)
    {
        // Follow the motion vector, then fetch the 2x2 history footprint with
        // bilinear weights, validating every tap against this pixel's surface
        // plane. Partial acceptance replaces the old accept/reject cliff.
        float2 prevPix = (float2(p) + 0.5f) + gMotion[p];
        float2 base = floor(prevPix - 0.5f);
        float2 f = (prevPix - 0.5f) - base;
        const float cornerW[4] = {
            (1.0f - f.x) * (1.0f - f.y),
            f.x * (1.0f - f.y),
            (1.0f - f.x) * f.y,
            f.x * f.y
        };
        const int2 cornerO[4] = { int2(0,0), int2(1,0), int2(0,1), int2(1,1) };

        // Distance measured along the normal tolerates the jitter-driven noise
        // in the reconstructed position (which lies mostly in-plane or along
        // the view ray) but still trips on genuine occlusion changes.
        const float planeTol = max(0.01f * cD, 0.05f);

        float3 hCol = 0.0f;
        float  hLen = 0.0f, hW = 0.0f;
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            int2 hp = int2(base) + cornerO[i];
            if (hp.x < 0 || hp.y < 0 || hp.x >= (int)gDim.x || hp.y >= (int)gDim.y)
                continue;

            float4 hg = gHistGeo[hp];
            if (hg.w <= 0.0f) continue;    // background last frame

            float planeDist = abs(dot(cN, hg.xyz - worldPos));
            if (planeDist >= planeTol) continue;

            float w = cornerW[i];
            float4 hc = gHistColor[hp];
            hCol += hc.rgb * w;
            hLen += hc.a * w;
            hW   += w;
        }

        if (hW > 0.05f)
        {
            histLight = hCol / hW;
            histLen   = hLen / hW;
        }
    }

    // Exponential moving average: trust the current frame while history is
    // short, settle to a small stable blend once it is long. A rejected
    // history leaves histLen at 0, which makes alpha 1 automatically.
    float  alpha    = max(gTemporalAlpha, 1.0f / (histLen + 1.0f));
    float3 outLight = lerp(histLight, curLight, alpha);
    float  newLen   = min(histLen + 1.0f, 64.0f);

    gHistColorOut[p] = float4(outLight, newLen);
    gHistGeoOut[p]   = float4(worldPos, 1.0f);

    float3 col = outLight * cAlbedo; // modulate texture detail back in
    gOut[p]    = float4(tonemap(col), 1.0f);
}
