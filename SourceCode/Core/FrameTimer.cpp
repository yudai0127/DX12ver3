#include "Core/FrameTimer.h"
#include "Core/RenderStats.h"
#include "imgui.h"
#include <algorithm>

//-----------------------------------------------------------------------------
// Update  ―  経過時間を履歴に記録し、一定間隔で統計を更新
//-----------------------------------------------------------------------------
void FrameTimer::Update(float elapsedTime)
{
    // 経過時間をミリ秒に
    const float ms = elapsedTime * 1000.0f;

    // リングバッファに記録（古いものを上書きしていく）
    m_history[m_historyIndex] = ms;
    m_historyIndex = (m_historyIndex + 1) % HISTORY_SIZE;
    if (m_sampleCount < HISTORY_SIZE) ++m_sampleCount;

    // 数値表示は一定間隔で更新（毎フレームだと揺れて読めない）
    m_updateTimer += elapsedTime;
    if (m_updateTimer < UPDATE_INTERVAL) return;
    m_updateTimer = 0.0f;

    // 履歴から 平均・最小・最大 を計算
    float sum = 0.0f;
    float mn = m_history[0];
    float mx = m_history[0];
    for (int i = 0; i < m_sampleCount; ++i)
    {
        const float v = m_history[i];
        sum += v;
        mn = (std::min)(mn, v);
        mx = (std::max)(mx, v);
    }

    m_avgMs = (m_sampleCount > 0) ? sum / m_sampleCount : 0.0f;
    m_minMs = mn;
    m_maxMs = mx;
    m_fps = (m_avgMs > 0.0f) ? 1000.0f / m_avgMs : 0.0f;
}

//-----------------------------------------------------------------------------
// DrawImGui  ―  数値とグラフを表示
//-----------------------------------------------------------------------------
void FrameTimer::DrawImGui()
{
    ImGui::Begin("Performance");

    // 数値表示
    ImGui::Text("FPS: %.1f", m_fps);
    ImGui::Text("Frame: %.3f ms (min %.3f / max %.3f)", m_avgMs, m_minMs, m_maxMs);

    // GPU時間（計測できていれば）
    if (m_gpuMs > 0.0f)
    {
        ImGui::Text("GPU:   %.3f ms", m_gpuMs);

        // ボトルネックの目安（フレーム時間のうち GPU が占める割合で判断）
        const float gpuRatio = (m_avgMs > 0.0f) ? m_gpuMs / m_avgMs : 0.0f;
        if (gpuRatio > 0.9f)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "GPU bound");
        else if (gpuRatio < 0.5f)
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "CPU bound / VSync");
        else
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Balanced");
    }

    // 60FPS(16.6ms) / 30FPS(33.3ms) の目安
    if (m_avgMs <= 16.7f)
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "60 FPS OK");
    else if (m_avgMs <= 33.4f)
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "30-60 FPS");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Below 30 FPS");

    ImGui::Separator();

    // 描画統計（前フレームの値：Reset される前に読む）
    const auto& stats = RenderStats::Instance();
    ImGui::Text("Draw Calls: %u", stats.GetDrawCallsTotal());
    ImGui::Text("Triangles:  %u", stats.GetTrianglesTotal());

    // カテゴリ別の内訳（描画があったものだけ表示）
    if (ImGui::TreeNode("Breakdown"))
    {
        for (int i = 0; i < static_cast<int>(DrawCategory::Count); ++i)
        {
            const auto cat = static_cast<DrawCategory>(i);
            const uint32_t calls = stats.GetDrawCalls(cat);
            if (calls == 0) continue; // 使っていないカテゴリは出さない

            ImGui::Text("%-7s calls %5u  tris %8u",
                RenderStats::GetCategoryName(cat),
                calls, stats.GetTriangles(cat));
        }
        ImGui::TreePop();
    }

    // VRAM使用量（計測できていれば）
    if (m_vramBudgetMB > 0.0f)
    {
        ImGui::Text("VRAM: %.0f / %.0f MB", m_vramUsedMB, m_vramBudgetMB);
        const float ratio = m_vramUsedMB / m_vramBudgetMB;
        ImGui::ProgressBar(ratio, ImVec2(-FLT_MIN, 0));
    }

    // システムメモリ（CPU側RAM）使用量
    if (m_ramUsedMB > 0.0f)
        ImGui::Text("RAM:  %.0f MB", m_ramUsedMB);

    ImGui::Separator();

    // フレーム時間のグラフ（カクつきが視覚的に分かる）
    const float scaleMax = (m_maxMs > 33.3f) ? m_maxMs * 1.1f : 33.3f;
    ImGui::PlotLines("##frametime", m_history.data(), HISTORY_SIZE,
        m_historyIndex,      // リングバッファの開始位置
        "Frame Time (ms)",
        0.0f, scaleMax,
        ImVec2(0, 80));

    ImGui::End();
}