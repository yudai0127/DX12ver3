#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>

using Microsoft::WRL::ComPtr;


//-----------------------------------------------------------------------------
// SwapChain
//  - IDXGISwapChain の生成・管理
//  - バックバッファ (RenderTarget) リソースの保持
//  - RTV (RenderTargetView) DescriptorHeap の管理
//  - Present / リサイズ
//
//  DX12 ではスワップチェーンのバックバッファ数 = フレームバッファ数。
//  通常 2 (ダブル) か 3 (トリプル) を使う。
//  Present する前に必ず RENDER_TARGET → PRESENT バリアが必要。
//-----------------------------------------------------------------------------
class SwapChain
{
public:
    static constexpr DXGI_FORMAT BACK_BUFFER_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

    SwapChain() = default;
    ~SwapChain() { Uninitialize(); }

    SwapChain(const SwapChain&) = delete;
    SwapChain& operator=(const SwapChain&) = delete;

    //-------------------------------------------------------------------
    // 初期化 / 解放
    //-------------------------------------------------------------------
    bool Initialize(IDXGIFactory6* factory,
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        HWND                hwnd,
        uint32_t            width,
        uint32_t            height,
        uint32_t            frameCount = 2);

    void Uninitialize();

    //-------------------------------------------------------------------
    // フレーム操作
    //-------------------------------------------------------------------

    /// @brief 現在のバックバッファインデックスを返す
    uint32_t GetCurrentBackBufferIndex() const;

    /// @brief 現在フレームのバックバッファリソースを返す
    ID3D12Resource* GetCurrentBackBuffer() const;

    /// @brief 現在フレームの RTV ハンドルを返す
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRTV() const;

    /// @brief Present を実行する
    /// @param syncInterval 0=即時, 1=VSync
    void Present(uint32_t syncInterval = 1);

    //-------------------------------------------------------------------
    // リサイズ
    //  ウィンドウサイズ変更時に呼ぶ。
    //  事前に GPU アイドル状態 (全フレーム Fence 待機済み) にすること。
    //-------------------------------------------------------------------
    bool Resize(uint32_t width, uint32_t height);

    //-------------------------------------------------------------------
    // アクセサ
    //-------------------------------------------------------------------
    uint32_t GetWidth()      const { return m_width; }
    uint32_t GetHeight()     const { return m_height; }
    uint32_t GetFrameCount() const { return m_frameCount; }
    DXGI_FORMAT GetFormat()  const { return BACK_BUFFER_FORMAT; }

    bool IsValid() const { return m_swapChain != nullptr; }

private:
    bool CreateRTVHeapAndViews();
    void ReleaseBuffers();

    ComPtr<IDXGISwapChain4>           m_swapChain;
    ComPtr<ID3D12DescriptorHeap>      m_rtvHeap;
    std::vector<ComPtr<ID3D12Resource>> m_backBuffers;

    ID3D12Device* m_device = nullptr; // 非所有参照

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_frameCount = 2;
    uint32_t m_rtvDescriptorSize = 0;
};