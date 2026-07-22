//=============================================================================
// PathTrace.hlsl  (DXR milestone 1 + base-color textures)
//   Primary rays through the camera + hard shadows from the directional light,
//   with the glTF base-color texture sampled at the hit point.
//
//   Exports (must match RaytracingPipeline export names):
//     RayGen / Miss / ShadowMiss / ClosestHit  (hit group "HitGroup")
//=============================================================================

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

// Bindless base-color textures (one per glTF texture, all models concatenated).
Texture2D    gTextures[] : register(t3);
SamplerState gSampler    : register(s0);

//---- local root signature (per hit record) ----------------------------------
StructuredBuffer<Vertex> gVertices : register(t1);
StructuredBuffer<uint>   gIndices  : register(t2);
cbuffer HitCB : register(b1)
{
    float4 gBaseColor;    // basecolor_factor
    int    gBaseColorTex; // index into gTextures[], or -1 if none
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

    TraceRay(gScene, RAY_FLAG_NONE, 0xFF,
             /*RayContributionToHitGroupIndex*/ 0,
             /*MultiplierForGeometryContributionToHitGroupIndex*/ 0,
             /*MissShaderIndex*/ 0,
             ray, payload);

    gOutput[pix] = float4(payload.color, 1.0f);
}

//-----------------------------------------------------------------------------
// Primary miss: simple sky.
//-----------------------------------------------------------------------------
[shader("miss")]
void Miss(inout Payload payload)
{
    float t = 0.5f;
    payload.color = lerp(float3(0.02f, 0.02f, 0.04f),
                         float3(0.35f, 0.55f, 0.85f), t);
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

//-----------------------------------------------------------------------------
// Closest hit: interpolate normal + uv, sample the base-color texture, apply a
// directional light with a traced shadow ray, and add ambient.
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
    float3 nWorld = normalize(mul((float3x3)ObjectToWorld3x4(), nObj));

    float2 uv = v0.texcoord * bc.x + v1.texcoord * bc.y + v2.texcoord * bc.z;

    float3 hitPos = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();

    // Base color: factor, optionally modulated by the base-color texture.
    // Ray tracing has no automatic derivatives, so sample an explicit LOD 0.
    float4 base = gBaseColor;
    if (gBaseColorTex >= 0)
        base *= gTextures[gBaseColorTex].SampleLevel(gSampler, uv, 0);

    // Directional light: lightDir is the travel direction, so L points back to it.
    float3 L = normalize(-lightDir.xyz);
    float ndotl = saturate(dot(nWorld, L));

    float shadow = 1.0f;
    if (ndotl > 0.0f)
    {
        RayDesc sray;
        sray.Origin = hitPos + nWorld * 1e-2f; // offset to avoid self-hit
        sray.Direction = L;
        sray.TMin = 1e-3f;
        sray.TMax = 1e5f;

        ShadowPayload sp;
        sp.visible = 0.0f; // assume occluded; ShadowMiss flips it to 1
        TraceRay(gScene,
                 RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                 0xFF, 0, 0, /*MissShaderIndex*/ 1, sray, sp);
        shadow = sp.visible;
    }

    float3 lit = base.rgb * lightColor.rgb * (ndotl * shadow);
    float3 amb = base.rgb * ambient.rgb;

    payload.color = lit + amb;
    payload.hitT = RayTCurrent();
}
