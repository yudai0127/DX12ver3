struct VS_OUT
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

// オブジェクトごとの定数（b0）
cbuffer OBJECT_CONSTANT_BUFFER : register(b0)
{
    row_major float4x4 world;
    float4 material_color;
};

// シーン共通の定数（b1）
cbuffer SCENE_CONSTANT_BUFFER : register(b1)
{
    row_major float4x4 view_projection;
    float4 light_direction;
};