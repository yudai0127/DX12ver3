#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

using Microsoft::WRL::ComPtr;


//-----------------------------------------------------------------------------
// Fence
//  DX12 で最も重要な同期プリミティブ。
//
//  典型的な使い方:
//    1. CommandQueue に Signal() を積む → GPU が処理完了後に値を書く
//    2. CPU 側で WaitForValue() を呼ぶ → その値が来るまでブロック
//
//  フレームバッファリング時は「フレームごとに Signal 値をインクリメント」し
//  古いフレームの値が到達しているかチェックすることで
//  CPU が GPU を過剰に追い越すのを防ぐ。
//-----------------------------------------------------------------------------
class Fence
{
public:
    Fence() = default;
    ~Fence() { Uninitialize(); }

    Fence(const Fence&) = delete;
    Fence& operator=(const Fence&) = delete;

    //-------------------------------------------------------------------
    // 初期化 / 解放
    //-------------------------------------------------------------------
    bool Initialize(ID3D12Device* device, const wchar_t* debugName = L"Fence");
    void Uninitialize();

    //-------------------------------------------------------------------
    // Signal / Wait
    //-------------------------------------------------------------------

    /// @brief GPU コマンドキューに Signal コマンドを積む
    ///        GPU がここまで処理したら m_nextFenceValue を書き込む
    /// @return Signal した値（後で WaitForValue に渡す）
    uint64_t Signal(ID3D12CommandQueue* queue);

    /// @brief CPU が value に達するまでブロック待機する
    void WaitForValue(uint64_t value);

    /// @brief キューの全コマンド完了を待つ（デバッグ・シャットダウン用）
    void WaitForIdle(ID3D12CommandQueue* queue);

    //-------------------------------------------------------------------
    // アクセサ
    //-------------------------------------------------------------------
    uint64_t GetCompletedValue()  const { return m_fence->GetCompletedValue(); }
    uint64_t GetNextSignalValue() const { return m_nextFenceValue; }
    ID3D12Fence* GetFence()       const { return m_fence.Get(); }

    bool IsValid() const { return m_fence != nullptr; }

private:
    ComPtr<ID3D12Fence> m_fence;
    HANDLE              m_event = nullptr;
    uint64_t            m_nextFenceValue = 1; // 0 は「まだ何もしていない」を意味する
};