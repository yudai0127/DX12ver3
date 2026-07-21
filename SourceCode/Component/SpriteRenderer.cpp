#include "SpriteRenderer.h"
//-----------------------------------------------------------------------------
// OnRender
//-----------------------------------------------------------------------------
void SpriteRenderer::OnRender(ID3D12GraphicsCommandList* commandList)
{
    if (m_sprite && m_sprite->IsValid())
    {
        m_sprite->Render(commandList,
            m_x, m_y, m_w, m_h,
            m_r, m_g, m_b, m_a,
            m_angle,
            m_sx, m_sy, m_sw, m_sh);
    }
}