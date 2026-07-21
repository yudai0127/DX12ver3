#include "ModelRenderer.h"
#include "Core/GameObject.h"
#include "Core/Transform.h"
#include "Camera/Camera.h"

using namespace DirectX;

Camera* ModelRenderer::s_camera = nullptr;

void ModelRenderer::OnRender(ID3D12GraphicsCommandList* commandList)
{
    if (!m_model || !m_model->IsValid() || !s_camera) return;

    XMFLOAT4X4 world;
    Transform* tf = GetOwner()->GetTransform();
    XMStoreFloat4x4(&world, tf->GetWorldMatrix());

    m_model->Render(commandList, world, m_color,
        s_camera->GetSceneCBAddress());
}