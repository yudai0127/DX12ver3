#include "SpriteBatchRenderer.h"

//-----------------------------------------------------------------------------
// OnRender  ―  Begin → 大量 Render → End でまとめて1回描画
//-----------------------------------------------------------------------------
void SpriteBatchRenderer::OnRender(ID3D12GraphicsCommandList* commandList)
{
    if (!m_batch || !m_batch->IsValid()) return;

    m_batch->Begin();

    // 授業 UNIT09 のテスト相当：画面いっぱいに大量に並べる
    float x = 0.0f;
    float y = 0.0f;
    for (size_t i = 0; i < m_count; ++i)
    {
        m_batch->Render(
            x, static_cast<float>(static_cast<int>(y) % 720),
            m_w, m_h,
            1, 1, 1, 1, 0,
            m_sx, m_sy, m_sw, m_sh);

        x += 32.0f;
        if (x > 1280.0f - m_w)
        {
            x = 0.0f;
            y += 24.0f;
        }
    }

    m_batch->End(commandList); // ここで1回のドローコール
}