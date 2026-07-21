#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// DescriptorHeap
//  SRV/CBV/UAV を置くための「棚」を管理する汎用クラス。
//  「次に空いているスロット」を順番に払い出していく連番アロケータ。
//-----------------------------------------------------------------------------
class DescriptorHeap
{
public:
    // 1スロット分のハンドル（CPU用とGPU用のペア）
    struct Handle
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = {}; // ビュー生成時に使う
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = {}; // 描画時にシェーダーへ渡す
        uint32_t index = 0;
    };

    DescriptorHeap() = default;
    ~DescriptorHeap() { Uninitialize(); }

    DescriptorHeap(const DescriptorHeap&) = delete;
    DescriptorHeap& operator=(const DescriptorHeap&) = delete;

    /// @param capacity      置けるディスクリプタの最大数
    /// @param shaderVisible シェーダーから見えるヒープにするか
    bool Initialize(ID3D12Device* device,
        uint32_t      capacity,
        bool          shaderVisible = true);

    void Uninitialize();

    /// @brief 空きスロットを1つ確保してハンドルを返す
    Handle Allocate();

    ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
    bool IsValid() const { return m_heap != nullptr; }

private:
    ComPtr<ID3D12DescriptorHeap> m_heap;
    uint32_t m_capacity = 0;
    uint32_t m_allocatedCount = 0;
    uint32_t m_descriptorSize = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart = {};
};
