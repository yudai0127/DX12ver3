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
RWTexture2D<float2>             gMotion : register(u4); // pixel-space motion vector (prev - cur)

// Guide buffers DLSS Ray Reconstruction consumes. They describe the primary
// hit's surface so the AI denoiser knows what it is looking at; our own
// denoiser does not read them.
RWTexture2D<float4>             gNormalRough : register(u5); // normal.xyz + roughness.w
RWTexture2D<float4>             gSpecAlbedo  : register(u6); // specular albedo (F0)
RWTexture2D<float>              gLinearDepth : register(u7); // primary-hit distance

cbuffer SceneCB : register(b0)
{
    row_major float4x4 invViewProj;
    float4 cameraPos;   // xyz = eye, w = far plane distance
    float4 lightDir;    // direction the light travels toward
    float4 lightColor;
    float4 ambient;
    uint4  frame;       // x=rng seed, y=maxDepth, z=accumIndex, w=render mode
    float4 envParams;   // x=env texture index (-1=none), y=intensity, z=samples/frame
    row_major float4x4 prevViewProj; // previous frame's view-projection
    // Camera basis, used to build primary rays exactly. Reconstructing them by
    // inverse-projecting the far plane loses catastrophic precision when the
    // near/far ratio is large (0.01 to 400 here): the error lands in the ray
    // direction, so the whole image shimmers as the camera moves. The
    // rasteriser never shows it because it only ever uses forward transforms.
    float4 camRight;  // xyz = right, w = tan(fovY/2) * aspect
    float4 camUp;     // xyz = up,    w = tan(fovY/2)
    float4 camFwd;    // xyz = forward
    // x = 1 when prevViewProj is valid; yz = fixed sub-pixel jitter in pixels;
    // w = 1 when that fixed jitter must be used (DLSS supplies its own sequence
    // and needs to know exactly which offset we rendered with).
    float4 prevParams;
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
// Kept as small as it can be: the payload travels with every ray and its size
// drives register pressure across the whole state object. The hit position is
// deliberately absent - it is exactly origin + direction * hitT, so the caller
// that already holds the ray can rebuild it for free.
struct Payload
{
    float3 albedo;
    float3 normal;
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

//---- GGX specular sampling --------------------------------------------------
// Importance-sample the *visible* normal distribution (Heitz 2018). Sampling a
// cone around the mirror direction instead, as this used to, ignores the shape
// of the BRDF entirely and leaves glossy metal extremely noisy - which is what
// makes those surfaces crawl once a denoiser tries to reconstruct them.
float3 sampleGGXVNDF(float3 Ve, float alpha, float u1, float u2)
{
    // Stretch the view direction so the distribution becomes isotropic.
    float3 Vh = normalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z));

    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = (lensq > 0.0f) ? (float3(-Vh.y, Vh.x, 0.0f) * rsqrt(lensq))
                               : float3(1.0f, 0.0f, 0.0f);
    float3 T2 = cross(Vh, T1);

    // Uniform point on the projected disk, warped to the visible hemisphere.
    float r = sqrt(u1);
    float phi = 6.2831853f * u2;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float sv = 0.5f * (1.0f + Vh.z);
    t2 = (1.0f - sv) * sqrt(saturate(1.0f - t1 * t1)) + sv * t2;

    float3 Nh = t1 * T1 + t2 * T2 +
                sqrt(saturate(1.0f - t1 * t1 - t2 * t2)) * Vh;

    // Unstretch to get the half vector.
    return normalize(float3(alpha * Nh.x, alpha * Nh.y, max(0.0f, Nh.z)));
}

// Smith masking term for GGX, used as the VNDF estimator weight.
float smithG1(float NdotX, float alpha)
{
    float a2 = alpha * alpha;
    return 2.0f * NdotX /
           max(NdotX + sqrt(a2 + (1.0f - a2) * NdotX * NdotX), 1e-6f);
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

//---- camera rays ------------------------------------------------------------
// Exact primary ray for a pixel-space position, straight from the camera basis.
float3 cameraRay(float2 pixPos, uint2 dim)
{
    float2 uv  = pixPos / float2(dim);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    return normalize(camFwd.xyz
                   + camRight.xyz * (ndc.x * camRight.w)
                   + camUp.xyz    * (ndc.y * camUp.w));
}

//---- motion vectors ---------------------------------------------------------
// Pixel-space motion vector: where this world point was on the previous frame's
// screen, minus where it is now. This is the convention DLSS expects when
// mvecScale is set to {1/width, 1/height}. Matrices carry no jitter.
//
// The current position must account for the sub-pixel offset the primary ray
// actually used: worldPos lies along the *jittered* ray, so it reprojects to
// pix + 0.5 + jitter, not to the pixel centre. Subtracting the centre instead
// would bake the jitter into every motion vector and make a completely static
// camera report movement, which shows up as the image shaking.
float2 motionVector(float3 worldPos, uint2 pix, uint2 dim, float2 jit)
{
    if (prevParams.x < 0.5f) return float2(0.0f, 0.0f); // no history yet

    float4 pc = mul(float4(worldPos, 1.0f), prevViewProj);
    if (pc.w <= 0.0f) return float2(0.0f, 0.0f);        // behind the old camera

    float2 pndc = pc.xy / pc.w;
    float2 puv  = float2(pndc.x * 0.5f + 0.5f, 0.5f - pndc.y * 0.5f);
    float2 ppix = puv * float2(dim);
    return ppix - (float2(pix) + 0.5f + jit);
}

//---- linear depth -----------------------------------------------------------
// DLSS's kBufferTypeLinearDepth means view-space Z: the distance along the
// camera's forward axis, not the length of the ray. They differ by up to tens
// of percent toward the edges of the screen, and feeding the ray length makes
// DLSS misjudge disocclusion there and throw its history away.
float3 cameraForward() { return camFwd.xyz; }

float linearDepthOf(float3 worldPos)
{
    return dot(worldPos - cameraPos.xyz, cameraForward());
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

    // Sub-pixel jitter for the primary ray. Path tracing picks its own offset
    // per sample further down, so this one only serves raytracing mode. That
    // mode is deterministic and has no antialiasing of its own, so it renders
    // at whatever offset the temporal resolve was told about - DLSS's, or our
    // own TAA's. C++ leaves this at zero when neither is running.
    float2 jitter = (mode == 0u) ? prevParams.yz : float2(0.0f, 0.0f);
    float3 origin = cameraPos.xyz;
    float3 dir = cameraRay(float2(pix) + 0.5f + jitter, dim);

    // Unjittered centre ray, kept for background motion vectors (DLSS wants
    // motion built from jitter-free matrices).
    float3 origin0 = cameraPos.xyz;
    float3 dir0 = cameraRay(float2(pix) + 0.5f, dim);

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
        float3 geoPos = float3(0, 0, 0); // primary-hit world position (motion vectors)
        float  geoRough = 1.0f;          // primary-hit roughness  (DLSS guide buffers)
        float3 geoSpec = float3(0, 0, 0);// primary-hit specular albedo (F0)
        float  geoMetal = 0.0f;          // primary-hit metalness
        [loop]
        for (uint b = 0; b < maxDepth; ++b)
        {
            RayDesc ray; ray.Origin = origin; ray.Direction = dir; ray.TMin = 1e-3f; ray.TMax = 1e5f;
            Payload p; p.hitT = -1.0f;
            TraceRay(gScene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, p);
            if (p.hitT < 0.0f) { radiance += tp * skyColor(dir); break; }

            radiance += tp * p.emissive; // glowing surfaces
            float3 hitPos = origin + dir * p.hitT;
            float3 N = p.normal, V = -dir;
            if (b == 0u)                                   // primary-hit G-buffer
            {
                geoN = N; geoD = p.hitT; geoAlb = p.albedo; geoPos = hitPos;
                geoRough = p.roughness;
                geoSpec = lerp(float3(0.04f, 0.04f, 0.04f), p.albedo, p.metallic);
                geoMetal = p.metallic;
            }
            float3 L = normalize(-lightDir.xyz);
            float ndotl = saturate(dot(N, L));
            float vis = (ndotl > 0.0f) ? traceShadow(hitPos + N * 1e-2f, L) : 0.0f;
            radiance += tp * PBR_DirectLight(N, V, L, p.albedo, p.metallic, p.roughness, lightColor.rgb) * vis;
            radiance += tp * p.albedo * ambient.rgb * (1.0f - p.metallic); // ambient fill

            // Continue along the sharp mirror direction, weighted by Fresnel
            // and smoothness; rough surfaces stop (throughput -> ~0).
            float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), p.albedo, p.metallic);
            float NdotV = saturate(dot(N, V));
            float3 F = F0 + (1.0f - F0) * pow(saturate(1.0f - NdotV), 5.0f);
            tp *= F * (1.0f - p.roughness);
            if (max(tp.r, max(tp.g, tp.b)) < 0.02f) break;

            origin = hitPos + N * 1e-3f;
            dir = reflect(dir, N);
        }
        gGeo[pix] = float4(geoN, geoD);
        // Diffuse albedo only: metals have no diffuse lobe, and their colour
        // is already carried by the specular albedo. Handing the full base
        // colour to both would make DLSS demodulate metal twice.
        gAlbedo[pix] = float4(geoAlb * (1.0f - geoMetal), 1.0f);
        // Background pixels still move when the camera turns: reproject a point
        // far along the primary ray instead of a surface hit. geoPos came from
        // the jittered ray, so it reprojects to the jittered position; the
        // background point comes from the centre ray and does not.
        gMotion[pix] = (geoD > 0.0f)
            ? motionVector(geoPos, pix, dim, jitter)
            : motionVector(origin0 + dir0 * 1e6f, pix, dim, float2(0.0f, 0.0f));
        gNormalRough[pix] = float4(geoN, geoRough);
        gSpecAlbedo[pix] = float4(geoSpec, 1.0f);
        // View-space Z, and the far plane for sky so DLSS never sees a
        // depth beyond the frustum it was told about.
        gLinearDepth[pix] = (geoD > 0.0f) ? linearDepthOf(geoPos) : cameraPos.w;
        // Ray Reconstruction only accepts linear HDR, and it reads gAccum. This
        // mode does not accumulate, so the frame's radiance goes there as-is;
        // gOutput stays the tonemapped image the non-DLSS path presents.
        gAccum[pix] = float4(radiance, 1.0f);
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

    // Primary-hit surface data, summed over every sample that hit something and
    // averaged at the end. Taking it from a single sample instead leaves the
    // guide buffers as noisy as a 1-sample image no matter how many samples the
    // lighting gets: the normal map is point-sampled at whatever sub-pixel
    // position that one ray used, so it lands on a different texel each frame
    // and DLSS sees the surface itself changing orientation.
    float3 gnSum = float3(0, 0, 0);
    float3 galbSum = float3(0, 0, 0);
    float3 gspecSum = float3(0, 0, 0);
    float3 gposSum = float3(0, 0, 0);
    float2 gjitSum = float2(0, 0);
    float  gdSum = 0.0f, groughSum = 0.0f, gmetalSum = 0.0f;
    uint   gcount = 0;

    for (uint sp = 0; sp < spp; ++sp)
    {
        // Per-sample primary ray offset.
        //   Without DLSS: an independent random offset per sample (plain AA).
        //   With DLSS: centred on the offset DLSS was told about. Taking every
        //     sample at that exact point leaves texture and normal-map detail
        //     unfiltered, so it aliases differently each frame and the surface
        //     appears to crawl; spreading the samples symmetrically around the
        //     reported offset filters that detail within the frame while the
        //     mean still matches what DLSS expects.
        float2 jit;
        if (prevParams.w > 0.5f)
        {
            jit = prevParams.yz;
            if (spp > 1u) jit += float2(rnd(s), rnd(s)) - 0.5f;
        }
        else
        {
            jit = float2(rnd(s), rnd(s)) - 0.5f;
        }
        float3 sOrigin = cameraPos.xyz;
        float3 sDir = cameraRay(float2(pix) + 0.5f + jit, dim);

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

            float3 hitPos = sOrigin + sDir * p.hitT;
            float3 N = p.normal;
            float3 V = -sDir;
            if (b == 0u)                     // primary hit: feed the guide buffers
            {
                gnSum += N;
                gdSum += p.hitT;
                galbSum += p.albedo;
                gposSum += hitPos;
                groughSum += p.roughness;
                gspecSum += lerp(float3(0.04f, 0.04f, 0.04f), p.albedo, p.metallic);
                gmetalSum += p.metallic;
                gjitSum += jit;              // offset this sample was traced with
                ++gcount;
            }

            // Direct light with a soft (sampled) sun direction.
            float3 L = normalize(-lightDir.xyz);
            float3 Ls = sampleCone(L, 0.9995f, s);
            float  ndotl = saturate(dot(N, Ls));
            if (ndotl > 0.0f)
            {
                float vis = traceShadow(hitPos + N * 1e-2f, Ls);
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
                // Sample a half vector from the visible normal distribution,
                // reflect about it, and weight by the Smith masking term - the
                // estimator for VNDF sampling reduces to F * G1(L).
                float alpha = max(p.roughness * p.roughness, 1e-3f);
                float3 T, B; basis(N, T, B);
                float3 Vt = float3(dot(V, T), dot(V, B), dot(V, N));
                float3 Ht = sampleGGXVNDF(Vt, alpha, rnd(s), rnd(s));
                float3 H = normalize(Ht.x * T + Ht.y * B + Ht.z * N);

                newDir = reflect(-V, H);
                float NdotL = dot(N, newDir);
                if (NdotL <= 0.0f) break;

                float3 Fh = F0 + (1.0f - F0) *
                            pow(saturate(1.0f - saturate(dot(V, H))), 5.0f);
                throughput *= Fh * smithG1(NdotL, alpha) / pSpec;

            }
            else
            {
                newDir = cosineSample(N, s);
                throughput *= (p.albedo * (1.0f - p.metallic)) / (1.0f - pSpec);

            }

            sOrigin = hitPos + N * 1e-3f;
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

    // ---- resolve the guide buffers -----------------------------------------
    // Averaging over the samples that hit gives DLSS a filtered view of the
    // surface, so normal-map and texture detail stops flickering between
    // frames. gcount == 0 means every sample missed: a background pixel.
    float3 geoN = float3(0, 0, 1);
    float  geoD = -1.0f;                 // < 0 = background
    float3 geoAlb = float3(0, 0, 0);
    float3 geoPos = float3(0, 0, 0);
    float  geoRough = 1.0f;
    float3 geoSpec = float3(0, 0, 0);
    float  geoMetal = 0.0f;
    float2 geoJit = float2(0, 0);
    if (gcount > 0u)
    {
        float inv = 1.0f / float(gcount);
        geoN = normalize(gnSum * inv);
        geoD = gdSum * inv;
        geoAlb = galbSum * inv;
        geoPos = gposSum * inv;
        geoRough = groughSum * inv;
        geoSpec = gspecSum * inv;
        geoMetal = gmetalSum * inv;
        geoJit = gjitSum * inv;
    }

    // ---- temporal accumulation ---------------------------------------------
    // Buffers hold RUNNING MEANS rather than sums, so every consumer can read
    // them as-is; DLSS in particular cannot divide by a sample count.
    uint  acc = frame.z;
    float t = 1.0f / float(acc + 1u);   // acc == 0 -> t = 1: plain overwrite

    // Under DLSS nothing accumulates here: colour and guides alike are this
    // frame's values. RR runs its own temporal history over the colour, and it
    // expects each guide to describe the SAME sub-pixel position as the colour
    // and the jitter offset we report. A mean spans many jitter positions while
    // we report only the latest, so the guides would disagree with the colour
    // they steer - the same mismatch that shifted the image when the colour was
    // still being accumulated. Averaging linear depth is the worst of it: across
    // a silhouette it lands between the foreground and the background, handing
    // RR a surface that is not there. Feeding it pre-averaged guides also throws
    // away the sub-pixel detail DLSS exists to reconstruct.
    //
    // Our own denoiser is the opposite case: it has no network to lean on, so it
    // keeps the means and converges them while the camera is still.
    bool  dlssMode = (prevParams.w > 0.5f);
    float tAcc  = dlssMode ? 1.0f : t;
    bool  fresh = (acc == 0u) || dlssMode;   // no history to blend against

    float3 meanRad = lerp(fresh ? float3(0, 0, 0) : gAccum[pix].rgb,
                          radiance, tAcc);
    gAccum[pix] = float4(meanRad, 1.0f);

    float4 pGeo = fresh ? float4(0, 0, 0, 0) : gGeo[pix];
    gGeo[pix] = lerp(pGeo, float4(geoN, geoD), tAcc);

    // Diffuse albedo only: metals have no diffuse lobe, and their colour is
    // already carried by the specular albedo. Handing the full base colour to
    // both would make DLSS demodulate metal twice.
    float4 pAlb = fresh ? float4(0, 0, 0, 0) : gAlbedo[pix];
    gAlbedo[pix] = lerp(pAlb, float4(geoAlb * (1.0f - geoMetal), 1.0f), tAcc);

    float4 pNR = fresh ? float4(0, 0, 0, 0) : gNormalRough[pix];
    gNormalRough[pix] = lerp(pNR, float4(geoN, geoRough), tAcc);

    float4 pSA = fresh ? float4(0, 0, 0, 0) : gSpecAlbedo[pix];
    gSpecAlbedo[pix] = lerp(pSA, float4(geoSpec, 1.0f), tAcc);

    float thisDepth = (geoD > 0.0f) ? linearDepthOf(geoPos) : cameraPos.w;
    float pLD = fresh ? 0.0f : gLinearDepth[pix];
    gLinearDepth[pix] = lerp(pLD, thisDepth, tAcc);

    // Motion stays per-frame: it describes THIS frame's camera step, and while
    // accumulating the camera is not moving anyway.
    gMotion[pix] = (geoD > 0.0f)
        ? motionVector(geoPos, pix, dim, geoJit)
        : motionVector(origin0 + dir0 * 1e6f, pix, dim, float2(0.0f, 0.0f));

    gOutput[pix] = float4(tonemap(meanRad), 1.0f);
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
    payload.emissive = emissive;
    payload.metallic = metallic;
    payload.roughness = clamp(roughness, 0.04f, 1.0f);
    payload.hitT = RayTCurrent();
}
