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

    bool IsValid() const { return m_queryHeap != nullptr; }

private:
    // 1フレームあたり 2個のタイムスタンプ（開始・終了）
    static const uint32_t TIMESTAMPS_PER_FRAME = 2;

    ComPtr<ID3D12QueryHeap> m_queryHeap;    // GPU側の時刻記録場所
    ComPtr<ID3D12Resource>  m_readbackBuf;  // CPUが読むためのバッファ

    uint64_t m_gpuFrequency = 0;    // GPUのタイムスタンプ周波数（1秒あたりのカウント）
    uint32_t m_frameCount = 0;
    float    m_gpuTimeMs = 0.0f; // 計測結果
};
