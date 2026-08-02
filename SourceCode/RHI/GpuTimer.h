#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// GpuTimer
//  GPU のタイムスタンプクエリを使って「GPUが1フレームの描画に
//  何ミリ秒かかったか」を計測する。
//
//  CPU時間と違い、GPUは非同期に動くので、CPU側の時計では測れない。
//  そこで「コマンドリストに時刻を記録する命令を埋め込む」方式を使う。
//
//  仕組み:
//    BeginFrame(cmd) … GPUがここを通った時刻を記録（開始）
//      ... 描画命令 ...
//    EndFrame(cmd)   … GPUがここを通った時刻を記録（終了）＋結果を解決
//    ↓ GPUが実行し終わった後（数フレーム後）
//    Resolve(frameIndex) … 記録された2つの時刻の差から GPU時間を算出
//
//  注意: 結果は「数フレーム前のもの」になる（GPUの実行を待つ必要があるため）。
//        体感できる遅れではないので実用上問題ない。
//-----------------------------------------------------------------------------
class GpuTimer
{
public:
    /// @brief クエリヒープと Readback バッファを作る
    /// @param frameCount フレーム多重度（DeviceManager の frameCount と同じ）
    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
        uint32_t frameCount);

    void Uninitialize();

    /// @brief フレーム開始時刻を記録（BeginFrame の描画前に呼ぶ）
    void BeginFrame(ID3D12GraphicsCommandList* cmd, uint32_t frameIndex);

    /// @brief フレーム終了時刻を記録し、結果を Readback へ解決（Execute の前に呼ぶ）
    void EndFrame(ID3D12GraphicsCommandList* cmd, uint32_t frameIndex);

    /// @brief 指定フレームの結果を読み出す（GPU完了後＝WaitForFrame の後に呼ぶ）
    void Resolve(uint32_t frameIndex);

    /// @brief 計測された GPU 時間(ms)
    float GetGpuTimeMs() const { return m_gpuTimeMs; }

    // Named sub-sections of the frame, each with its own timestamp pair,
    // so the overlay can show where the GPU time actually goes (the DLSS
    // evaluate on its own, per the perf checklist).
    enum Scope
    {
        ScopeTrace = 0,     // DispatchRays
        ScopeResolve,       // denoiser / TAA compute
        ScopeDLSS,          // Streamline evaluate (Super Resolution or RR)
        ScopePost,          // tonemap/upscale + copy to the back buffer
        ScopeCount
    };

    // Bracket a section on the current frame's command list. A section
    // that is never entered in a frame reports 0 for it.
    void BeginScope(ID3D12GraphicsCommandList* cmd, Scope s);
    void EndScope(ID3D12GraphicsCommandList* cmd, Scope s);

    // Result for the frame Resolve() last read (two frames back).
    float GetScopeMs(Scope s) const { return m_scopeMs[s]; }

    // The instance the frame loop drives, for callers that are not handed
    // a reference (the ray tracer brackets its own passes with this).
    static GpuTimer* Active() { return s_active; }

    bool IsValid() const { return m_queryHeap != nullptr; }

private:
    // 1フレームあたり 2個のタイムスタンプ（開始・終了）
    // 2 for the whole frame, plus a begin/end pair per scope.
    static const uint32_t TIMESTAMPS_PER_FRAME = 2 + 2 * ScopeCount;

    ComPtr<ID3D12QueryHeap> m_queryHeap;    // GPU側の時刻記録場所
    ComPtr<ID3D12Resource>  m_readbackBuf;  // CPUが読むためのバッファ

    uint64_t m_gpuFrequency = 0;    // GPUのタイムスタンプ周波数（1秒あたりのカウント）
    uint32_t m_frameCount = 0;
    uint32_t m_currentFrame = 0;            // set by BeginFrame
    float    m_scopeMs[ScopeCount] = {};
    bool     m_scopeOpen[ScopeCount] = {};  // BeginScope seen this frame
    // Which scopes were recorded, per in-flight frame; read by Resolve so
    // a pass that stopped running does not keep showing stale numbers.
    std::vector<uint8_t> m_scopeUsed;       // frameCount * ScopeCount
    static GpuTimer* s_active;
    float    m_gpuTimeMs = 0.0f; // 計測結果
};
