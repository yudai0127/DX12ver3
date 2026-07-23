#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include "RHI/ConstantBuffer.h"

using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// Camera
//  カメラ（View/Projection行列）とライト方向を管理し、
//  シーン定数バッファ(b1)としてGPUに送る。
//  授業 UNIT11 手順7 の scene_constants に相当。
//
//  View       … カメラの位置・向きから「視点座標系」への変換
//  Projection … 遠近感をつけて画面に投影する変換
//-----------------------------------------------------------------------------
class Camera
{
public:
    // シーン定数バッファ(b1)の中身（シェーダーの SCENE_CONSTANT_BUFFER と一致）
    struct SceneConstants
    {
        DirectX::XMFLOAT4X4 view_projection;
        DirectX::XMFLOAT4   camera_position;  // PBRの視線計算用
        DirectX::XMFLOAT4   light_direction;
        DirectX::XMFLOAT4   light_color;
        DirectX::XMFLOAT4   ambient_color;
    };

    Camera() = default;
    ~Camera() = default;

    bool Initialize();

    /// @brief カメラ位置・注視点・画面比から行列を計算し、定数バッファを更新
    void Update(float aspectRatio);

    //-------------------------------------------------------------------
    // 設定（ImGui から変更できるように public な値で持つ）
    //-------------------------------------------------------------------
    DirectX::XMFLOAT3 eye = { 0.0f, 160.0f, 180.0f }; // カメラ位置
    DirectX::XMFLOAT3 focus = { 0.0f, 160.0f, 0.0f };   // 注視点
    DirectX::XMFLOAT3 up = { 0.0f, 1.0f, 0.0f };   // 上方向
    float             fovDegree = 30.0f;                  // 視野角
    float             nearZ = 0.01f;                  // 手前の表示限界（小さめでめり込み緩和）
    float             farZ = 400.0f;                 // 奥の表示限界（広めにして遠い/大きいモデルに対応）
    DirectX::XMFLOAT4 lightDir = { 1.0f, -1.0f, -1.0f, 0.0f }; // ライト方向
    DirectX::XMFLOAT4 lightColor = { 1.0f, 1.0f, 1.0f, 1.0f };  // ライト色
    float             lightIntensity = 3.0f;                     // light strength multiplier
    DirectX::XMFLOAT4 ambient = { 0.2f, 0.2f, 0.2f, 1.0f };  // 環境光

    /// @brief シーン定数バッファのGPUアドレス（Mesh::Render に渡す）
    D3D12_GPU_VIRTUAL_ADDRESS GetSceneCBAddress() const
    {
        return m_sceneCB.GetGpuAddress();
    }

private:
    ConstantBuffer m_sceneCB;
};