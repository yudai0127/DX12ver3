#pragma once
#include "Component.h"
#include "Transform.h"
#include <vector>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>


//-----------------------------------------------------------------------------
// GameObject
//  コンポーネントを保持するオブジェクト。
//  Transform は生成時に自動でアタッチされる。
//
//  使い方:
//    auto obj = std::make_shared<GameObject>("Player");
//    auto renderer = obj->AddComponent<MeshRenderer>();
//    auto tr = obj->GetComponent<Transform>();
//    tr->SetPosition(0, 1, 0);
//-----------------------------------------------------------------------------
class GameObject
{
public:
    explicit GameObject(const std::string& name = "GameObject");
    ~GameObject();

    // コピー禁止
    GameObject(const GameObject&) = delete;
    GameObject& operator=(const GameObject&) = delete;

    //-------------------------------------------------------------------
    // コンポーネント操作
    //-------------------------------------------------------------------

    /// @brief コンポーネントを追加してポインタを返す
    template<typename T, typename... Args>
    T* AddComponent(Args&&... args)
    {
        static_assert(std::is_base_of<Component, T>::value,
            "T は Component を継承している必要があります");

        auto comp = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = comp.get();
        ptr->SetOwner(this);
        ptr->OnStart();
        m_components[std::type_index(typeid(T))] = std::move(comp);
        return ptr;
    }

    /// @brief 型でコンポーネントを取得する（なければ nullptr）
    template<typename T>
    T* GetComponent() const
    {
        auto it = m_components.find(std::type_index(typeid(T)));
        if (it == m_components.end()) return nullptr;
        return static_cast<T*>(it->second.get());
    }

    /// @brief コンポーネントを削除する
    template<typename T>
    void RemoveComponent()
    {
        auto it = m_components.find(std::type_index(typeid(T)));
        if (it != m_components.end())
        {
            it->second->OnDestroy();
            m_components.erase(it);
        }
    }

    //-------------------------------------------------------------------
    // ライフサイクル
    //-------------------------------------------------------------------

    /// @brief 全コンポーネントの OnUpdate を呼ぶ（Scene から毎フレーム呼ばれる）
    void Update();

    /// @brief 全コンポーネントの OnRender を呼ぶ（Scene から毎フレーム呼ばれる）
    void Render(ID3D12GraphicsCommandList* commandList);

    //-------------------------------------------------------------------
    // アクセサ
    //-------------------------------------------------------------------
    const std::string& GetName()    const { return m_name; }
    void               SetName(const std::string& name) { m_name = name; }

    bool IsActive()          const { return m_active; }
    void SetActive(bool active) { m_active = active; }

    /// @brief Transform への直接アクセス（毎回 GetComponent しなくて済む）
    Transform* GetTransform() const { return m_transform; }

private:
    std::string m_name = "GameObject";
    bool        m_active = true;

    // type_index をキーにすることで型ごとに1つだけ持てる
    std::unordered_map<std::type_index, std::unique_ptr<Component>> m_components;

    Transform* m_transform = nullptr; // AddComponent<Transform> の結果をキャッシュ
};
