#include "GltfModel.hlsli"

VS_OUT main(float4 position : POSITION,
            float3 normal : NORMAL,
            float4 tangent : TANGENT,
            float2 texcoord : TEXCOORD)
{
    VS_OUT vout;
    float4 wpos = mul(position, world);
    vout.w_position = wpos.xyz;
    vout.position = mul(wpos, view_projection);

    vout.w_normal = normalize(mul(float4(normal, 0), world).xyz);
    vout.w_tangent = normalize(mul(float4(tangent.xyz, 0), world).xyz);
    vout.texcoord = texcoord;
    return vout;
}
