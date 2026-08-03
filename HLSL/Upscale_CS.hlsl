//=============================================================================
// Upscale_CS.hlsl
//   Bilinear upscale from the render-resolution image to the output-resolution
//   image.
//
//   This is the fallback path: when DLSS is unavailable (no RTX GPU, SDK
//   missing, or the user turned it off) the ray tracer still renders at a
//   reduced resolution for speed, and this pass stretches the result to the
//   back buffer. When DLSS is active it replaces this pass entirely, doing a
//   far better job of reconstructing the missing pixels.
//=============================================================================

Texture2D<float4>   gSrc : register(t0); // render-resolution LDR image
RWTexture2D<float4> gDst : register(u0); // output-resolution LDR image
SamplerState        gSmp : register(s0); // linear clamp

cbuffer UpscaleCB : register(b0)
{
    uint2 gDstDim;   // output resolution
    uint2 gSrcDim;   // rendered sub-rectangle (top-left of the source texture)
    uint2 gTexDim;   // allocated size of the source texture
    uint  gTonemap;  // 1 = source is linear HDR and must be tonemapped
    float gExposure;
    float gSharpness;
    float3 gPadding;
};

// Same ACES curve the ray tracer uses, so both paths look alike.
float3 tonemap(float3 c)
{
    c *= gExposure;
    const float a = 2.51f, b = 0.03f, cc = 2.43f, d = 0.59f, e = 0.14f;
    c = saturate((c * (a * c + b)) / (c * (cc * c + d) + e));
    return pow(c, 1.0f / 2.2f);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gDstDim.x || tid.y >= gDstDim.y) return;

    // The source texture is allocated at full size but only its top-left
    // gSrcDim region was rendered, so map the destination pixel into that
    // region first and only then into texture UV space.
    float2 p      = (float2(tid.xy) + 0.5f) / float2(gDstDim); // [0,1) in dest
    float2 srcPix = p * float2(gSrcDim);                       // rendered pixels
    float2 uv     = srcPix / float2(gTexDim);                  // texture UV

    float4 c = gSrc.SampleLevel(gSmp, uv, 0);

    // DLSS hands back linear HDR, so that path tonemaps here instead of in the
    // ray tracer; the fallback path is already tonemapped LDR.
    if (gTonemap != 0) c.rgb = tonemap(c.rgb);

    // DLSS sharpening in older SDKs is deprecated, so restore a small amount
    // of local contrast here. Work in display space and use only the four
    // immediate neighbours to avoid the large halos of a wide unsharp mask.
    if (gSharpness > 0.0f)
    {
        float2 texel = 1.0f / float2(gTexDim);
        float3 n = gSrc.SampleLevel(gSmp, uv + float2(0.0f, -texel.y), 0).rgb;
        float3 s = gSrc.SampleLevel(gSmp, uv + float2(0.0f,  texel.y), 0).rgb;
        float3 e = gSrc.SampleLevel(gSmp, uv + float2( texel.x, 0.0f), 0).rgb;
        float3 w = gSrc.SampleLevel(gSmp, uv + float2(-texel.x, 0.0f), 0).rgb;
        if (gTonemap != 0)
        {
            n = tonemap(n); s = tonemap(s);
            e = tonemap(e); w = tonemap(w);
        }
        float3 crossBlur = (n + s + e + w) * 0.25f;
        c.rgb = saturate(c.rgb + (c.rgb - crossBlur) * gSharpness);
    }

    gDst[tid.xy] = c;
}
