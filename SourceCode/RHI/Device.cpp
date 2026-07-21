#include "Device.h"
#include <stdexcept>
#include <cassert>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

#ifdef _DEBUG
#include <d3d12sdklayers.h>
#endif


//-----------------------------------------------------------------------------
// Initialize
//-----------------------------------------------------------------------------
bool Device::Initialize(bool preferHighPerformance)
{
    EnableDebugLayer();

    if (!CreateFactory())        return false;
    if (!SelectAdapter(preferHighPerformance)) return false;
    if (!CreateDevice())         return false;

    return true;
}

//-----------------------------------------------------------------------------
// Uninitialize
//-----------------------------------------------------------------------------
void Device::Uninitialize()
{
    m_device = nullptr;
    m_adapter = nullptr;
    m_factory = nullptr;
}

//-----------------------------------------------------------------------------
// CheckMSAASupport
//-----------------------------------------------------------------------------
UINT Device::CheckMSAASupport(DXGI_FORMAT format, UINT sampleCount) const
{
    assert(m_device);

    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msaaData = {};
    msaaData.Format = format;
    msaaData.SampleCount = sampleCount;
    msaaData.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;

    if (FAILED(m_device->CheckFeatureSupport(
        D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msaaData, sizeof(msaaData))))
    {
        return 0;
    }
    return msaaData.NumQualityLevels;
}

//-----------------------------------------------------------------------------
// CreateFactory
//-----------------------------------------------------------------------------
bool Device::CreateFactory()
{
    UINT flags = 0;

#ifdef _DEBUG
    flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

    if (FAILED(CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_factory))))
    {
        OutputDebugStringW(L"[DX12] IDXGIFactory6 の生成に失敗しました\n");
        return false;
    }
    return true;
}

//-----------------------------------------------------------------------------
// SelectAdapter  ―  最大 VRAM のアダプターを選ぶ
//-----------------------------------------------------------------------------
bool Device::SelectAdapter(bool preferHighPerformance)
{
    ComPtr<IDXGIAdapter1> bestAdapter;
    size_t bestMemory = 0;

    // DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE で列挙するとiGPU を後回しにできる
    const DXGI_GPU_PREFERENCE gpuPref = preferHighPerformance
        ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
        : DXGI_GPU_PREFERENCE_UNSPECIFIED;

    for (UINT i = 0; ; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(m_factory->EnumAdapterByGpuPreference(
            i, gpuPref, IID_PPV_ARGS(&adapter))))
        {
            break; // 列挙終了
        }

        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);

        // ソフトウェアアダプター (WARP) はスキップ
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        // D3D12 をサポートしているか確認（実際には生成しない）
        if (FAILED(D3D12CreateDevice(adapter.Get(),
            D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr)))
        {
            continue;
        }

        if (desc.DedicatedVideoMemory > bestMemory)
        {
            bestMemory = desc.DedicatedVideoMemory;
            bestAdapter = adapter;
            m_adapterInfo.rawDesc = desc;
            m_adapterInfo.description = desc.Description;
            m_adapterInfo.dedicatedVideoMemory = desc.DedicatedVideoMemory;
        }
    }

    if (!bestAdapter)
    {
        OutputDebugStringW(L"[DX12] 対応アダプターが見つかりませんでした\n");
        return false;
    }

    m_adapter = bestAdapter;
    return true;
}

//-----------------------------------------------------------------------------
// CreateDevice
//-----------------------------------------------------------------------------
bool Device::CreateDevice()
{
    // Feature Level 12.0 を優先し、なければ 11.0 にフォールバック
    constexpr D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    for (auto fl : featureLevels)
    {
        if (SUCCEEDED(D3D12CreateDevice(
            m_adapter.Get(), fl, IID_PPV_ARGS(&m_device))))
        {
            wchar_t buf[256];
            swprintf_s(buf, L"[DX12] Device 生成完了 (FL %u_%u): %s\n",
                (UINT)fl >> 12, ((UINT)fl >> 8) & 0xF,
                m_adapterInfo.description.c_str());
            OutputDebugStringW(buf);
            return true;
        }
    }

    OutputDebugStringW(L"[DX12] D3D12CreateDevice に失敗しました\n");
    return false;
}

//-----------------------------------------------------------------------------
// EnableDebugLayer  ―  DEBUG ビルド時のみ有効化
//-----------------------------------------------------------------------------
void Device::EnableDebugLayer()
{
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        OutputDebugStringW(L"[DX12] デバッグレイヤー 有効化\n");

        // GPU ベースバリデーション（重いが詳細なエラーが出る）
        ComPtr<ID3D12Debug3> debug3;
        if (SUCCEEDED(debugController.As(&debug3)))
        {
            debug3->SetEnableGPUBasedValidation(TRUE);
        }
    }
#endif
}