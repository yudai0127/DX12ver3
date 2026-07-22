#include "CommandContext.h"
#include <cassert>


//-----------------------------------------------------------------------------
// Initialize
//-----------------------------------------------------------------------------
bool CommandContext::Initialize(ID3D12Device* device,
    D3D12_COMMAND_LIST_TYPE type,
    uint32_t frameCount)
{
    assert(device);
    m_type = type;
    m_frameCount = frameCount;

    // ---- コマンドキュー ------------------------------------------------
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = type;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue))))
    {
        OutputDebugStringW(L"[DX12] CommandQueue 生成失敗\n");
        return false;
    }
    m_queue->SetName(L"MainCommandQueue");

    // ---- コマンドアロケータ (フレーム数分) --------------------------------
    // アロケータは GPU が使い終わるまで Reset できない。
    // フレームごとに別アロケータを使うことで CPU と GPU を並走させる。
    m_allocators.resize(frameCount);
    for (uint32_t i = 0; i < frameCount; ++i)
    {
        if (FAILED(device->CreateCommandAllocator(
            type, IID_PPV_ARGS(&m_allocators[i]))))
        {
            OutputDebugStringW(L"[DX12] CommandAllocator 生成失敗\n");
            return false;
        }

        wchar_t name[64];
        swprintf_s(name, L"CommandAllocator[%u]", i);
        m_allocators[i]->SetName(name);
    }

    // ---- コマンドリスト ------------------------------------------------
    // 最初のアロケータで生成し、すぐクローズしておく（Reset 前提）
    if (FAILED(device->CreateCommandList(
        0, type,
        m_allocators[0].Get(),
        nullptr,                           // 初期 PSO は nullptr で OK
        IID_PPV_ARGS(&m_commandList))))
    {
        OutputDebugStringW(L"[DX12] CommandList 生成失敗\n");
        return false;
    }
    m_commandList->SetName(L"MainCommandList");

    // DXR: try to obtain the ID3D12GraphicsCommandList4 view (stays null on
    // machines without DXR; the raytracing renderer checks for it).
    m_commandList.As(&m_commandList4);
    m_commandList->Close(); // 生成直後はクローズ状態にする

    // ---- フェンス -------------------------------------------------------
    if (!m_fence.Initialize(device, L"MainFence"))
        return false;

    return true;
}

//-----------------------------------------------------------------------------
// Uninitialize
//-----------------------------------------------------------------------------
void CommandContext::Uninitialize()
{
    // すでに解放済み（Uninitialize が2回呼ばれた）場合は何もしない
    if (!m_queue) return;

    m_fence.WaitForIdle(m_queue.Get()); // 残コマンドをフラッシュ
    m_fence.Uninitialize();
    m_commandList = nullptr;
    m_allocators.clear();
    m_queue = nullptr;
}

//-----------------------------------------------------------------------------
// Reset  ―  新フレームのコマンド録画開始
//-----------------------------------------------------------------------------
void CommandContext::Reset(uint32_t frameIndex)
{
    assert(frameIndex < m_frameCount);

    // このフレームのアロケータをリセット（GPU 完了後のみ呼んで良い）
    m_allocators[frameIndex]->Reset();

    // コマンドリストをリセットして録画可能状態にする
    m_commandList->Reset(m_allocators[frameIndex].Get(), nullptr);
}

//-----------------------------------------------------------------------------
// Execute  ―  リストをクローズして Submit
//-----------------------------------------------------------------------------
uint64_t CommandContext::Execute()
{
    // Close → Execute → Signal の順は DX12 では必須
    m_commandList->Close();

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_queue->ExecuteCommandLists(_countof(lists), lists);

    return m_fence.Signal(m_queue.Get());
}

//-----------------------------------------------------------------------------
// ExecuteAndWait  ―  Submit + GPU 完了まで待機
//-----------------------------------------------------------------------------
void CommandContext::ExecuteAndWait()
{
    const uint64_t fenceValue = Execute();
    m_fence.WaitForValue(fenceValue);
}

//-----------------------------------------------------------------------------
// ResourceBarrier  ―  ステート遷移バリア
//-----------------------------------------------------------------------------
void CommandContext::ResourceBarrier(ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    m_commandList->ResourceBarrier(1, &barrier);
}

//-----------------------------------------------------------------------------
// ClearRenderTarget
//-----------------------------------------------------------------------------
void CommandContext::ClearRenderTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv,
    const float color[4])
{
    m_commandList->ClearRenderTargetView(rtv, color, 0, nullptr);
}

//-----------------------------------------------------------------------------
// ClearDepthStencil
//-----------------------------------------------------------------------------
void CommandContext::ClearDepthStencil(D3D12_CPU_DESCRIPTOR_HANDLE dsv,
    float depth, UINT8 stencil)
{
    m_commandList->ClearDepthStencilView(
        dsv,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        depth, stencil, 0, nullptr);
}