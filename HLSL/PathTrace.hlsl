//=============================================================================
// PathTrace.hlsl  (DXR: PBR direct lighting + textures + hard shadows)
//   Primary rays through the camera, shaded with the same metallic-roughness
//   PBR BRDF as the rasterizer (PBR.hlsli), plus a traced hard shadow ray.
//
//   Exports (must match RaytracingPipeline export names):
//     RayGen / Miss / ShadowMiss / ClosestHit  (hit group "HitGroup")
//=============================================================================
#include "PBR.hlsli"   // PBR_DirectLight (GGX specular, Fresnel, metallic)

// Vertex layout must match GltfModel::Vertex (48 bytes, tightly packed).
struct Vertex
{
    float3 position;
    float3 normal;
    float4 tangent;
    float2 texcoord;
};

//---- global root signature ---------------------------------------------------
RaytracingAccelerationStructure gScene  : register(t0);
RWTexture2D<float4>             gOutput : register(u0);

cbuffer SceneCB : register(b0)
{
    row_major float4x4 invViewProj; // clip -> world (row-vector convention)
    float4 cameraPos;
    float4 lightDir;    // direction the light travels toward (app-provided)
    float4 lightColor;
    float4 ambient;
    uint4  frame;
};

// Bindless textures (one per glTF texture, all models concatenated).
Texture2D    gTextures[] : register(t3);
SamplerState gSampler    : register(s0);

//---- local root signature (per hit record) ----------------------------------
StructuredBuffer<Vertex> gVertices : register(t1);
StructuredBuffer<uint>   gIndices  : register(t2);
cbuffer HitCB : register(b1)
{
    float4 gBaseColor;    // basecolor_factor
    int    gBaseColorTex; // index into gTextures[], or -1
    float  gMetallic;     // metallic_factor
    float  gRoughness;    // roughness_factor
    int    gMRTex;        // metallic-roughness texture (G=rough, B=metal), or -1
    int    gNormalTex;    // normal map, or -1
    float  gNormalScale;  // normal map strength
};

//---- ray payloads -----------------------------------------------------------
struct Payload
{
    float3 color;
    float  hitT;
};

struct ShadowPayload
{
    float visible; // 1 = lit, 0 = occluded
};

//-----------------------------------------------------------------------------
// Ray generation: one primary ray per pixel.
//-----------------------------------------------------------------------------
[shader("raygeneration")]
void RayGen()
{
    uint2 pix = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    // Pixel center -> NDC. (DX NDC: x right, y up, z in [0,1].)
    float2 uv = (float2(pix) + 0.5f) / float2(dim);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    // Unproject a far-plane point (z = 1) to get the ray direction.
    float4 farW = mul(float4(ndc, 1.0f, 1.0f), invViewProj);
    farW /= farW.w;

    float3 origin = cameraPos.xyz;
    float3 dir = normalize(farW.xyz - origin);

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 1e-3f;
    ray.TMax = 1e5f;

    Payload payload;
    payload.color = float3(0, 0, 0);
    payload.hitT = -1.0f;

    TraceRay(gScene, RAY_FLAG_NONE, 0xFF, 0, 0, /*MissShaderIndex*/ 0, ray, payload);

    gOutput[pix] = float4(payload.color, 1.0f);
}

//-----------------------------------------------------------------------------
// Primary miss: simple sky.
//-----------------------------------------------------------------------------
[shader("miss")]
void Miss(inout Payload payload)
{
    payload.color = lerp(float3(0.02f, 0.02f, 0.04f),
                         float3(0.35f, 0.55f, 0.85f), 0.5f);
    payload.hitT = -1.0f;
}

//-----------------------------------------------------------------------------
// Shadow miss: the shadow ray reached the light unobstructed.
//-----------------------------------------------------------------------------
[shader("miss")]
void ShadowMiss(inout ShadowPayload s)
{
    s.visible = 1.0f;
}

// Trace a shadow ray toward the light; returns 1 if lit, 0 if occluded.
float TraceShadow(float3 origin, float3 L)
{
    RayDesc sray;
    sray.Origin = origin;
    sray.Direction = L;
    sray.TMin = 1e-3f;
    sray.TMax = 1e5f;

    ShadowPayload sp;
    sp.visible = 0.0f; // assume occluded; ShadowMiss flips it to 1
    TraceRay(gScene,
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             0xFF, 0, 0, /*MissShaderIndex*/ 1, sray, sp);
    return sp.visible;
}

//-----------------------------------------------------------------------------
// Closest hit: PBR shading with the directional light + traced shadow.
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

    // Interpolated attributes.
    float3 nObj = normalize(v0.normal * bc.x + v1.normal * bc.y + v2.normal * bc.z);
    float2 uv   = v0.texcoord * bc.x + v1.texcoord * bc.y + v2.texcoord * bc.z;
    float4 tang = v0.tangent  * bc.x + v1.tangent  * bc.y + v2.tangent  * bc.z;

    float3x3 obj2world = (float3x3)ObjectToWorld3x4();
    float3 Ngeom = normalize(mul(obj2world, nObj));
    float3 N = Ngeom;

    // Normal mapping (tangent space -> world) when a normal map is present.
    if (gNormalTex >= 0)
    {
        float3 T = normalize(mul(obj2world, tang.xyz));
        T = normalize(T - dot(T, Ngeom) * Ngeom);   // Gram-Schmidt
        float3 B = cross(Ngeom, T) * tang.w;        // tangent.w = handedness
        float3 nT = gTextures[gNormalTex].SampleLevel(gSampler, uv, 0).xyz * 2.0f - 1.0f;
        nT.xy *= gNormalScale;
        N = normalize(nT.x * T + nT.y * B + nT.z * Ngeom);
    }

    // Base color (factor * texture).
    float4 base = gBaseColor;
    if (gBaseColorTex >= 0)
        base *= gTextures[gBaseColorTex].SampleLevel(gSampler, uv, 0);

    // Metallic / roughness (factor * texture; glTF: G=rough, B=metal).
    float metallic = gMetallic;
    float roughness = gRoughness;
    if (gMRTex >= 0)
    {
        float4 mr = gTextures[gMRTex].SampleLevel(gSampler, uv, 0);
        roughness *= mr.g;
        metallic *= mr.b;
    }
    roughness = clamp(roughness, 0.04f, 1.0f);

    float3 hitPos = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
    float3 V = normalize(-WorldRayDirection());   // toward the camera
    float3 L = normalize(-lightDir.xyz);          // toward the light

    // Hard shadow (offset along the geometric normal to avoid self-hit).
    float ndotl = saturate(dot(N, L));
    float shadow = (ndotl > 0.0f) ? TraceShadow(hitPos + Ngeom * 1e-2f, L) : 1.0f;

    // PBR direct lighting (shared with the rasterizer) + simple ambient.
    float3 direct = PBR_DirectLight(N, V, L, base.rgb, metallic, roughness,
                                    lightColor.rgb) * shadow;
    float3 amb = base.rgb * ambient.rgb * (1.0f - metallic);

    payload.color = direct + amb;
    payload.hitT = RayTCurrent();
}
