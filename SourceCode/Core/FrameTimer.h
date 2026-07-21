#pragma once
#include <array>

//-----------------------------------------------------------------------------
// FrameTimer
//  フレーム時間(ms)と FPS を計測し、ImGui で表示する。
//  最適化の第一歩は「計測」。どこが遅いかを知るための土台。
//
//  ・毎フレーム Update(elapsedTime) を呼ぶ
//  ・GPU時間や VRAM は外から Set して渡す
//  ・DrawImGui() でグラフと数値を表示
//
//  使い方:
//    m_frameTimer.Update(elapsedTime);          // Framework::Update で
//    m_frameTimer.SetGpuTimeMs(gpuMs);          // GpuTimer の結果
//    m_frameTimer.SetVramUsageMB(used, budget); // VRAM 使用量
//    m_frameTimer.DrawImGui();                  // ImGui 表示
//-----------------------------------------------------------------------------
class FrameTimer
{
public:
    /// @brief 経過時間を記録する（毎フレーム呼ぶ）
    /// @param elapsedTime 前フレームからの経過秒数
    void Update(float elapsedTime);

    /// @brief GPU時間(ms)を外から設定する（GpuTimer の結果を渡す）
    void SetGpuTimeMs(float ms) { m_gpuMs = ms; }

    /// @brief VRAM使用量を外から設定する（MB単位）
    void SetVramUsageMB(float usedMB, float budgetMB)
    {
        m_vramUsedMB = usedMB; m_vramBudgetMB = budgetMB;
    }

    /// @brief システムメモリ(CPU側RAM)使用量を設定する（MB単位）
    void SetRamUsageMB(float usedMB) { m_ramUsedMB = usedMB; }

    /// @brief ImGui ウィンドウに FPS・フレーム時間・グラフを表示
    void DrawImGui();

    // 数値を外から使いたい場合のアクセサ
    float GetFPS() const { return m_fps; }
    float GetFrameTimeMs() const { return m_avgMs; }

private:
    static const int HISTORY_SIZE = 120; // 履歴の長さ（約2秒分）

    std::array<float, HISTORY_SIZE> m_history = {}; // フレーム時間(ms)の履歴
    int   m_historyIndex = 0;   // 次に書き込む位置（リングバッファ）
    int   m_sampleCount = 0;   // 記録したフレーム数（起動直後の平均用）

    float m_fps = 0.0f;  // 平均FPS
    float m_avgMs = 0.0f;  // 平均フレーム時間(ms)
    float m_minMs = 0.0f;  // 最小
    float m_maxMs = 0.0f;  // 最大
    float m_gpuMs = 0.0f;  // GPU時間(ms)（GpuTimer から受け取る）

    float m_vramUsedMB = 0.0f; // VRAM使用量(MB)
    float m_vramBudgetMB = 0.0f; // VRAM上限(MB)
    float m_ramUsedMB = 0.0f; // システムメモリ(CPU側RAM)使用量(MB)

    // 表示の更新間隔（毎フレーム更新すると数値が揺れて読めない）
    float m_updateTimer = 0.0f;
    static constexpr float UPDATE_INTERVAL = 0.25f; // 0.25秒ごとに数値を更新
};
