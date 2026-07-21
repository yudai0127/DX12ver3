#include "RHI/GpuBuffer.h"
#include "RHI/DeviceManager.h"

namespace GpuBuffer
{
    // バッファ共通の RESOURCE_DESC を作る
    static D3D12_RESOURCE_DESC MakeBufferDesc(UINT64 byteSize)
    {
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = byteSize;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return rd;
    }

    ComPtr<ID3D12Resource> CreateUpload(UINT64 byteSize)
    {
        auto* device = DeviceManager::Instance()->GetDevice();

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = MakeBufferDesc(byteSize);

        ComPtr<ID3D12Resource> res;
        if (FAILED(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&res))))
        {
            OutputDebugStringW(L"[GpuBuffer] UPLOAD バッファ生成失敗\n");
            return nullptr;
        }
        return res;
    }

    ComPtr<ID3D12Resource> CreateUploadMapped(UINT64 byteSize, void** outMapped)
    {
        ComPtr<ID3D12Resource> res = CreateUpload(byteSize);
        if (res && outMapped)
        {
            D3D12_RANGE readRange = { 0, 0 }; // CPUからは読まない
            res->Map(0, &readRange, outMapped);
        }
        return res;
    }

    ComPtr<ID3D12Resource> CreateUploadWithData(const void* data, UINT64 byteSize)
    {
        void* mapped = nullptr;
        ComPtr<ID3D12Resource> res = CreateUploadMapped(byteSize, &mapped);
        if (res && mapped && data)
        {
            memcpy(mapped, data, byteSize);
            res->Unmap(0, nullptr); // コピー後は解除（変化しないデータ用）
        }
        return res;
    }

    ComPtr<ID3D12Resource> CreateDefault(
        UINT64 byteSize, D3D12_RESOURCE_STATES initialState)
    {
        auto* device = DeviceManager::Instance()->GetDevice();

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = MakeBufferDesc(byteSize);

        ComPtr<ID3D12Resource> res;
        if (FAILED(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            initialState, nullptr,
            IID_PPV_ARGS(&res))))
        {
            OutputDebugStringW(L"[GpuBuffer] DEFAULT バッファ生成失敗\n");
            return nullptr;
        }
        return res;
    }
}