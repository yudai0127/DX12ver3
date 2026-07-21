#pragma once
#include "Core/Component.h"
#include "Graphics/Mesh.h"
#include <memory>

class Camera;

//-----------------------------------------------------------------------------
// MeshRenderer
//  GameObject にアタッチして 3D メッシュを描画するコンポーネント。
//  Transform の位置・回転・スケールからワールド行列を作って描画する。
//
//  使い方:
//    auto* mr = obj->AddComponent<MeshRenderer>();
//    mr->CreateCube();
//    mr->SetColor(0.5f, 0.8f, 0.2f, 1.0f);
//-----------------------------------------------------------------------------
class MeshRenderer : public Component
{
public:
    MeshRenderer() = default;

    /// @brief 立方体メッシュを作る
    bool CreateCube();

    void SetColor(float r, float g, float b, float a)
    {
        m_color = { r, g, b, a };
    }

    /// @brief 描画に使うカメラ（シーン定数バッファ）を設定する
    static void SetCamera(Camera* camera) { s_camera = camera; }

    bool IsValid() const { return m_mesh && m_mesh->IsValid(); }

    void OnRender(ID3D12GraphicsCommandList* commandList) override;

private:
    std::unique_ptr<Mesh> m_mesh;
    DirectX::XMFLOAT4 m_color = { 1, 1, 1, 1 };

    // 全 MeshRenderer で共有するカメラ（シーン定数バッファの供給元）
    static Camera* s_camera;
};
