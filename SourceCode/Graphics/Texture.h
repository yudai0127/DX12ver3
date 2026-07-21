#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include "RHI/DescriptorHeap.h"
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// Texture
//  画像ファイル（DDS / PNG / JPG など）を読み込んで GPU テクスチャにする。
//  device/command/srvHeap は DeviceManager から取得するので引数不要。
//-----------------------------------------------------------------------------
class Texture
{
public:
    Texture() = default;
    ~Texture() = default;

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    /// @brief 画像ファイルを読み込んで GPU テクスチャ + SRV を作る
    /// @param filename 画像ファイルパス（.dds / .png / .jpg など）
    bool Load(const wchar_t* filename);

    /// @brief メモリ上の画像バイト列（PNG/JPGなど）から読み込む
    ///        glTF/glb に埋め込まれた画像用。WIC でデコードする。
    /// @param data 画像ファイルのバイト列
    /// @param size バイト数
    bool LoadFromMemory(const uint8_t* data, size_t size);

    /// @brief 単色のダミーテクスチャを作る（テクスチャが無いマテリアル用）
    /// @param rgba 0xAABBGGRR 形式の色（例: 白 = 0xFFFFFFFF）
    bool CreateDummy(uint32_t rgba);

    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle() const { return m_srv.gpu; }
    ID3D12Resource* GetResource() const { return m_texture.Get(); } // bindless配置用
    uint32_t GetWidth()  const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    bool     IsValid()   const { return m_texture != nullptr; }

private:
    bool FinalizeUpload(std::vector<D3D12_SUBRESOURCE_DATA>& subresources);

    ComPtr<ID3D12Resource> m_texture;
    ComPtr<ID3D12Resource> m_uploadBuffer;
    DescriptorHeap::Handle m_srv;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
};