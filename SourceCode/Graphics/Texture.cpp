#include "Texture.h"
#include "RHI/DeviceManager.h"
#include "RHI/CommandContext.h"
#include "RHI/Fence.h"

// DirectXTK12 のローダー（TK12 の Inc がインクルードパスに入っている前提）
#include "DDSTextureLoader.h"
#include "WICTextureLoader.h"

#include <vector>
#include <string>
#include <memory>
#include <map>
#include <cassert>

//-----------------------------------------------------------------------------
// テクスチャキャッシュ（授業 UNIT10 手順1）
//   同じファイルを2回読み込まないように、読み込み済みの情報を記憶しておく。
//   2回目以降は読み込まずにキャッシュから返す（高速）。
//-----------------------------------------------------------------------------
struct TextureCacheEntry
{
    ComPtr<ID3D12Resource> texture;
    DescriptorHeap::Handle srv;
    uint32_t width = 0;
    uint32_t height = 0;
};
static std::map<std::wstring, TextureCacheEntry> s_textureCache;

// プログラム終了時にキャッシュを解放する（任意で呼ぶ）
void ReleaseAllTextures()
{
    s_textureCache.clear();
}

//-----------------------------------------------------------------------------
// 拡張子が .dds かどうかを判定する小さなヘルパー
//-----------------------------------------------------------------------------
static bool IsDDS(const wchar_t* filename)
{
    std::wstring path = filename;
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    for (auto& c : ext) c = towlower(c);
    return ext == L".dds";
}

//-----------------------------------------------------------------------------
// GetRequiredIntermediateSize の自前実装
//   テクスチャを GPU に送るのに必要なアップロードバッファのサイズを計算する。
//   （d3dx12.h を使わずに済ませるため自前で書く）
//-----------------------------------------------------------------------------
static UINT64 CalcUploadSize(ID3D12Device* device,
    ID3D12Resource* texture,
    UINT numSubresources)
{
    D3D12_RESOURCE_DESC desc = texture->GetDesc();
    UINT64 requiredSize = 0;
    device->GetCopyableFootprints(
        &desc, 0, numSubresources, 0,
        nullptr, nullptr, nullptr, &requiredSize);
    return requiredSize;
}

//-----------------------------------------------------------------------------
// サブリソースを1枚ずつアップロードバッファ経由でテクスチャへコピーする
//   （d3dx12.h の UpdateSubresources 相当を自前実装）
//-----------------------------------------------------------------------------
static void UploadTexture(ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* dstTexture,
    ID3D12Resource* uploadBuffer,
    const std::vector<D3D12_SUBRESOURCE_DATA>& subresources)
{
    const UINT num = static_cast<UINT>(subresources.size());

    // 各サブリソースの配置情報（フットプリント）を取得
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(num);
    std::vector<UINT>   numRows(num);
    std::vector<UINT64> rowSizes(num);
    UINT64 totalBytes = 0;

    D3D12_RESOURCE_DESC desc = dstTexture->GetDesc();
    device->GetCopyableFootprints(
        &desc, 0, num, 0,
        layouts.data(), numRows.data(), rowSizes.data(), &totalBytes);

    // アップロードバッファを Map して、行ごとに画素データをコピー
    uint8_t* mapped = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    uploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped));

    for (UINT i = 0; i < num; ++i)
    {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout = layouts[i];
        const D3D12_SUBRESOURCE_DATA& src = subresources[i];

        uint8_t* dstBase = mapped + layout.Offset;
        const uint8_t* srcBase = static_cast<const uint8_t*>(src.pData);

        // 行ごとにコピー（行のピッチが GPU 側と CPU 側で異なるため）
        for (UINT y = 0; y < numRows[i]; ++y)
        {
            memcpy(dstBase + layout.Footprint.RowPitch * y,
                srcBase + src.RowPitch * y,
                static_cast<size_t>(rowSizes[i]));
        }
    }
    uploadBuffer->Unmap(0, nullptr);

    // バッファ → テクスチャへコピーするコマンドを積む
    for (UINT i = 0; i < num; ++i)
    {
        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource = dstTexture;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = i;

        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = uploadBuffer;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = layouts[i];

        cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    }
}

//-----------------------------------------------------------------------------
// Load
//-----------------------------------------------------------------------------
bool Texture::Load(const wchar_t* filename)
{
    // ---- キャッシュ確認（授業 UNIT10 手順1）------------------------
    //   同じファイルを既に読み込んでいたら、それを使い回す。
    auto it = s_textureCache.find(filename);
    if (it != s_textureCache.end())
    {
        const TextureCacheEntry& e = it->second;
        m_texture = e.texture;  // テクスチャ本体を共有
        m_srv = e.srv;      // SRV ハンドルも共有
        m_width = e.width;
        m_height = e.height;
        return true;            // 読み込みをスキップ
    }

    // DeviceManager から必要なものを取得
    auto* dm = DeviceManager::Instance();
    ID3D12Device* device = dm->GetDevice();
    CommandContext& command = dm->GetCommand();
    DescriptorHeap& srvHeap = dm->GetSrvHeap();

    assert(device);

    // ---- 1. 画像を読み込む（拡張子で DDS / WIC を切り替え）-----------
    std::unique_ptr<uint8_t[]>          imageData;
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;
    HRESULT hr = S_OK;

    if (IsDDS(filename))
    {
        hr = DirectX::LoadDDSTextureFromFile(
            device, filename,
            m_texture.ReleaseAndGetAddressOf(),
            imageData, subresources);
    }
    else
    {
        std::unique_ptr<uint8_t[]> wicData;
        D3D12_SUBRESOURCE_DATA      wicSubresource = {};
        hr = DirectX::LoadWICTextureFromFile(
            device, filename,
            m_texture.ReleaseAndGetAddressOf(),
            wicData, wicSubresource);
        if (SUCCEEDED(hr))
        {
            imageData = std::move(wicData);
            subresources.push_back(wicSubresource);
        }
    }

    if (FAILED(hr) || !m_texture)
    {
        wchar_t buf[256];
        swprintf_s(buf, L"[Texture] 読み込み失敗: %s\n", filename);
        OutputDebugStringW(buf);
        return false;
    }

    D3D12_RESOURCE_DESC texDesc = m_texture->GetDesc();
    m_width = static_cast<uint32_t>(texDesc.Width);
    m_height = texDesc.Height;

    // ---- 2. アップロード用バッファを作る ----------------------------
    const UINT64 uploadSize = CalcUploadSize(
        device, m_texture.Get(), static_cast<UINT>(subresources.size()));

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = uploadSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_uploadBuffer))))
    {
        OutputDebugStringW(L"[Texture] アップロードバッファ生成失敗\n");
        return false;
    }

    // ---- 3. 転送専用コマンドリストで CPU→GPU コピー -----------------
    ComPtr<ID3D12CommandAllocator>    allocator;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(), nullptr, IID_PPV_ARGS(&cmdList));

    UploadTexture(device, cmdList.Get(),
        m_texture.Get(), m_uploadBuffer.Get(), subresources);

    // テクスチャを「シェーダーから読める状態」へ遷移
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    cmdList->Close();

    // ---- 4. 実行して GPU の転送完了を待つ ---------------------------
    ID3D12CommandList* lists[] = { cmdList.Get() };
    command.GetQueue()->ExecuteCommandLists(1, lists);
    command.GetFence().WaitForIdle(command.GetQueue());

    // ---- 5. SRV を作る ----------------------------------------------
    m_srv = srvHeap.Allocate();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = texDesc.MipLevels;

    device->CreateShaderResourceView(m_texture.Get(), &srvDesc, m_srv.cpu);

    // ---- キャッシュに保存（次回以降は読み込まずに使い回す）---------
    TextureCacheEntry entry;
    entry.texture = m_texture;
    entry.srv = m_srv;
    entry.width = m_width;
    entry.height = m_height;
    s_textureCache[filename] = entry;

    return true;
}

//-----------------------------------------------------------------------------
// 内部共通処理：subresources を GPU テクスチャへ転送し SRV を作る
//   m_texture は作成済みの前提。Load 後半と同じ処理を共通化。
//-----------------------------------------------------------------------------
bool Texture::FinalizeUpload(std::vector<D3D12_SUBRESOURCE_DATA>& subresources)
{
    auto* dm = DeviceManager::Instance();
    ID3D12Device* device = dm->GetDevice();
    CommandContext& command = dm->GetCommand();
    DescriptorHeap& srvHeap = dm->GetSrvHeap();

    D3D12_RESOURCE_DESC texDesc = m_texture->GetDesc();
    m_width = static_cast<uint32_t>(texDesc.Width);
    m_height = texDesc.Height;

    // アップロードバッファ
    const UINT64 uploadSize = CalcUploadSize(
        device, m_texture.Get(), static_cast<UINT>(subresources.size()));

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = uploadSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_uploadBuffer))))
        return false;

    ComPtr<ID3D12CommandAllocator>    allocator;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(), nullptr, IID_PPV_ARGS(&cmdList));

    UploadTexture(device, cmdList.Get(),
        m_texture.Get(), m_uploadBuffer.Get(), subresources);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
    cmdList->Close();

    ID3D12CommandList* lists[] = { cmdList.Get() };
    command.GetQueue()->ExecuteCommandLists(1, lists);
    command.GetFence().WaitForIdle(command.GetQueue());

    m_srv = srvHeap.Allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = texDesc.MipLevels;
    device->CreateShaderResourceView(m_texture.Get(), &srvDesc, m_srv.cpu);

    return true;
}

//-----------------------------------------------------------------------------
// LoadFromMemory  ―  メモリ上の画像バイト列（PNG/JPG等）から読み込む
//   glTF/glb に埋め込まれた画像用。DirectXTK12 の WIC ローダーを使う。
//-----------------------------------------------------------------------------
bool Texture::LoadFromMemory(const uint8_t* data, size_t size)
{
    auto* device = DeviceManager::Instance()->GetDevice();

    std::unique_ptr<uint8_t[]> wicData;
    D3D12_SUBRESOURCE_DATA     wicSubresource = {};
    HRESULT hr = DirectX::LoadWICTextureFromMemory(
        device, data, size,
        m_texture.ReleaseAndGetAddressOf(),
        wicData, wicSubresource);

    if (FAILED(hr) || !m_texture)
    {
        OutputDebugStringW(L"[Texture] メモリからの読み込み失敗\n");
        return false;
    }

    std::vector<D3D12_SUBRESOURCE_DATA> subresources = { wicSubresource };
    return FinalizeUpload(subresources);
}

//-----------------------------------------------------------------------------
// CreateDummy  ―  単色1x1テクスチャ（テクスチャが無いマテリアル用）
//-----------------------------------------------------------------------------
bool Texture::CreateDummy(uint32_t rgba)
{
    auto* device = DeviceManager::Instance()->GetDevice();

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;

    if (FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_texture))))
        return false;

    // 1ピクセルぶんの色データを転送
    D3D12_SUBRESOURCE_DATA sub = {};
    sub.pData = &rgba;
    sub.RowPitch = 4;
    sub.SlicePitch = 4;
    std::vector<D3D12_SUBRESOURCE_DATA> subresources = { sub };
    return FinalizeUpload(subresources);
}