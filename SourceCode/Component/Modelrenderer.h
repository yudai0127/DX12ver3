#pragma once
#include "Core/Component.h"
#include "Graphics/GltfModel.h"
#include <memory>
#include <string>

class Camera;

//-----------------------------------------------------------------------------
// ModelRenderer
//  GameObject にアタッチしてモデル（glTF / 将来FBX）を描画するコンポーネント。
//  現在の中身は GltfModel。将来 FBX を追加するときは、共通の Model クラスに
//  まとめてローダーを切り替える設計に発展させる予定。
//-----------------------------------------------------------------------------
class ModelRenderer : public Component
{
public:
    /// @param filename モデルファイルパス（.gltf / .glb）
    explicit ModelRenderer(const std::string& filename)
    {
        m_model = std::make_unique<GltfModel>(filename);
    }

    bool IsValid() const { return m_model && m_model->IsValid(); }

    void SetColor(float r, float g, float b, float a)
    {
        m_color = { r, g, b, a };
    }

    static void SetCamera(Camera* camera) { s_camera = camera; }

    /// @brief シェーダーを再コンパイル（ホットリロード）
    void ReloadShaders() { if (m_model) m_model->ReloadShaders(); }

    /// @brief .hlsl の更新を検知して自動リロード（毎フレーム呼ぶ）
    void CheckAutoReload() { if (m_model) m_model->CheckAutoReload(); }

    /// @brief シェーダーエディタ用：編集対象の .hlsl パス
    std::wstring GetVSPath() const { return m_model ? m_model->GetVSPath() : L""; }
    std::wstring GetPSPath() const { return m_model ? m_model->GetPSPath() : L""; }

    void OnRender(ID3D12GraphicsCommandList* commandList) override;

private:
    std::unique_ptr<GltfModel> m_model;
    DirectX::XMFLOAT4 m_color = { 1, 1, 1, 1 };

    static Camera* s_camera;
};