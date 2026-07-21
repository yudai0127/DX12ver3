#include "MeshRenderer.h"
#include "Core/GameObject.h"
#include "Core/Transform.h"
#include "Camera/Camera.h"

using namespace DirectX;

// 全 MeshRenderer で共有するカメラ
Camera* MeshRenderer::s_camera = nullptr;

//-----------------------------------------------------------------------------
// CreateCube
//-----------------------------------------------------------------------------
bool MeshRenderer::CreateCube()
{
    m_mesh = std::make_unique<Mesh>();
    return m_mesh->CreateCube();
}

//-----------------------------------------------------------------------------
// OnRender
//-----------------------------------------------------------------------------
void MeshRenderer::OnRender(ID3D12GraphicsCommandList* commandList)
{
    if (!m_mesh || !m_mesh->IsValid() || !s_camera) return;

    // Transform からワールド行列を取得
    XMFLOAT4X4 world;
    Transform* tf = GetOwner()->GetTransform();
    XMStoreFloat4x4(&world, tf->GetWorldMatrix());

    // 描画（シーン定数バッファはカメラから取得）
    m_mesh->Render(commandList, world, m_color,
        s_camera->GetSceneCBAddress());
}