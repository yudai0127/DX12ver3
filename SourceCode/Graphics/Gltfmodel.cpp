#include "Graphics/GltfModel.h"
#include "Graphics/ShaderManager.h"
#include "RHI/DeviceManager.h"
#include "RHI/PipelineBuilder.h"
#include "RHI/RootSignatureBuilder.h"
#include "RHI/GpuBuffer.h"
#include "Core/RenderStats.h"
#include <stack>
#include <functional>
#include <cassert>
#include <filesystem>


#define TINYGLTF_NO_EXTERNAL_IMAGE
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#define TINYGLTF_NO_INCLUDE_STB_IMAGE_WRITE
#include "tiny_gltf.h"

using namespace DirectX;


// tinygltf の画像ローダーを無効化するダミー（DX11版と同じ）
static bool NullLoadImageData(tinygltf::Image*, const int, std::string*,
    std::string*, int, int,
    const unsigned char*, int, void*)
{
    return true;
}

//-----------------------------------------------------------------------------
// コンストラクタ
//-----------------------------------------------------------------------------
GltfModel::GltfModel(const std::string& filename)
{
    auto* dm = DeviceManager::Instance();
    ID3D12Device* device = dm->GetDevice();

    // ---- tinygltf で読み込み（DX11版と同じ）------------------------
    tinygltf::TinyGLTF tinyGltf;
    tinyGltf.SetImageLoader(NullLoadImageData, nullptr);

    tinygltf::Model gltfModel;
    std::string error, warning;
    bool succeeded = false;
    if (filename.find(".glb") != std::string::npos)
        succeeded = tinyGltf.LoadBinaryFromFile(&gltfModel, &error, &warning, filename.c_str());
    else if (filename.find(".gltf") != std::string::npos)
        succeeded = tinyGltf.LoadASCIIFromFile(&gltfModel, &error, &warning, filename.c_str());

    if (!succeeded)
    {
        OutputDebugStringA(("[GltfModel] 読み込み失敗: " + filename + "\n").c_str());
        if (!error.empty()) OutputDebugStringA(error.c_str());
        return;
    }

    // シーン
    for (const auto& gltfScene : gltfModel.scenes)
    {
        Scene& scene = m_scenes.emplace_back();
        scene.name = gltfScene.name;
        scene.nodes = gltfScene.nodes;
    }
    m_defaultScene = gltfModel.defaultScene < 0 ? 0 : gltfModel.defaultScene;

    // モデルのあるフォルダを覚えておく（外部画像の相対パス解決用）
    {
        size_t slash = filename.find_last_of("/\\");
        m_directory = (slash != std::string::npos)
            ? filename.substr(0, slash + 1) : "";
    }

    // ---- マテリアル読み込み（PBR一式）-----------------------------
    for (const auto& gltfMat : gltfModel.materials)
    {
        MaterialData& mat = m_materials.emplace_back();
        const auto& pbr = gltfMat.pbrMetallicRoughness;

        // basecolor
        const auto& bcf = pbr.baseColorFactor;
        if (bcf.size() == 4)
            mat.basecolor_factor = { (float)bcf[0], (float)bcf[1], (float)bcf[2], (float)bcf[3] };
        mat.basecolor_texture = pbr.baseColorTexture.index;

        // metallic-roughness
        mat.metallic_factor = (float)pbr.metallicFactor;
        mat.roughness_factor = (float)pbr.roughnessFactor;
        mat.metallic_roughness_texture = pbr.metallicRoughnessTexture.index;

        // normal
        mat.normal_texture = gltfMat.normalTexture.index;
        mat.normal_scale = (float)gltfMat.normalTexture.scale;

        // occlusion
        mat.occlusion_texture = gltfMat.occlusionTexture.index;
        mat.occlusion_strength = (float)gltfMat.occlusionTexture.strength;

        // emissive
        mat.emissive_texture = gltfMat.emissiveTexture.index;
        const auto& ef = gltfMat.emissiveFactor;
        if (ef.size() == 3)
            mat.emissive_factor = { (float)ef[0], (float)ef[1], (float)ef[2] };
    }

    // ---- テクスチャ（どの画像を指すか）-----------------------------
    for (const auto& gltfTex : gltfModel.textures)
    {
        GltfTexture& tex = m_textures.emplace_back();
        tex.source = gltfTex.source;
    }

    // ---- 画像を読み込んでテクスチャ化 ------------------------------
    for (const auto& gltfImage : gltfModel.images)
    {
        auto texture = std::make_unique<Texture>();
        bool ok = false;

        if (gltfImage.bufferView > -1)
        {
            // glb 埋め込み画像：バイト列から読む
            const auto& bv = gltfModel.bufferViews.at(gltfImage.bufferView);
            const auto& buf = gltfModel.buffers.at(bv.buffer);
            const uint8_t* data = buf.data.data() + bv.byteOffset;
            ok = texture->LoadFromMemory(data, bv.byteLength);
        }
        else if (!gltfImage.uri.empty())
        {
            // 外部ファイル参照：モデルのフォルダ + uri
            std::string path = m_directory + gltfImage.uri;
            std::wstring wpath(path.begin(), path.end());
            ok = texture->Load(wpath.c_str());
        }

        if (!ok) OutputDebugStringA("[GltfModel] 画像読み込み失敗\n");
        m_images.push_back(std::move(texture));
    }

    // テクスチャが無いマテリアル用の白テクスチャ
    m_dummyWhite = std::make_unique<Texture>();
    m_dummyWhite->CreateDummy(0xFFFFFFFF);

    // ---- ノード読み込み（DX11版 fetchNodes 相当）------------------
    for (const auto& gltfNode : gltfModel.nodes)
    {
        Node& node = m_nodes.emplace_back();
        node.name = gltfNode.name;
        node.mesh = gltfNode.mesh;
        node.children = gltfNode.children;

        if (gltfNode.scale.size() == 3)
            node.scale = { (float)gltfNode.scale[0], (float)gltfNode.scale[1], (float)gltfNode.scale[2] };
        if (gltfNode.translation.size() == 3)
            node.translation = { (float)gltfNode.translation[0], (float)gltfNode.translation[1], (float)gltfNode.translation[2] };
        if (gltfNode.rotation.size() == 4)
            node.rotation = { (float)gltfNode.rotation[0], (float)gltfNode.rotation[1], (float)gltfNode.rotation[2], (float)gltfNode.rotation[3] };

        // matrix が直接指定されている場合は分解する
        if (gltfNode.matrix.size() == 16)
        {
            XMFLOAT4X4 m;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    m(r, c) = (float)gltfNode.matrix[4 * r + c];
            XMVECTOR S, R, T;
            XMMatrixDecompose(&S, &R, &T, XMLoadFloat4x4(&m));
            XMStoreFloat3(&node.scale, S);
            XMStoreFloat4(&node.rotation, R);
            XMStoreFloat3(&node.translation, T);
        }
    }
    CumulateTransforms();

    // ---- メッシュ読み込み（position/normal/index のみ）-------------
    for (const auto& gltfMesh : gltfModel.meshes)
    {
        Mesh& mesh = m_meshes.emplace_back();
        mesh.name = gltfMesh.name;

        for (const auto& gltfPrim : gltfMesh.primitives)
        {
            // POSITION がなければスキップ
            auto itPos = gltfPrim.attributes.find("POSITION");
            if (itPos == gltfPrim.attributes.end()) continue;

            Primitive& prim = mesh.primitives.emplace_back();
            prim.material = gltfPrim.material; // マテリアル番号を記録

            // 頂点数
            const auto& posAccessor = gltfModel.accessors.at(itPos->second);
            // Local AABB from the POSITION accessor (used to place the RT floor).
            if (posAccessor.minValues.size() == 3 && posAccessor.maxValues.size() == 3)
            {
                prim.aabbMin = { (float)posAccessor.minValues[0],
                                 (float)posAccessor.minValues[1],
                                 (float)posAccessor.minValues[2] };
                prim.aabbMax = { (float)posAccessor.maxValues[0],
                                 (float)posAccessor.maxValues[1],
                                 (float)posAccessor.maxValues[2] };
            }
            const size_t vertexCount = posAccessor.count;
            std::vector<Vertex> vertices(vertexCount);

            // 属性ごとにデータをコピー
            auto copyAttribute = [&](const char* attr, auto memberPtr)
                {
                    auto it = gltfPrim.attributes.find(attr);
                    if (it == gltfPrim.attributes.end()) return;
                    const auto& accessor = gltfModel.accessors.at(it->second);
                    const auto& bufferView = gltfModel.bufferViews.at(accessor.bufferView);
                    const unsigned char* data =
                        gltfModel.buffers.at(bufferView.buffer).data.data()
                        + bufferView.byteOffset + accessor.byteOffset;
                    const size_t stride = accessor.ByteStride(bufferView);
                    for (size_t i = 0; i < accessor.count; ++i)
                    {
                        memcpy(&(vertices[i].*memberPtr),
                            data + stride * i,
                            sizeof(vertices[i].*memberPtr));
                    }
                };
            copyAttribute("POSITION", &Vertex::position);
            copyAttribute("NORMAL", &Vertex::normal);
            copyAttribute("TEXCOORD_0", &Vertex::texcoord);
            copyAttribute("TANGENT", &Vertex::tangent);

            // tangent があるか記録（法線マップの可否に使う）
            prim.has_tangent =
                (gltfPrim.attributes.find("TANGENT") != gltfPrim.attributes.end()) ? 1 : 0;

            // インデックス（32bitに統一して格納）
            std::vector<uint32_t> indices;
            if (gltfPrim.indices > -1)
            {
                const auto& accessor = gltfModel.accessors.at(gltfPrim.indices);
                const auto& bufferView = gltfModel.bufferViews.at(accessor.bufferView);
                const unsigned char* data =
                    gltfModel.buffers.at(bufferView.buffer).data.data()
                    + bufferView.byteOffset + accessor.byteOffset;
                indices.resize(accessor.count);

                if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
                {
                    memcpy(indices.data(), data, accessor.count * sizeof(uint32_t));
                }
                else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                {
                    const uint16_t* src = reinterpret_cast<const uint16_t*>(data);
                    for (size_t i = 0; i < accessor.count; ++i) indices[i] = src[i];
                }
                else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
                {
                    const uint8_t* src = data;
                    for (size_t i = 0; i < accessor.count; ++i) indices[i] = src[i];
                }
            }
            prim.indexCount = static_cast<uint32_t>(indices.size());

            // ---- GPUバッファ生成（GpuBufferヘルパー）------------------
            const UINT vbSize = static_cast<UINT>(sizeof(Vertex) * vertices.size());
            prim.vertexBuffer = GpuBuffer::CreateUploadWithData(vertices.data(), vbSize);
            prim.vbView.BufferLocation = prim.vertexBuffer->GetGPUVirtualAddress();
            prim.vbView.SizeInBytes = vbSize;
            prim.vbView.StrideInBytes = sizeof(Vertex);

            const UINT ibSize = static_cast<UINT>(sizeof(uint32_t) * indices.size());
            prim.indexBuffer = GpuBuffer::CreateUploadWithData(indices.data(), ibSize);
            prim.ibView.BufferLocation = prim.indexBuffer->GetGPUVirtualAddress();
            prim.ibView.SizeInBytes = ibSize;
            prim.ibView.Format = DXGI_FORMAT_R32_UINT;

            // オブジェクト定数バッファ(b0)
            //   中身: world(64) + material(4) + has_tangent(4) + pad(8) = 80
            prim.objectCB.Initialize(sizeof(XMFLOAT4X4) + sizeof(int) * 4);
        }
    }

    // ---- RootSignature と PSO ---------------------------------------
    if (!CreateRootSignature(device)) return;
    if (!CreatePipelineState(device, dm->GetRTVFormat())) return;

    // ---- マテリアル配列の StructuredBuffer を作る ------------------
    if (!m_materials.empty())
    {
        const UINT stride = sizeof(MaterialData);
        const UINT size = stride * static_cast<UINT>(m_materials.size());

        // Map保持して書き込む（StructuredBuffer）
        m_materialBuffer = GpuBuffer::CreateUploadMapped(size, &m_mappedMaterials);
        if (m_materialBuffer)
            memcpy(m_mappedMaterials, m_materials.data(), size);
    }

    // ---- マテリアルバッファ + 全テクスチャを連続SRV配置（bindless）-
    BuildResourceTable();
}

//-----------------------------------------------------------------------------
// CumulateTransforms  ―  ノード階層をたどってワールド行列を計算（DX11版流用）
//-----------------------------------------------------------------------------
void GltfModel::CumulateTransforms()
{
    std::stack<XMFLOAT4X4> parents;
    std::function<void(int)> traverse = [&](int nodeIndex)
        {
            Node& node = m_nodes.at(nodeIndex);
            XMMATRIX S = XMMatrixScaling(node.scale.x, node.scale.y, node.scale.z);
            XMMATRIX R = XMMatrixRotationQuaternion(
                XMVectorSet(node.rotation.x, node.rotation.y, node.rotation.z, node.rotation.w));
            XMMATRIX T = XMMatrixTranslation(node.translation.x, node.translation.y, node.translation.z);
            XMStoreFloat4x4(&node.global_transform,
                S * R * T * XMLoadFloat4x4(&parents.top()));
            for (int child : node.children)
            {
                parents.push(node.global_transform);
                traverse(child);
                parents.pop();
            }
        };

    if (m_scenes.empty()) return;
    for (int nodeIndex : m_scenes.at(m_defaultScene).nodes)
    {
        XMFLOAT4X4 identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        parents.push(identity);
        traverse(nodeIndex);
        parents.pop();
    }
}

//-----------------------------------------------------------------------------
// CreateRootSignature  ―  Mesh と同じ（b0: オブジェクト, b1: シーン）
//-----------------------------------------------------------------------------
bool GltfModel::CreateRootSignature(ID3D12Device* device)
{
    // b0: オブジェクト定数、b1: シーン定数（VS/PS両方）
    // bindless: t0 マテリアル + t1? テクスチャ配列（PS）
    // s0: サンプラー
    RootSignatureBuilder builder;
    builder.AddCBV(0, D3D12_SHADER_VISIBILITY_ALL)
        .AddCBV(1, D3D12_SHADER_VISIBILITY_ALL)
        .AddBindlessTable(D3D12_SHADER_VISIBILITY_PIXEL)
        .AddStaticSampler(0);
    m_rootSignature = builder.Build(L"GltfModelRootSignature");
    return m_rootSignature != nullptr;
}

//-----------------------------------------------------------------------------
// CreatePipelineState
//-----------------------------------------------------------------------------
bool GltfModel::CreatePipelineState(ID3D12Device* device, DXGI_FORMAT rtvFormat)
{
    std::vector<char> vs, ps;
    if (m_hotReload)
    {
        // 実行時に .hlsl をコンパイル（ホットリロード）
        vs = ShaderManager::Instance()->CompileFromFile(
            L"HLSL/GltfModel_VS.hlsl", L"main", L"vs_6_2");
        ps = ShaderManager::Instance()->CompileFromFile(
            L"HLSL/GltfModel_PS.hlsl", L"main", L"ps_6_2");
    }
    else
    {
        // ビルド済み CSO を読む（通常）
        vs = ShaderManager::Instance()->LoadCSO("GltfModel_VS.cso");
        ps = ShaderManager::Instance()->LoadCSO("GltfModel_PS.cso");
    }
    if (vs.empty() || ps.empty()) return false;

    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,
          D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0,
          D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
          D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0,
          D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    PipelineBuilder builder;
    builder.SetRootSignature(m_rootSignature.Get())
        .SetVS(vs).SetPS(ps)
        .SetInputLayout(inputLayout, _countof(inputLayout))
        .SetDepth(true)                  // 3D：深度テスト有効
        .SetCull(D3D12_CULL_MODE_NONE)
        .SetFrontCounterClockwise(true); // glTFは反時計回りが表

    m_pipelineState = builder.Build(rtvFormat);
    if (!m_pipelineState)
    {
        OutputDebugStringW(L"[GltfModel] PSO 生成失敗\n");
        return false;
    }
    m_pipelineState->SetName(L"GltfModelPSO");
    return true;
}

//-----------------------------------------------------------------------------
// BuildResourceTable  ―  bindless 用の連続SRVヒープを作る
//   [0]      マテリアル配列の StructuredBuffer  → materials(t0)
//   [1..N]   全テクスチャ（texture順）          → material_textures[](t1?)
//-----------------------------------------------------------------------------
void GltfModel::BuildResourceTable()
{
    auto* device = DeviceManager::Instance()->GetDevice();

    const UINT textureCount = static_cast<UINT>(m_textures.size());
    const UINT descriptorCount = 1 + textureCount; // material + textures

    // モデル専用の SRV ヒープ（shader visible）
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = descriptorCount > 0 ? descriptorCount : 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvTable));

    const UINT inc = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvTable->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_srvTable->GetGPUDescriptorHandleForHeapStart();

    // [0] マテリアル StructuredBuffer の SRV
    m_materialSrvGpu = gpu;
    if (m_materialBuffer)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.Buffer.NumElements = static_cast<UINT>(m_materials.size());
        srv.Buffer.StructureByteStride = sizeof(MaterialData);
        device->CreateShaderResourceView(m_materialBuffer.Get(), &srv, cpu);
    }
    cpu.ptr += inc;
    gpu.ptr += inc;

    // [1..N] テクスチャの SRV（texture順 = シェーダーの material_textures[] と一致）
    m_textureSrvGpu = gpu;
    for (UINT i = 0; i < textureCount; ++i)
    {
        int imageIndex = m_textures[i].source;
        Texture* tex = (imageIndex > -1 && m_images.at(imageIndex)->IsValid())
            ? m_images.at(imageIndex).get()
            : m_dummyWhite.get();

        ID3D12Resource* res = tex->GetResource();
        D3D12_RESOURCE_DESC rd = res->GetDesc();

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = rd.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = rd.MipLevels;
        device->CreateShaderResourceView(res, &srv, cpu);

        cpu.ptr += inc;
    }
}

//-----------------------------------------------------------------------------
// ReloadShaders  ―  .hlsl を再コンパイルして PSO を作り直す（ホットリロード）
//-----------------------------------------------------------------------------
void GltfModel::ReloadShaders()
{
    auto* dm = DeviceManager::Instance();
    dm->GetCommand().GetFence().WaitForIdle(dm->GetCommand().GetQueue());

    m_hotReload = true;
    ComPtr<ID3D12PipelineState> oldPSO = m_pipelineState;
    m_pipelineState.Reset();

    if (!CreatePipelineState(dm->GetDevice(), dm->GetRTVFormat()))
    {
        OutputDebugStringW(L"[GltfModel] シェーダー再読み込み失敗。元に戻します\n");
        m_pipelineState = oldPSO;
    }
    else
    {
        OutputDebugStringW(L"[GltfModel] シェーダー再読み込み成功\n");
    }
    m_hotReload = false;

    // リロード後、現在のファイル更新時刻を記録（次回比較用）
    m_vsWriteTime = GetFileWriteTime(m_vsPath);
    m_psWriteTime = GetFileWriteTime(m_psPath);
}

//-----------------------------------------------------------------------------
// GetFileWriteTime  ―  ファイルの最終更新時刻を取得（数値化）
//-----------------------------------------------------------------------------
long long GltfModel::GetFileWriteTime(const std::wstring& path) const
{
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0; // ファイルが無い等
    return t.time_since_epoch().count();
}

//-----------------------------------------------------------------------------
// CheckAutoReload  ―  .hlsl の更新を検知して自動リロード（毎フレーム呼ぶ）
//   シェーダーを保存するだけで反映される。
//-----------------------------------------------------------------------------
void GltfModel::CheckAutoReload()
{
    long long vsTime = GetFileWriteTime(m_vsPath);
    long long psTime = GetFileWriteTime(m_psPath);

    // 初回（まだ記録が無い）は今の時刻を記録するだけ
    if (m_vsWriteTime == 0 && m_psWriteTime == 0)
    {
        m_vsWriteTime = vsTime;
        m_psWriteTime = psTime;
        return;
    }

    // どちらかが更新されていたらリロード
    if (vsTime != m_vsWriteTime || psTime != m_psWriteTime)
    {
        OutputDebugStringW(L"[GltfModel] ファイル更新を検知。自動リロード\n");
        ReloadShaders(); // この中で更新時刻も記録される
    }
}

//-----------------------------------------------------------------------------
// Render  ―  ノード階層をたどってメッシュを描画
//-----------------------------------------------------------------------------
void GltfModel::Render(ID3D12GraphicsCommandList* commandList,
    const XMFLOAT4X4& world,
    const XMFLOAT4& color,
    D3D12_GPU_VIRTUAL_ADDRESS sceneCB)
{
    if (!m_pipelineState) return;

    commandList->SetPipelineState(m_pipelineState.Get());
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootConstantBufferView(1, sceneCB); // b1: シーン

    // モデル専用の SRV ヒープ（マテリアル + 全テクスチャ）をセット
    if (m_srvTable)
    {
        ID3D12DescriptorHeap* heaps[] = { m_srvTable.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        // テーブル（t0: materials, t1?: textures）を先頭からバインド
        commandList->SetGraphicsRootDescriptorTable(2, m_materialSrvGpu);
    }

    // ノードをたどって描画
    std::function<void(int)> traverse = [&](int nodeIndex)
        {
            const Node& node = m_nodes.at(nodeIndex);
            if (node.mesh > -1)
            {
                Mesh& mesh = m_meshes.at(node.mesh);
                XMFLOAT4X4 worldMat;
                XMStoreFloat4x4(&worldMat,
                    XMLoadFloat4x4(&node.global_transform) * XMLoadFloat4x4(&world));

                for (Primitive& prim : mesh.primitives)
                {
                    // オブジェクト定数(b0): world + material番号 + has_tangent
                    struct CB
                    {
                        XMFLOAT4X4 world;
                        int material;
                        int has_tangent;
                        int pad[2];
                    } cb;
                    cb.world = worldMat;
                    cb.material = prim.material;
                    cb.has_tangent = prim.has_tangent;
                    prim.objectCB.Update(&cb, sizeof(cb));

                    commandList->SetGraphicsRootConstantBufferView(
                        0, prim.objectCB.GetGpuAddress());

                    commandList->IASetVertexBuffers(0, 1, &prim.vbView);
                    commandList->IASetIndexBuffer(&prim.ibView);
                    commandList->DrawIndexedInstanced(prim.indexCount, 1, 0, 0, 0);
                    RenderStats::Instance().AddDrawCall(DrawCategory::Model, prim.indexCount / 3);
                }
            }
            for (int child : node.children) traverse(child);
        };

    if (m_scenes.empty()) return;
    for (int nodeIndex : m_scenes.at(m_defaultScene).nodes)
        traverse(nodeIndex);
}

//-----------------------------------------------------------------------------
// GetTextureResourcesForRT  -  texture resources in bindless order for DXR.
//   Mirrors BuildResourceTable's selection: dummy white where an image is
//   missing, so the returned list has exactly one entry per glTF texture.
//-----------------------------------------------------------------------------
std::vector<ID3D12Resource*> GltfModel::GetTextureResourcesForRT() const
{
    std::vector<ID3D12Resource*> out;
    out.reserve(m_textures.size());
    for (const auto& t : m_textures)
    {
        const int imageIndex = t.source;
        const Texture* tex =
            (imageIndex > -1 && (size_t)imageIndex < m_images.size()
                && m_images[imageIndex] && m_images[imageIndex]->IsValid())
            ? m_images[imageIndex].get()
            : m_dummyWhite.get();
        out.push_back(tex ? tex->GetResource() : nullptr);
    }
    return out;
}