#ifndef PBR_HLSLI
#define PBR_HLSLI

static const float PI = 3.14159265f;

// GGX 法線分布関数（D項）：表面の微小面の向きの分布
float D_GGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

// フレネル（Schlick近似）：見る角度による反射率の変化
float3 F_Schlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// 幾何項（Smith-GGX）：微小面同士の遮蔽
float V_SmithGGX(float NdotV, float NdotL, float roughness)
{
    float a = roughness * roughness;
    float gv = NdotL * sqrt(NdotV * NdotV * (1.0 - a) + a);
    float gl = NdotV * sqrt(NdotL * NdotL * (1.0 - a) + a);
    return 0.5 / max(gv + gl, 1e-7);
}

// PBR の直接光計算（1つの方向ライト）
//   N:法線 V:視線 L:ライト方向 baseColor:基本色 metallic:金属度 roughness:粗さ
float3 PBR_DirectLight(float3 N, float3 V, float3 L,
                       float3 baseColor, float metallic, float roughness,
                       float3 lightColor)
{
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    // 金属は基本色が反射色、非金属は0.04
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metallic);

    float D = D_GGX(NdotH, roughness);
    float3 F = F_Schlick(VdotH, F0);
    float Vis = V_SmithGGX(NdotV, NdotL, roughness);

    // 鏡面反射
    float3 specular = D * F * Vis;

    // 拡散反射（金属は拡散しない）
    float3 kd = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kd * baseColor / PI;

    return (diffuse + specular) * lightColor * NdotL;
}

#endif