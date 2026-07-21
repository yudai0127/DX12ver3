#include "Scene.h"
#include <algorithm>


Scene::Scene(const std::string& name)
    : m_name(name)
{
}

//-----------------------------------------------------------------------------
// CreateObject
//-----------------------------------------------------------------------------
GameObject* Scene::CreateObject(const std::string& name)
{
    auto obj = std::make_shared<GameObject>(name);
    GameObject* ptr = obj.get();
    m_objects.push_back(std::move(obj));
    return ptr;
}

//-----------------------------------------------------------------------------
// AddObject
//-----------------------------------------------------------------------------
void Scene::AddObject(std::shared_ptr<GameObject> obj)
{
    m_objects.push_back(std::move(obj));
}

//-----------------------------------------------------------------------------
// FindObject
//-----------------------------------------------------------------------------
GameObject* Scene::FindObject(const std::string& name) const
{
    for (const auto& obj : m_objects)
    {
        if (obj->GetName() == name) return obj.get();
    }
    return nullptr;
}

//-----------------------------------------------------------------------------
// DestroyObject  ―  削除予約（即削除ではなく FlushDestroyQueue まで待つ）
//-----------------------------------------------------------------------------
void Scene::DestroyObject(GameObject* obj)
{
    m_destroyQueue.push_back(obj);
}

//-----------------------------------------------------------------------------
// Update
//-----------------------------------------------------------------------------
void Scene::Update()
{
    for (auto& obj : m_objects)
    {
        obj->Update();
    }
}

void Scene::Render(ID3D12GraphicsCommandList* commandList)
{
    for (auto& obj : m_objects)
    {
        obj->Render(commandList);
    }
}

//-----------------------------------------------------------------------------
// FlushDestroyQueue  ―  Update 後に呼んで実際に削除する
//-----------------------------------------------------------------------------
void Scene::FlushDestroyQueue()
{
    for (GameObject* target : m_destroyQueue)
    {
        m_objects.erase(
            std::remove_if(m_objects.begin(), m_objects.end(),
                [target](const std::shared_ptr<GameObject>& o)
                { return o.get() == target; }),
            m_objects.end());
    }
    m_destroyQueue.clear();
}