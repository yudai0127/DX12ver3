#include "DepthBuffer.h"
#include <cassert>


//-----------------------------------------------------------------------------
// Initialize
//-----------------------------------------------------------------------------
bool DepthBuffer::Initialize(ID3D12Device* device,
    uint32_t width,
    uint32_t height)
{
    assert(device);
    m_device = device;

    // ---- DSV DescriptorHeap 生成 --------------------------------------
    // 深度バッファは1枚だけなので NumDescriptors = 1
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // DSV は SHADER_VISIBLE 不要
    heapDesc.NodeMask = 0;

    if (FAILED(m_device->CreateDescriptorHeap(
        &heapDesc, IID_PPV_ARGS(&m_dsvHeap))))
    {
        OutputDebugStringW(L"[DX12] DSV DescriptorHeap 生成失敗\n");
        return false;
    }
    m_dsvHeap->SetName(L"DepthStencilHeap");

    // ---- 深度バッファリソース + DSV 生成 ------------------------------
    return CreateBuffer(width, height);
}

//-----------------------------------------------------------------------------
// Uninitialize
//-----------------------------------------------------------------------------
void DepthBuffer::Uninitialize()
{
    m_depthBuffer = nullptr;
    m_dsvHeap = nullptr;
    m_device = nullptr;
}

//-----------------------------------------------------------------------------
// Resize
//-----------------------------------------------------------------------------
bool DepthBuffer::Resize(uint32_t width, uint32_t height)
{
    // 古いバッファを解放してから作り直す
    m_depthBuffer = nullptr;
    return CreateBuffer(width, height);
}

//-----------------------------------------------------------------------------
// CreateBuffer  ―  内部ヘルパー
//-----------------------------------------------------------------------------
bool DepthBuffer::CreateBuffer(uint32_t width, uint32_t height)
{
    // ---- テクスチャリソース記述子 -------------------------------------
    //  DX11:  D3D11_TEXTURE2D_DESC + device->CreateTexture2D()
    //  DX12:  D3D12_RESOURCE_DESC  + CreateCommittedResource()
    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Alignment = 0;
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DEPTH_FORMAT;
    depthDesc.SampleDesc.Count = 1;     // MSAA なし（DX11 授業の SampleDesc.Count=1 と同じ）
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    // ↑ DX12 では深度として使うリソースに明示的にフラグが必要（DX11 の BindFlags 相当）

    // ---- クリア値の最適化 --------------------------------------------
    // GPU がこの値でクリアされることを事前に知っておくと内部最適化が効く
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DEPTH_FORMAT;
    clearValue.DepthStencil.Depth = 1.0f;  // DX11 授業の ClearDepthStencilView の 1.0f と同じ
    clearValue.DepthStencil.Stencil = 0;

    // ---- ヒープ設定（GPU 専用メモリに配置）---------------------------
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    // DX11 の D3D11_USAGE_DEFAULT に相当。CPU からは書かない専用バッファ。

    // ---- リソース生成 ------------------------------------------------
    if (FAILED(m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, // 最初から深度書き込み状態で生成
        &clearValue,
        IID_PPV_ARGS(&m_depthBuffer))))
    {
        OutputDebugStringW(L"[DX12] 深度バッファリソース生成失敗\n");
        return false;
    }
    m_depthBuffer->SetName(L"DepthStencilBuffer");

    // ---- DSV（DepthStencilView）生成 ---------------------------------
    //  DX11: device->CreateDepthStencilView(buffer, &desc, &depth_stencil_view)
    //  DX12: device->CreateDepthStencilView(resource, &desc, heapHandle)
    //        → ビューはヒープの中に書き込まれる（ポインタではなくハンドル）
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DEPTH_FORMAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

    m_device->CreateDepthStencilView(
        m_depthBuffer.Get(),
        &dsvDesc,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    return true;
}