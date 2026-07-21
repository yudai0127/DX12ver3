#include "GltfModel.hlsli"
#include "PBR.hlsli"

float4 main(VS_OUT pin) : SV_TARGET
{
    MaterialData m = materials[material];

    // --- basecolor ---
    float4 baseColor = m.basecolor_factor;
    if (m.basecolor_texture > -1)
    {
        float4 tex = material_textures[m.basecolor_texture].Sample(linear_sampler, pin.texcoord);
        baseColor *= float4(pow(tex.rgb, 2.2), tex.a);
    }

    // --- normal マップ ---
    float3 N = normalize(pin.w_normal);
    if (m.normal_texture > -1 && has_tangent > 0)
    {
        float3 T = normalize(pin.w_tangent);
        float3 B = normalize(cross(N, T));
        float3x3 TBN = float3x3(T, B, N);
        float3 ns = material_textures[m.normal_texture].Sample(linear_sampler, pin.texcoord).xyz;
        float3 nt = (ns * 2.0 - 1.0) * float3(m.normal_scale, m.normal_scale, 1.0);
        N = normalize(mul(nt, TBN));
    }

    // --- metallic-roughness ---
    float metallic = m.metallic_factor;
    float roughness = m.roughness_factor;
    if (m.metallic_roughness_texture > -1)
    {
        float4 mr = material_textures[m.metallic_roughness_texture].Sample(linear_sampler, pin.texcoord);
        roughness *= mr.g;
        metallic *= mr.b;
    }

    // --- occlusion ---
    float occlusion = 1.0;
    if (m.occlusion_texture > -1)
    {
        float ao = material_textures[m.occlusion_texture].Sample(linear_sampler, pin.texcoord).r;
        occlusion = lerp(1.0, ao, m.occlusion_strength);
    }

    // --- emissive ---
    float3 emissive = m.emissive_factor;
    if (m.emissive_texture > -1)
        emissive *= pow(material_textures[m.emissive_texture].Sample(linear_sampler, pin.texcoord).rgb, 2.2);

    // --- PBR ライティング ---
    float3 V = normalize(camera_position.xyz - pin.w_position);
    float3 L = normalize(-light_direction.xyz);

    float3 direct = PBR_DirectLight(N, V, L, baseColor.rgb, metallic, roughness, light_color.rgb);
    float3 ambient = ambient_color.rgb * baseColor.rgb * occlusion;

    float3 color = direct + ambient + emissive;
    color = color / (color + 1.0);
    color = pow(color, 1.0 / 2.2);

   return float4(color, baseColor.a);
   // return float4(1, 0, 0, 1);
}