struct VS_OUT
{
    float4 position : SV_POSITION;
    float3 w_position : POSITION; // ワールド位置（視線計算用）
    float3 w_normal : NORMAL;
    float3 w_tangent : TANGENT;
    float2 texcoord : TEXCOORD;
};

cbuffer OBJECT_CONSTANT_BUFFER : register(b0)
{
    row_major float4x4 world;
    int material; // このプリミティブのマテリアル番号
    int has_tangent; // tangentがあるか
    int2 pad;
};

cbuffer SCENE_CONSTANT_BUFFER : register(b1)
{
    row_major float4x4 view_projection;
    float4 camera_position; // PBRの視線計算に必要
    float4 light_direction;
    float4 light_color;
    float4 ambient_color;
};

// マテリアル情報（DX11版の material_constants 相当）
struct MaterialData
{
    float4 basecolor_factor;
    float metallic_factor;
    float roughness_factor;
    int basecolor_texture; // テクスチャ番号（-1なら無し）
    int metallic_roughness_texture;
    int normal_texture;
    int emissive_texture;
    int occlusion_texture;
    float normal_scale;
    float3 emissive_factor;
    float occlusion_strength;
};
StructuredBuffer<MaterialData> materials : register(t0);

// テクスチャ配列（5種を連続SRVで）
Texture2D material_textures[] : register(t1);
SamplerState linear_sampler : register(s0);

#define TEX_BASECOLOR 0
#define TEX_MR        1
#define TEX_NORMAL    2
#define TEX_EMISSIVE  3
#define TEX_OCCLUSION 4