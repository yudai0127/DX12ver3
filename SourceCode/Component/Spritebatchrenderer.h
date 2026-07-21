#pragma once
#include "Core/Component.h"
#include "Graphics/SpriteBatch.h"
#include <memory>

//-----------------------------------------------------------------------------
// SpriteBatchRenderer
//  SpriteBatch を使って大量のスプライトを一括描画するコンポーネント。
//  授業 UNIT09 のパフォーマンステスト相当（同じ画像を大量に並べる）。
//
//  通常の SpriteRenderer が「1オブジェクト=1スプライト」なのに対し、
//  これは1コンポーネントで「大量のスプライト」をまとめて描く。
//-----------------------------------------------------------------------------
class SpriteBatchRenderer : public Component
{
public:
    /// @param textureFile 画像ファイル
    /// @param maxSprites  最大スプライト数
    SpriteBatchRenderer(const wchar_t* textureFile, size_t maxSprites)
    {
        m_batch = std::make_unique<SpriteBatch>(textureFile, maxSprites);
    }

    bool IsValid() const { return m_batch && m_batch->IsValid(); }

    /// @brief 切り出し範囲（スプライトシートの1コマ）
    void SetSourceRect(float sx, float sy, float sw, float sh)
    {
        m_sx = sx; m_sy = sy; m_sw = sw; m_sh = sh;
    }

    /// @brief 並べる1枚あたりのサイズ
    void SetSpriteSize(float w, float h) { m_w = w; m_h = h; }

    /// @brief 並べる枚数
    void SetCount(size_t count) { m_count = count; }

    void OnRender(ID3D12GraphicsCommandList* commandList) override;

private:
    std::unique_ptr<SpriteBatch> m_batch;

    size_t m_count = 1000;
    float  m_w = 64.0f, m_h = 64.0f;
    float  m_sx = 0.0f, m_sy = 0.0f, m_sw = 0.0f, m_sh = 0.0f;
};