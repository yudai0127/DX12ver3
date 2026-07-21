#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstring>
#include <cstdint>
#include <utility>

using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// ConstantBuffer
//  DX12 の定数バッファを扱う共通クラス。
//  各クラスで重複していた「UPLOADバッファ作成 → Map保持 → memcpy更新」を
//  1つにまとめたもの。DX11版の GPUConstantBuffer に相当。
//
//  DX12 の作法:
//    ・定数バッファは 256 バイト境界に整列する必要がある
//    ・UPLOAD ヒープに作り、Map したアドレスへ毎フレーム書き込む
//    ・GPU へは GetGpuAddress() を SetGraphicsRootConstantBufferView に渡す
//
//  使い方:
//    ConstantBuffer cb;
//    cb.Initialize(sizeof(MyData));      // 生成
//    cb.Update(&myData, sizeof(myData)); // 更新（毎フレーム）
//    commandList->SetGraphicsRootConstantBufferView(0, cb.GetGpuAddress());
//-----------------------------------------------------------------------------
class ConstantBuffer
{
public:
    ConstantBuffer() = default;
    ~ConstantBuffer() = default;

    // コピーは禁止（GPUリソースの二重管理を防ぐ）
    ConstantBuffer(const ConstantBuffer&) = delete;
    ConstantBuffer& operator=(const ConstantBuffer&) = delete;

    // ムーブは許可（vector に入れるため）
    ConstantBuffer(ConstantBuffer&& other) noexcept
    {
        m_resource = std::move(other.m_resource);
        m_mapped = other.m_mapped;
        other.m_mapped = nullptr;
    }
    ConstantBuffer& operator=(ConstantBuffer&& other) noexcept
    {
        if (this != &other)
        {
            m_resource = std::move(other.m_resource);
            m_mapped = other.m_mapped;
            other.m_mapped = nullptr;
        }
        return *this;
    }

    /// @brief 定数バッファを生成する（256バイト整列・UPLOAD・Map保持）
    /// @param byteSize 中身のサイズ（sizeof(構造体)）
    bool Initialize(size_t byteSize);

    /// @brief 中身を書き込む（毎フレーム呼んでよい・高速）
    void Update(const void* data, size_t byteSize)
    {
        if (m_mapped) memcpy(m_mapped, data, byteSize);
    }

    /// @brief 型から直接書き込むテンプレート版
    template <typename T>
    void Update(const T& data) { Update(&data, sizeof(T)); }

    /// @brief GPU 仮想アドレス（SetGraphicsRootConstantBufferView に渡す）
    D3D12_GPU_VIRTUAL_ADDRESS GetGpuAddress() const
    {
        return m_resource ? m_resource->GetGPUVirtualAddress() : 0;
    }

    bool IsValid() const { return m_resource != nullptr; }

private:
    ComPtr<ID3D12Resource> m_resource;
    void* m_mapped = nullptr;
};