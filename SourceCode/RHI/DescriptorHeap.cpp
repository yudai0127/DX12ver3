#include "DescriptorHeap.h"
#include <cassert>

bool DescriptorHeap::Initialize(ID3D12Device* device,
    uint32_t      capacity,
    bool          shaderVisible)
{
    assert(device);
    m_capacity = capacity;
    m_allocatedCount = 0;

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = capacity;
    desc.Flags = shaderVisible
        ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
        : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap))))
    {
        OutputDebugStringW(L"[DescriptorHeap] ¶¬Ž¸”s\n");
        return false;
    }
    m_heap->SetName(L"SrvDescriptorHeap");

    m_descriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
    if (shaderVisible)
        m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();

    return true;
}

void DescriptorHeap::Uninitialize()
{
    m_heap = nullptr;
    m_allocatedCount = 0;
}

DescriptorHeap::Handle DescriptorHeap::Allocate()
{
    assert(m_allocatedCount < m_capacity && "DescriptorHeap ‚Ì‹ó‚«‚ª‚ ‚è‚Ü‚¹‚ñ");

    const uint32_t index = m_allocatedCount++;

    Handle handle;
    handle.index = index;
    handle.cpu.ptr = m_cpuStart.ptr +
        static_cast<SIZE_T>(index) * m_descriptorSize;
    handle.gpu.ptr = m_gpuStart.ptr +
        static_cast<UINT64>(index) * m_descriptorSize;

    return handle;
}