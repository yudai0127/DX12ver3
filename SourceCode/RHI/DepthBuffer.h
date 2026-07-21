#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

using Microsoft::WRL::ComPtr;


//-----------------------------------------------------------------------------
// DepthBuffer
//  深度ステンシルバッファ + DSV (DepthStencilView) を管理するクラス。
//
//  DX11 との対応:
//    DX11: device->CreateTexture2D()          → DX12: CreateCommittedResource()
//    DX11: device->CreateDepthStencilView()   → DX12: CreateDepthStencilView()
//    DX11: depth_stencil_view                 → DX12: DSV DescriptorHeap + Handle
//
//  DX12 では DSV も DescriptorHeap に入れて CPU ハンドルで参照する。
//  RTV と同じく SHADER_VISIBLE は不要（CPU からしかセットしない）。
//-----------------------------------------------------------------------------
class DepthBuffer
{
public:
    // DX12 標準の深度フォーマット（DX11 の DXGI_FORMAT_D24_UNORM_S8_UINT に相当）
    static constexpr DXGI_FORMAT DEPTH_FORMAT = DXGI_FORMAT_D32_FLOAT;

    DepthBuffer() = default;
    ~DepthBuffer() { Uninitialize(); }

    DepthBuffer(const DepthBuffer&) = delete;
    DepthBuffer& operator=(const DepthBuffer&) = delete;

    //-------------------------------------------------------------------
    // 初期化 / 解放
    //-------------------------------------------------------------------

    /// @brief 深度バッファを生成する
    /// @param width  バックバッファと同じ幅
    /// @param height バックバッファと同じ高さ
    bool Initialize(ID3D12Device* device,
        uint32_t width,
        uint32_t height);

    void Uninitialize();

    //-------------------------------------------------------------------
    // リサイズ（ウィンドウサイズ変更時）
    //-------------------------------------------------------------------
    bool Resize(uint32_t width, uint32_t height);

    //-------------------------------------------------------------------
    // アクセサ
    //-------------------------------------------------------------------

    /// @brief DSV の CPU ハンドルを返す（OMSetRenderTargets に渡す）
    D3D12_CPU_DESCRIPTOR_HANDLE GetDSV() const
    {
        return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    ID3D12Resource* GetResource() const { return m_depthBuffer.Get(); }
    DXGI_FORMAT     GetFormat()   const { return DEPTH_FORMAT; }
    bool            IsValid()     const { return m_depthBuffer != nullptr; }

private:
    bool CreateBuffer(uint32_t width, uint32_t height);

    ComPtr<ID3D12Resource>       m_depthBuffer;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;

    ID3D12Device* m_device = nullptr; // 非所有参照
};