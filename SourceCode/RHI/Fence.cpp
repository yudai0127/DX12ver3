#include "Fence.h"
#include <cassert>


//-----------------------------------------------------------------------------
// Initialize
//-----------------------------------------------------------------------------
bool Fence::Initialize(ID3D12Device* device, const wchar_t* debugName)
{
    assert(device);

    // フェンス生成（初期値 0）
    if (FAILED(device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
    {
        OutputDebugStringW(L"[DX12] Fence 生成失敗\n");
        return false;
    }

    // デバッグ名を設定（PIX / GPU デバッガで見やすくなる）
    m_fence->SetName(debugName);

    // CPU 待機用イベントオブジェクトを作成
    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_event)
    {
        OutputDebugStringW(L"[DX12] Fence イベント生成失敗\n");
        return false;
    }

    m_nextFenceValue = 1;
    return true;
}

//-----------------------------------------------------------------------------
// Uninitialize
//-----------------------------------------------------------------------------
void Fence::Uninitialize()
{
    if (m_event)
    {
        CloseHandle(m_event);
        m_event = nullptr;
    }
    m_fence = nullptr;
}

//-----------------------------------------------------------------------------
// Signal  ―  GPU コマンドキューに Signal コマンドを積む
//-----------------------------------------------------------------------------
uint64_t Fence::Signal(ID3D12CommandQueue* queue)
{
    assert(queue && m_fence);

    const uint64_t signalValue = m_nextFenceValue++;
    queue->Signal(m_fence.Get(), signalValue);
    return signalValue;
}

//-----------------------------------------------------------------------------
// WaitForValue  ―  指定した値が GPU に書き込まれるまで CPU をブロック
//-----------------------------------------------------------------------------
void Fence::WaitForValue(uint64_t value)
{
    assert(m_fence && m_event);

    // すでに完了していればノーコスト
    if (m_fence->GetCompletedValue() >= value) return;

    // 完了時にイベントを発火するよう登録
    m_fence->SetEventOnCompletion(value, m_event);

    // イベントが来るまで CPU スレッドをブロック
    // INFINITE にしているが、本番では適切なタイムアウトを設定するのも良い
    WaitForSingleObject(m_event, INFINITE);
}

//-----------------------------------------------------------------------------
// WaitForIdle  ―  キューの全コマンド完了を待つ
//-----------------------------------------------------------------------------
void Fence::WaitForIdle(ID3D12CommandQueue* queue)
{
    assert(queue);
    const uint64_t signalValue = Signal(queue);
    WaitForValue(signalValue);
}