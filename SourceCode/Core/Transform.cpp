#include "Transform.h"

XMMATRIX Transform::GetWorldMatrix() const
{
    if (m_dirty)
    {
        UpdateMatrix();
        m_dirty = false;
    }
    return XMLoadFloat4x4(&m_worldMatrix);
}

void Transform::UpdateMatrix() const
{
    XMMATRIX S = XMMatrixScaling(m_scale.x, m_scale.y, m_scale.z);
    XMMATRIX R = XMMatrixRotationRollPitchYaw(
        XMConvertToRadians(m_rotation.x),
        XMConvertToRadians(m_rotation.y),
        XMConvertToRadians(m_rotation.z));
    XMMATRIX T = XMMatrixTranslation(m_position.x, m_position.y, m_position.z);
    XMStoreFloat4x4(&m_worldMatrix, S * R * T);
}