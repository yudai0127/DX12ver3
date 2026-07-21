#include "SwapChain.h"
#include <cassert>


//-----------------------------------------------------------------------------
// Initialize
//-----------------------------------------------------------------------------
bool SwapChain::Initialize(IDXGIFactory6* factory,
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    HWND                hwnd,
    uint32_t            width,
    uint32_t            height,
    uint32_t            frameCount)
{
    assert(factory && device && queue && hwnd);

    m_device = device;
    m_width = width;
    m_height = height;
    m_frameCount = frameCount;

    // RTV ディスクリプタのサイズを取得（ハードウェアごとに異なる）
    m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // ---- スワップチェーン生成 -----------------------------------------
    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width = width;
    scDesc.Height = height;
    scDesc.Format = BACK_BUFFER_FORMAT;
    scDesc.Stereo = FALSE;
    scDesc.SampleDesc = { 1, 0 };                       // MSAA なし
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = frameCount;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // DX12 推奨
    scDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    scDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(factory->CreateSwapChainForHwnd(
        queue,          // DX12 では CommandQueue を渡す（DX11 と異なる）
        hwnd,
        &scDesc,
        nullptr,        // フルスクリーン設定（nullptr = ウィンドウのみ）
        nullptr,        // 出力制限なし
        &swapChain1)))
    {
        OutputDebugStringW(L"[DX12] SwapChain 生成失敗\n");
        return false;
    }

    // Alt+Enter によるフルスクリーン切り替えを無効化
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // IDXGISwapChain4 にアップキャスト（GetCurrentBackBufferIndex が必要）
    if (FAILED(swapChain1.As(&m_swapChain)))
    {
        OutputDebugStringW(L"[DX12] SwapChain4 取得失敗\n");
        return false;
    }

    // ---- バックバッファ + RTV 生成 ------------------------------------
    if (!CreateRTVHeapAndViews()) return false;

    return true;
}

//-----------------------------------------------------------------------------
// Uninitialize
//-----------------------------------------------------------------------------
void SwapChain::Uninitialize()
{
    ReleaseBuffers();
    m_rtvHeap = nullptr;
    m_swapChain = nullptr;
    m_device = nullptr;
}

//-----------------------------------------------------------------------------
// GetCurrentBackBufferIndex
//-----------------------------------------------------------------------------
uint32_t SwapChain::GetCurrentBackBufferIndex() const
{
    return m_swapChain->GetCurrentBackBufferIndex();
}

//-----------------------------------------------------------------------------
// GetCurrentBackBuffer
//-----------------------------------------------------------------------------
ID3D12Resource* SwapChain::GetCurrentBackBuffer() const
{
    return m_backBuffers[GetCurrentBackBufferIndex()].Get();
}

//-----------------------------------------------------------------------------
// GetCurrentRTV
//-----------------------------------------------------------------------------
D3D12_CPU_DESCRIPTOR_HANDLE SwapChain::GetCurrentRTV() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    // ヒープは配列なので、インデックス分だけオフセット
    handle.ptr += static_cast<SIZE_T>(GetCurrentBackBufferIndex())
        * m_rtvDescriptorSize;
    return handle;
}

//-----------------------------------------------------------------------------
// Present
//-----------------------------------------------------------------------------
void SwapChain::Present(uint32_t syncInterval)
{
    // Present 前にバックバッファのステートが
    // D3D12_RESOURCE_STATE_PRESENT であることが必須。
    // （CommandContext::ResourceBarrier で遷移しておくこと）
    m_swapChain->Present(syncInterval, 0);
}

//-----------------------------------------------------------------------------
// Resize
//-----------------------------------------------------------------------------
bool SwapChain::Resize(uint32_t width, uint32_t height)
{
    if (width == m_width && height == m_height) return true;

    m_width = width;
    m_height = height;

    // バッファを解放してからリサイズ
    ReleaseBuffers();

    if (FAILED(m_swapChain->ResizeBuffers(
        m_frameCount,
        width, height,
        BACK_BUFFER_FORMAT,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH)))
    {
        OutputDebugStringW(L"[DX12] SwapChain::ResizeBuffers 失敗\n");
        return false;
    }

    return CreateRTVHeapAndViews();
}

//-----------------------------------------------------------------------------
// CreateRTVHeapAndViews  ―  内部ヘルパー
//-----------------------------------------------------------------------------
bool SwapChain::CreateRTVHeapAndViews()
{
    // ---- RTV DescriptorHeap 生成 --------------------------------------
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = m_frameCount;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    // RTV/DSV は SHADER_VISIBLE 不要（CPU からしかアクセスしない）

    if (FAILED(m_device->CreateDescriptorHeap(
        &heapDesc, IID_PPV_ARGS(&m_rtvHeap))))
    {
        OutputDebugStringW(L"[DX12] RTV DescriptorHeap 生成失敗\n");
        return false;
    }
    m_rtvHeap->SetName(L"SwapChainRTVHeap");

    // ---- バックバッファと RTV の紐付け --------------------------------
    m_backBuffers.resize(m_frameCount);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t i = 0; i < m_frameCount; ++i)
    {
        if (FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i]))))
        {
            OutputDebugStringW(L"[DX12] バックバッファ取得失敗\n");
            return false;
        }

        wchar_t name[64];
        swprintf_s(name, L"BackBuffer[%u]", i);
        m_backBuffers[i]->SetName(name);

        // RTV を生成してヒープに書き込む
        m_device->CreateRenderTargetView(
            m_backBuffers[i].Get(), nullptr, rtvHandle);

        // 次の RTV スロットへ進む
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    return true;
}

//-----------------------------------------------------------------------------
// ReleaseBuffers
//-----------------------------------------------------------------------------
void SwapChain::ReleaseBuffers()
{
    m_backBuffers.clear();
}