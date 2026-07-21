#pragma once
#include "Core/Component.h"
#include "Graphics/Sprite.h"
#include <memory>

//-----------------------------------------------------------------------------
// SpriteRenderer
//  GameObject にアタッチして 2D スプライトを描画するコンポーネント。
//  コンストラクタで画像ファイルを受け取り、内部で Sprite を生成する。
//  （Sprite は DeviceManager から device 等を取得する）
//
//  使い方:
//    auto* sr = obj->AddComponent<SpriteRenderer>(L"resources/test.png");
//    sr->SetPosition(100, 100);
//-----------------------------------------------------------------------------
class SpriteRenderer : public Component
{
public:
    /// @brief 画像ファイルを指定して生成する
    ///        AddComponent<SpriteRenderer>(L"...") の引数がここに渡る
    explicit SpriteRenderer(const wchar_t* textureFile)
    {
        m_sprite = std::make_unique<Sprite>(textureFile);
    }

    /// @brief Sprite が正しく生成できたか
    bool IsValid() const { return m_sprite && m_sprite->IsValid(); }

    //-------------------------------------------------------------------
    // 表示パラメータ
    //-------------------------------------------------------------------
    void SetPosition(float x, float y) { m_x = x; m_y = y; }
    void SetSize(float w, float h) { m_w = w; m_h = h; }
    void SetColor(float r, float g, float b, float a)
    {
        m_r = r; m_g = g; m_b = b; m_a = a;
    }
    void SetAngle(float degree) { m_angle = degree; }
    void SetSourceRect(float sx, float sy, float sw, float sh)
    {
        m_sx = sx; m_sy = sy; m_sw = sw; m_sh = sh;
    }

    void OnRender(ID3D12GraphicsCommandList* commandList) override;

private:
    std::unique_ptr<Sprite> m_sprite;

    float m_x = 0.0f, m_y = 0.0f;
    float m_w = 256.0f, m_h = 256.0f;
    float m_r = 1.0f, m_g = 1.0f, m_b = 1.0f, m_a = 1.0f;
    float m_angle = 0.0f;
    float m_sx = 0.0f, m_sy = 0.0f, m_sw = 0.0f, m_sh = 0.0f;
};