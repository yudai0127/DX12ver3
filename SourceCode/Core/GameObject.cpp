#include "GameObject.h"

GameObject::GameObject(const std::string& name)
    : m_name(name)
{
    // Transform は必ずアタッチ（Unityと同じ）
    m_transform = AddComponent<Transform>();
}

GameObject::~GameObject()
{
    // 全コンポーネントの OnDestroy を呼んでから解放
    for (auto& [type, comp] : m_components)
    {
        comp->OnDestroy();
    }
}

void GameObject::Update()
{
    if (!m_active) return;

    for (auto& [type, comp] : m_components)
    {
        if (comp->IsEnabled())
        {
            comp->OnUpdate();
        }
    }
}

void GameObject::Render(ID3D12GraphicsCommandList* commandList)
{
    if (!m_active) return;

    for (auto& [type, comp] : m_components)
    {
        if (comp->IsEnabled())
        {
            comp->OnRender(commandList);
        }
    }
}