#pragma once
#include "GameObject.h"
#include <vector>
#include <memory>
#include <string>


//-----------------------------------------------------------------------------
// Scene
//  GameObject を一括管理するクラス。
//
//  使い方:
//    Scene scene;
//    auto player = scene.CreateObject("Player");
//    player->AddComponent<MeshRenderer>();
//
//    // メインループ内
//    scene.Update();
//    scene.Render();   // Phase2以降で実装
//-----------------------------------------------------------------------------
class Scene
{
public:
    explicit Scene(const std::string& name = "Scene");
    ~Scene() = default;

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    //-------------------------------------------------------------------
    // GameObject 操作
    //-------------------------------------------------------------------

    /// @brief 新しい GameObject を生成してシーンに追加する
    /// @return 生成した GameObject の生ポインタ（所有権はシーンが持つ）
    GameObject* CreateObject(const std::string& name = "GameObject");

    /// @brief 既存の GameObject をシーンに追加する
    void AddObject(std::shared_ptr<GameObject> obj);

    /// @brief 名前で GameObject を検索する（最初に見つかったもの）
    GameObject* FindObject(const std::string& name) const;

    /// @brief GameObject をシーンから削除する
    void DestroyObject(GameObject* obj);

    //-------------------------------------------------------------------
    // ライフサイクル
    //-------------------------------------------------------------------

    /// @brief 全 GameObject の Update を呼ぶ
    void Update();

    /// @brief 全 GameObject の Render を呼ぶ（描画コマンドを積む）
    void Render(ID3D12GraphicsCommandList* commandList);

    /// @brief 削除予約された GameObject を実際に削除する
    ///        Update の後に呼ぶ（Update 中に削除すると不具合が出るため）
    void FlushDestroyQueue();

    //-------------------------------------------------------------------
    // アクセサ
    //-------------------------------------------------------------------
    const std::string& GetName()       const { return m_name; }
    size_t             GetObjectCount() const { return m_objects.size(); }

    // 全オブジェクトへの読み取りアクセス（Renderer System が使う）
    const std::vector<std::shared_ptr<GameObject>>& GetObjects() const
    {
        return m_objects;
    }

private:
    std::string m_name;
    std::vector<std::shared_ptr<GameObject>> m_objects;
    std::vector<GameObject*>                 m_destroyQueue; // 削除予約リスト
};
