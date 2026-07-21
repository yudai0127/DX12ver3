#pragma once
#include "Component.h"
#include <DirectXMath.h>

using namespace DirectX;

class Transform : public Component
{
public:
    Transform() = default;

    void     SetPosition(float x, float y, float z) { m_position = { x, y, z }; m_dirty = true; }
    void     SetPosition(XMFLOAT3 pos) { m_position = pos;          m_dirty = true; }
    XMFLOAT3 GetPosition() const { return m_position; }

    void     SetRotation(float x, float y, float z) { m_rotation = { x, y, z }; m_dirty = true; }
    void     SetRotation(XMFLOAT3 rot) { m_rotation = rot;          m_dirty = true; }
    XMFLOAT3 GetRotation() const { return m_rotation; }

    void     SetScale(float x, float y, float z) { m_scale = { x, y, z };    m_dirty = true; }
    void     SetScale(float uniform) { SetScale(uniform, uniform, uniform); }
    XMFLOAT3 GetScale() const { return m_scale; }

    XMMATRIX GetWorldMatrix() const;
    void     UpdateMatrix() const;

private:
    XMFLOAT3 m_position = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 m_rotation = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 m_scale = { 1.0f, 1.0f, 1.0f };

    mutable XMFLOAT4X4 m_worldMatrix = {};
    mutable bool       m_dirty = true;
};
