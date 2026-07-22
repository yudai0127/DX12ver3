#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;


//-----------------------------------------------------------------------------
// アダプター情報
//-----------------------------------------------------------------------------
struct AdapterInfo
{
    std::wstring  description;
    size_t        dedicatedVideoMemory = 0;
    DXGI_ADAPTER_DESC1 rawDesc = {};
};

//-----------------------------------------------------------------------------
// Device
//  - IDXGIFactory / IDXGIAdapter の列挙
//  - ID3D12Device の生成
//  - デバッグレイヤーの有効化（DEBUG ビルド時）
//-----------------------------------------------------------------------------
class Device
{
public:
    Device() = default;
    ~Device() = default;

    // コピー禁止
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    //-------------------------------------------------------------------
    // 初期化 / 解放
    //-------------------------------------------------------------------

    /// @brief デバイスを初期化する
    /// @param preferHighPerformance true = 最大VRAM アダプターを選択
    bool Initialize(bool preferHighPerformance = true);
    void Uninitialize();

    //-------------------------------------------------------------------
    // アクセサ
    //-------------------------------------------------------------------
    ID3D12Device* GetDevice()  const { return m_device.Get(); }
    IDXGIFactory6* GetFactory() const { return m_factory.Get(); }
    IDXGIAdapter1* GetAdapter() const { return m_adapter.Get(); }

    // ---- DXR (DirectX Raytracing) ------------------------------------
    // ID3D12Device5 is required for BuildRaytracingAccelerationStructure,
    // CreateStateObject, etc. Returns nullptr on devices without DXR.
    ID3D12Device5* GetDevice5() const { return m_device5.Get(); }

    // True when the adapter/driver reports RaytracingTier >= 1.0.
    bool SupportsRaytracing() const { return m_raytracingSupported; }

    const AdapterInfo& GetAdapterInfo() const { return m_adapterInfo; }

    /// @brief MSAA サポートレベルを確認する
    /// @return サポートされる最大サンプル数（4 or 1）
    UINT CheckMSAASupport(DXGI_FORMAT format, UINT sampleCount) const;

    bool IsValid() const { return m_device != nullptr; }

private:
    bool CreateFactory();
    bool SelectAdapter(bool preferHighPerformance);
    bool CreateDevice();
    void EnableDebugLayer();

    ComPtr<IDXGIFactory6>  m_factory;
    ComPtr<IDXGIAdapter1>  m_adapter;
    ComPtr<ID3D12Device>   m_device;
    ComPtr<ID3D12Device5>  m_device5;            // DXR interface (may be null)
    bool                   m_raytracingSupported = false;

    AdapterInfo m_adapterInfo;
};