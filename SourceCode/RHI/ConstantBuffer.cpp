#include "RHI/ConstantBuffer.h"
#include "RHI/GpuBuffer.h"

// 定数バッファは256バイト境界に整列させる必要がある（DX12の制約）
static UINT Align256(UINT size) { return (size + 255) & ~255u; }

//-----------------------------------------------------------------------------
// Initialize  ―  256バイト整列の UPLOAD バッファを作り、Map して保持
//-----------------------------------------------------------------------------
bool ConstantBuffer::Initialize(size_t byteSize)
{
    const UINT size = Align256(static_cast<UINT>(byteSize));
    m_resource = GpuBuffer::CreateUploadMapped(size, &m_mapped);
    return m_resource != nullptr;
}