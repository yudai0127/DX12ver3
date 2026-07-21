#pragma once
#include <cstdint>

struct ID3D12GraphicsCommandList; // 前方宣言
class GameObject;                 // 前方宣言

//-----------------------------------------------------------------------------
// Component
//  すべてのコンポーネントの基底クラス。
//
//  使い方:
//    class MeshRenderer : public Component
//    {
//        void OnUpdate() override { ... }
//    };
//
//  GameObject に AddComponent<MeshRenderer>() でアタッチする。
//  親の GameObject には GetOwner() でアクセスできる。
//-----------------------------------------------------------------------------
class Component
{
public:
    Component() = default;
    virtual ~Component() = default;

    // コピー禁止
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;

    //-------------------------------------------------------------------
    // ライフサイクル（必要なものだけオーバーライドする）
    //-------------------------------------------------------------------

    /// @brief シーンに追加されたとき1回だけ呼ばれる
    virtual void OnStart() {}

    /// @brief 毎フレーム呼ばれる
    virtual void OnUpdate() {}

    /// @brief 毎フレームの描画時に呼ばれる（描画コマンドを積む）
    ///        commandList は BeginFrame 済み（RTV セット済み）の状態で渡される
    virtual void OnRender(ID3D12GraphicsCommandList* /*commandList*/) {}

    /// @brief シーンから削除されるとき1回だけ呼ばれる
    virtual void OnDestroy() {}

    //-------------------------------------------------------------------
    // アクセサ
    //-------------------------------------------------------------------
    GameObject* GetOwner() const { return m_owner; }
    bool IsEnabled()       const { return m_enabled; }
    void SetEnabled(bool v) { m_enabled = v; }

private:
    // GameObject だけが m_owner を設定できる
    friend class GameObject;
    void SetOwner(GameObject* owner) { m_owner = owner; }

    GameObject* m_owner = nullptr;
    bool        m_enabled = true;
};
