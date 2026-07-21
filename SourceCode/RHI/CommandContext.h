#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include "RHI/Fence.h"

using Microsoft::WRL::ComPtr;


using Microsoft::WRL::ComPtr;


//-----------------------------------------------------------------------------
// CommandContext
//  コマンドキュー・アロケータ・リストを一括管理するクラス。
//
//  DX12 のコマンド録画フロー:
//    1. Reset()         ― アロケータとリストをリセット（前フレームのGPU完了後）
//    2. [録画] …        ― ResourceBarrier, ClearRTV, DrawInstanced などを呼ぶ
//    3. Execute()       ― リストをクローズしてキューに Submit
//    4. Signal()        ― GPU に完了通知を要求 → Fence に値を書かせる
//
//  フレームバッファリング (N-buffering) では
//  アロケータを N 個持ち、インデックスをローテーションすることで
//  CPU が GPU を先行しすぎないように管理する。
//-----------------------------------------------------------------------------
class CommandContext
{
public:
    CommandContext() = default;
    ~CommandContext() { Uninitialize(); }

    CommandContext(const CommandContext&) = delete;
    CommandContext& operator=(const CommandContext&) = delete;

    //-------------------------------------------------------------------
    // 初期化 / 解放
    //-------------------------------------------------------------------

    /// @param frameCount フレームバッファ数（通常 2 or 3）
    bool Initialize(ID3D12Device* device,
        D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        uint32_t frameCount = 2);
    void Uninitialize();

    //-------------------------------------------------------------------
    // フレーム単位の操作
    //-------------------------------------------------------------------

    /// @brief 指定フレームのアロケータをリセットし、コマンド録画を開始する
    ///        ※ GPU がこのフレームを使い終わっていることを確認してから呼ぶこと
    void Reset(uint32_t frameIndex);

    /// @brief コマンドリストをクローズして GPU キューに Submit する
    /// @return Signal した Fence 値
    uint64_t Execute();

    /// @brief Execute + 完了まで CPU でブロック待機（デバッグ・初期化用）
    void ExecuteAndWait();

    //-------------------------------------------------------------------
    // アクセサ
    //-------------------------------------------------------------------
    ID3D12GraphicsCommandList* GetCommandList() const { return m_commandList.Get(); }
    ID3D12CommandQueue* GetQueue()       const { return m_queue.Get(); }
    Fence& GetFence() { return m_fence; }
    const Fence& GetFence()       const { return m_fence; }

    bool IsValid() const { return m_commandList != nullptr; }

    //-------------------------------------------------------------------
    // よく使うヘルパー
    //-------------------------------------------------------------------

    /// @brief リソースバリアをまとめて発行する
    void ResourceBarrier(ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after);

    /// @brief RTV をクリアする
    void ClearRenderTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        const float color[4]);

    /// @brief DSV をクリアする
    void ClearDepthStencil(D3D12_CPU_DESCRIPTOR_HANDLE dsv,
        float depth = 1.0f, UINT8 stencil = 0);

private:
    ComPtr<ID3D12CommandQueue>              m_queue;
    std::vector<ComPtr<ID3D12CommandAllocator>> m_allocators; // フレーム数分
    ComPtr<ID3D12GraphicsCommandList>       m_commandList;
    Fence                                   m_fence;

    D3D12_COMMAND_LIST_TYPE m_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    uint32_t                m_frameCount = 2;
};