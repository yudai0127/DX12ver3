#pragma once
#include <cstdint>
#include <array>

//-----------------------------------------------------------------------------
// DrawCategory
//  描画の種類。カテゴリごとに集計することで
//  「何がドローコールを多く使っているか」が分かる。
//-----------------------------------------------------------------------------
enum class DrawCategory
{
    Model,   // glTFモデル
    Mesh,    // プリミティブ（立方体など）
    Sprite,  // 2Dスプライト（個別描画）
    Batch,   // スプライトバッチ（まとめて描画）
    Other,   // その他
    Count    // ← カテゴリ数（配列サイズ用。実際の分類ではない）
};

//-----------------------------------------------------------------------------
// RenderStats
//  1フレームの描画統計（ドローコール数・三角形数）をカテゴリ別に集計する。
//
//  ドローコールは「GPUへの描画命令」で、回数が多いほど CPU 負荷が高い。
//  カテゴリ別に見ることで、どの描画が重いかを特定できる。
//
//  使い方:
//    フレーム開始: RenderStats::Instance().Reset();
//    描画のたび:   RenderStats::Instance().AddDrawCall(DrawCategory::Model, tris);
//    表示:         GetDrawCalls(cat) / GetDrawCallsTotal()
//-----------------------------------------------------------------------------
class RenderStats
{
public:
    static RenderStats& Instance()
    {
        static RenderStats inst;
        return inst;
    }

    /// @brief フレーム開始時にカウンタをクリア
    void Reset()
    {
        m_drawCalls.fill(0);
        m_triangles.fill(0);
    }

    /// @brief 描画命令を1回記録する
    /// @param category 描画の種類
    /// @param triangles この描画で描いた三角形の数
    void AddDrawCall(DrawCategory category, uint32_t triangles = 0)
    {
        const size_t i = static_cast<size_t>(category);
        ++m_drawCalls[i];
        m_triangles[i] += triangles;
    }

    /// @brief カテゴリ別のドローコール数
    uint32_t GetDrawCalls(DrawCategory category) const
    {
        return m_drawCalls[static_cast<size_t>(category)];
    }

    /// @brief カテゴリ別の三角形数
    uint32_t GetTriangles(DrawCategory category) const
    {
        return m_triangles[static_cast<size_t>(category)];
    }

    /// @brief 全カテゴリ合計のドローコール数
    uint32_t GetDrawCallsTotal() const
    {
        uint32_t sum = 0;
        for (uint32_t v : m_drawCalls) sum += v;
        return sum;
    }

    /// @brief 全カテゴリ合計の三角形数
    uint32_t GetTrianglesTotal() const
    {
        uint32_t sum = 0;
        for (uint32_t v : m_triangles) sum += v;
        return sum;
    }

    /// @brief カテゴリ名（表示用）
    static const char* GetCategoryName(DrawCategory category)
    {
        switch (category)
        {
        case DrawCategory::Model:  return "Model";
        case DrawCategory::Mesh:   return "Mesh";
        case DrawCategory::Sprite: return "Sprite";
        case DrawCategory::Batch:  return "Batch";
        default:                   return "Other";
        }
    }

private:
    RenderStats() = default;
    RenderStats(const RenderStats&) = delete;
    RenderStats& operator=(const RenderStats&) = delete;

    static const size_t CATEGORY_COUNT = static_cast<size_t>(DrawCategory::Count);

    std::array<uint32_t, CATEGORY_COUNT> m_drawCalls = {};
    std::array<uint32_t, CATEGORY_COUNT> m_triangles = {};
};
