#include "Sprite.hlsli"
Texture2D color_map : register(t0);
SamplerState point_sampler : register(s0);
float4 main(VS_OUT pin) : SV_TARGET
{
    return color_map.Sample(point_sampler, pin.texcoord) * pin.color;
}