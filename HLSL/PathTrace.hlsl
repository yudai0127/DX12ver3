//=============================================================================
// PathTrace.hlsl  (DXR milestone 2: Monte-Carlo path tracer)
//   Iterative megakernel path tracer with:
//     - global illumination (diffuse + glossy indirect bounces)
//     - soft shadows (sampled sun disk)
//     - anti-aliasing (sub-pixel jitter)
//     - temporal accumulation (converges while the view is static)
//     - Reinhard tonemapping
//
//   The closest-hit shader only *fills surface data* into the payload; all
//   lighting and bounce sampling happens in RayGen, so every TraceRay is
//   issued from RayGen (recursion depth 1) and the path length is a loop.
//
//   Exports: RayGen / Miss / ShadowMiss / ClosestHit  (hit group "HitGroup")
//=============================================================================
#include "PBR.hlsli"

struct Vertex
{
    float3 position;
    float3 normal;
    float4 tangent;
    float2 texcoord;
};

//---- global root signature ---------------------------------------------------
RaytracingAccelerationStructure gScene  : register(t0);
RWTexture2D<float4>             gOutput : register(u0); // LDR, back-buffer format
RWTexture2D<float4>             gAccum  : register(u1); // HDR accumulation (sum)
RWTexture2D<float4>             gGeo    : register(u2); // G-buffer: normal.xyz + depth.w
RWTexture2D<float4>             gAlbedo : register(u3); // primary-hit base color (for demodulation)

cbuffer SceneCB : register(b0)
{
    row_major float4x4 invViewProj;
    float4 cameraPos;
    float4 lightDir;    // direction the light travels toward
    float4 lightColor;
    float4 ambient;
    uint4  frame;       // x=rng seed, y=maxDepth, z=accumIndex, w=render mode
    float4 envParams;   // x=env texture index (-1=none), y=intensity
};

Texture2D    gTextures[] : register(t3);
SamplerState gSampler    : register(s0);

//---- local root signature (per hit record) ----------------------------------
StructuredBuffer<Vertex> gVertices : register(t1);
StructuredBuffer<uint>   gIndices  : register(t2);
cbuffer HitCB : register(b1)
{
    float4 gBaseColor;    // dwords 0-3
    float4 gEmissive;     // dwords 4-7  (.rgb = emissive_factor)
    float  gMetallic;     // dword 8
    float  gRoughness;    // dword 9
    float  gNormalScale;  // dword 10
    int    gBaseColorTex; // dword 11
    int    gMRTex;        // dword 12
    int    gNormalTex;    // dword 13
    int    gEmissiveTex;  // dword 14
};

//---- payloads ---------------------------------------------------------------
// Surface data filled by ClosestHit and consumed by RayGen.
struct Payload
{
    float3 albedo;
    float3 normal;
    float3 worldPos;
    float3 emissive;
    float  metallic;
    float  roughness;
    float  hitT;      // < 0 : miss
};

struct ShadowPayload { float visible; };

//---- random numbers (PCG hash) ----------------------------------------------
uint pcg(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint w = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (w >> 22u) ^ w;
}
float rnd(inout uint s) { return pcg(s) * (1.0f / 4294967296.0f); }
uint  seedFrom(uint2 pix, uint frame) { return (pix.x * 1973u + pix.y * 9277u + frame * 26699u) | 1u; }

// Build an orthonormal basis around n.
void basis(float3 n, out float3 t, out float3 b)
{
    float3 up = abs(n.x) > 0.9f ? float3(0, 1, 0) : float3(1, 0, 0);
    t = normalize(cross(up, n));
    b = cross(n, t);
}

// Cosine-weighted hemisphere sample around n.
float3 cosineSample(float3 n, inout uint s)
{
    float u1 = rnd(s), u2 = rnd(s);
    float r = sqrt(u1), phi = 6.2831853f * u2;
    float3 t, b; basis(n, t, b);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0f, 1.0f - u1)));
}

// Uniform sample within a cone of half-angle acos(cosThetaMax) around dir.
float3 sampleCone(float3 dir, float cosThetaMax, inout uint s)
{
    float u1 = rnd(s), u2 = rnd(s);
    float cosT = lerp(1.0f, cosThetaMax, u1);
    float sinT = sqrt(saturate(1.0f - cosT * cosT));
    float phi = 6.2831853f * u2;
    float3 t, b; basis(dir, t, b);
    return normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + dir * cosT);
}

float3 skyColor(float3 d)
{
    // Equirectangular environment map when available.
    int envIdx = (int)envParams.x;
    if (envIdx >= 0)
    {
        float u = atan2(d.z, d.x) * (0.5f / PI) + 0.5f;
        float v = acos(clamp(d.y, -1.0f, 1.0f)) / PI;
        return gTextures[envIdx].SampleLevel(gSampler, float2(u, v), 0).rgb * envParams.y;
    }

    // Otherwise a simple procedural sky (dim so GI fill doesn't wash out).
    float t = saturate(d.y * 0.5f + 0.5f);
    return lerp(float3(0.03f, 0.04f, 0.06f), float3(0.25f, 0.38f, 0.55f), t);
}

//float3 tonemap(float3 c)
//{
//    // ACES filmic (Narkowicz) for punchier contrast, then gamma for the UNORM
//    // back buffer.
//    const float a = 2.51f, b = 0.03f, cc = 2.43f, d = 0.59f, e = 0.14f;
//    c = saturate((c * (a * c + b)) / (c * (cc * c + d) + e));
//    return pow(c, 1.0f / 2.2f);
//}
float3 tonemap(float3 c)
{
    c *= ambient.w; // exposure (driven by the UI slider)

    // ACES filmic (Narkowicz) for punchier contrast, then gamma for the UNORM
    // back buffer.
    const float a = 2.51f, b = 0.03f, cc = 2.43f, d = 0.59f, e = 0.14f;
    c = saturate((c * (a * c + b)) / (c * (cc * c + d) + e));
    
    // ÉKÉìÉ}ï‚ê≥
    return pow(c, 1.0f / 2.2f);
}

//---- shadow ray -------------------------------------------------------------
float traceShadow(float3 origin, float3 L)
{
    RayDesc r; r.Origin = origin; r.Direction = L; r.TMin = 1e-3f; r.TMax = 1e5f;
    ShadowPayload sp; sp.visible = 0.0f;
    TraceRay(gScene,
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             0xFF, 0, 0, 1, r, sp);
    return sp.visible;
}

//-----------------------------------------------------------------------------
// Ray generation: the path tracing loop.
//-----------------------------------------------------------------------------
[shader("raygeneration")]
void RayGen()
{
    uint2 pix = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    uint  s = seedFrom(pix, frame.x);
    uint  mode = frame.w;            // 0 = raytracing (realtime), 1 = path tracing
    uint  maxDepth = max(frame.y, 1u);

    // Sub-pixel jitter for AA (path tracing only; raytracing stays noise-free).
    float2 jitter = (mode == 1u) ? (float2(rnd(s), rnd(s)) - 0.5f) : float2(0.0f, 0.0f);
    float2 uv = (float2(pix) + 0.5f + jitter) / float2(dim);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    float4 farW = mul(float4(ndc, 1.0f, 1.0f), invViewProj); farW /= farW.w;
    float3 origin = cameraPos.xyz;
    float3 dir = normalize(farW.xyz - origin);

    //=====================================================================
    // Raytracing mode: direct light + hard shadow + sharp mirror reflection.
    // Deterministic (no random sampling), so it is clean every frame even
    // while moving, and needs no accumulation.
    //=====================================================================
    if (mode == 0u)
    {
        float3 radiance = float3(0, 0, 0);
        float3 tp = float3(1, 1, 1);
        float3 geoN = float3(0, 0, 1);
        float  geoD = -1.0f;             // < 0 = background
        float3 geoAlb = float3(0, 0, 0);
        [loop]
        for (uint b = 0; b < maxDepth; ++b)
        {
            RayDesc ray; ray.Origin = origin; ray.Direction = dir; ray.TMin = 1e-3f; ray.TMax = 1e5f;
            Payload p; p.hitT = -1.0f;
            TraceRay(gScene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, p);
            if (p.hitT < 0.0f) { radiance += tp * skyColor(dir); break; }

            radiance += tp * p.emissive; // glowing surfaces
            float3 N = p.normal, V = -dir;
            if (b == 0u) { geoN = N; geoD = p.hitT; geoAlb = p.albedo; } // primary-hit G-buffer
            float3 L = normalize(-lightDir.xyz);
            float ndotl = saturate(dot(N, L));
            float vis = (ndotl > 0.0f) ? traceShadow(p.worldPos + N * 1e-2f, L) : 0.0f;
            radiance += tp * PBR_DirectLight(N, V, L, p.albedo, p.metallic, p.roughness, lightColor.rgb) * vis;
            radiance += tp * p.albedo * ambient.rgb * (1.0f - p.metallic); // ambient fill

            // Continue along the sharp mirror direction, weighted by Fresnel
            // and smoothness; rough surfaces stop (throughput -> ~0).
            float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), p.albedo, p.metallic);
            float NdotV = saturate(dot(N, V));
            float3 F = F0 + (1.0f - F0) * pow(saturate(1.0f - NdotV), 5.0f);
            tp *= F * (1.0f - p.roughness);
            if (max(tp.r, max(tp.g, tp.b)) < 0.02f) break;

            origin = p.worldPos + N * 1e-3f;
            dir = reflect(dir, N);
        }
        gGeo[pix] = float4(geoN, geoD);
        gAlbedo[pix] = float4(geoAlb, 1.0f);
        gOutput[pix] = float4(tonemap(radiance), 1.0f);
        return;
    }

    //=====================================================================
    // Path tracing mode: full Monte-Carlo GI with temporal accumulation.
    // Trace several samples per pixel per frame (envParams.z) so moving frames
    // are less noisy before the denoiser ever runs.
    //=====================================================================
    uint   spp = (uint)max(envParams.z, 1.0f);
    float3 radiance = float3(0, 0, 0);
    float3 geoN = float3(0, 0, 1);
    float  geoD = -1.0f;                 // < 0 = background
    float3 geoAlb = float3(0, 0, 0);
    bool   captured = false;

    for (uint sp = 0; sp < spp; ++sp)
    {
        // Per-sample primary ray (own sub-pixel jitter for anti-aliasing).
        float2 jit = float2(rnd(s), rnd(s)) - 0.5f;
        float2 suv = (float2(pix) + 0.5f + jit) / float2(dim);
        float2 sndc = float2(suv.x * 2.0f - 1.0f, 1.0f - suv.y * 2.0f);
        float4 sfarW = mul(float4(sndc, 1.0f, 1.0f), invViewProj); sfarW /= sfarW.w;
        float3 sOrigin = cameraPos.xyz;
        float3 sDir = normalize(sfarW.xyz - sOrigin);

        float3 sampleRad = float3(0, 0, 0);
        float3 throughput = float3(1, 1, 1);

        [loop]
        for (uint b = 0; b < maxDepth; ++b)
        {
            RayDesc ray; ray.Origin = sOrigin; ray.Direction = sDir; ray.TMin = 1e-3f; ray.TMax = 1e5f;
            Payload p; p.hitT = -1.0f;
            TraceRay(gScene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, p);

            if (p.hitT < 0.0f)
            {
                sampleRad += throughput * skyColor(sDir);
                break;
            }

            sampleRad += throughput * p.emissive; // glowing surfaces contribute + drive GI

            float3 N = p.normal;
            float3 V = -sDir;
            if (b == 0u && !captured)        // primary-hit G-buffer (first sample)
            {
                geoN = N; geoD = p.hitT; geoAlb = p.albedo; captured = true;
            }

            // Direct light with a soft (sampled) sun direction.
            float3 L = normalize(-lightDir.xyz);
            float3 Ls = sampleCone(L, 0.9995f, s);
            float  ndotl = saturate(dot(N, Ls));
            if (ndotl > 0.0f)
            {
                float vis = traceShadow(p.worldPos + N * 1e-2f, Ls);
                sampleRad += throughput *
                    PBR_DirectLight(N, V, Ls, p.albedo, p.metallic, p.roughness, lightColor.rgb) * vis;
            }

            // ---- indirect bounce: pick a diffuse or specular lobe ---------
            float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), p.albedo, p.metallic);
            float  NdotV = saturate(dot(N, V));
            float3 F = F0 + (1.0f - F0) * pow(saturate(1.0f - NdotV), 5.0f);
            float  pSpec = clamp(dot(F, float3(0.3333f, 0.3333f, 0.3333f)), 0.05f, 0.95f);

            float3 newDir;
            if (rnd(s) < pSpec)
            {
                float3 R = reflect(sDir, N);
                float a2 = p.roughness * p.roughness;
                float cosMax = lerp(0.99999f, 0.60f, a2);
                newDir = sampleCone(R, cosMax, s);
                if (dot(newDir, N) <= 0.0f) break;
                throughput *= F / pSpec;
            }
            else
            {
                newDir = cosineSample(N, s);
                throughput *= (p.albedo * (1.0f - p.metallic)) / (1.0f - pSpec);
            }

            sOrigin = p.worldPos + N * 1e-3f;
            sDir = newDir;

            if (b >= 3u)
            {
                float q = max(throughput.r, max(throughput.g, throughput.b));
                if (rnd(s) > q) break;
                throughput /= max(q, 1e-4f);
            }
        }

        // Firefly suppression: clamp a single sample's extreme luminance so rare
        // high-variance paths don't leave sparkling pixels the denoiser can only
        // smear around. Slightly biased, but far less shimmery.
        float lum = dot(sampleRad, float3(0.2126f, 0.7152f, 0.0722f));
        const float maxLum = 10.0f;
        if (lum > maxLum) sampleRad *= maxLum / lum;

        radiance += sampleRad;
    }
    radiance /= float(spp);

    // ---- temporal accumulation ---------------------------------------------
    uint acc = frame.z;
    float3 prev = (acc == 0u) ? float3(0, 0, 0) : gAccum[pix].rgb;
    float3 sum = prev + radiance;
    gAccum[pix] = float4(sum, 1.0f);

    gGeo[pix] = float4(geoN, geoD);
    gAlbedo[pix] = float4(geoAlb, 1.0f);

    float3 avg = sum / float(acc + 1u);
    gOutput[pix] = float4(tonemap(avg), 1.0f);
}

//-----------------------------------------------------------------------------
// Primary miss (unused for shading; RayGen handles the sky via p.hitT<0).
//-----------------------------------------------------------------------------
[shader("miss")]
void Miss(inout Payload p)
{
    p.hitT = -1.0f;
}

//-----------------------------------------------------------------------------
// Shadow miss: the shadow ray reached the light unobstructed.
//-----------------------------------------------------------------------------
[shader("miss")]
void ShadowMiss(inout ShadowPayload s)
{
    s.visible = 1.0f;
}

//-----------------------------------------------------------------------------
// Closest hit: fill surface data for the RayGen path loop.
//-----------------------------------------------------------------------------
[shader("closesthit")]
void ClosestHit(inout Payload payload,
                in BuiltInTriangleIntersectionAttributes attr)
{
    uint prim = PrimitiveIndex();
    uint i0 = gIndices[prim * 3 + 0];
    uint i1 = gIndices[prim * 3 + 1];
    uint i2 = gIndices[prim * 3 + 2];

    Vertex v0 = gVertices[i0];
    Vertex v1 = gVertices[i1];
    Vertex v2 = gVertices[i2];

    float3 bc = float3(1.0f - attr.barycentrics.x - attr.barycentrics.y,
                       attr.barycentrics.x, attr.barycentrics.y);

    float3 nObj = normalize(v0.normal * bc.x + v1.normal * bc.y + v2.normal * bc.z);
    float2 uv   = v0.texcoord * bc.x + v1.texcoord * bc.y + v2.texcoord * bc.z;
    float4 tang = v0.tangent  * bc.x + v1.tangent  * bc.y + v2.tangent  * bc.z;

    float3x3 obj2world = (float3x3)ObjectToWorld3x4();
    float3 Ngeom = normalize(mul(obj2world, nObj));
    float3 N = Ngeom;
    if (gNormalTex >= 0)
    {
        float3 T = normalize(mul(obj2world, tang.xyz));
        T = normalize(T - dot(T, Ngeom) * Ngeom);
        float3 B = cross(Ngeom, T) * tang.w;
        float3 nT = gTextures[gNormalTex].SampleLevel(gSampler, uv, 0).xyz * 2.0f - 1.0f;
        nT.xy *= gNormalScale;
        N = normalize(nT.x * T + nT.y * B + nT.z * Ngeom);
    }

    float4 base = gBaseColor;
    if (gBaseColorTex >= 0)
        base *= gTextures[gBaseColorTex].SampleLevel(gSampler, uv, 0);

    float metallic = gMetallic;
    float roughness = gRoughness;
    if (gMRTex >= 0)
    {
        float4 mr = gTextures[gMRTex].SampleLevel(gSampler, uv, 0);
        roughness *= mr.g;
        metallic *= mr.b;
    }

    float3 emissive = gEmissive.rgb;
    if (gEmissiveTex >= 0)
        emissive *= gTextures[gEmissiveTex].SampleLevel(gSampler, uv, 0).rgb;

    payload.albedo = base.rgb;
    payload.normal = N;
    payload.worldPos = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
    payload.emissive = emissive;
    payload.metallic = metallic;
    payload.roughness = clamp(roughness, 0.04f, 1.0f);
    payload.hitT = RayTCurrent();
}
