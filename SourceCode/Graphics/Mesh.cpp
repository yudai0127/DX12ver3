#include "Graphics/Mesh.h"
#include "Graphics/ShaderManager.h"
#include "RHI/DeviceManager.h"
#include "RHI/PipelineBuilder.h"
#include "RHI/RootSignatureBuilder.h"
#include "RHI/GpuBuffer.h"
#include "Core/RenderStats.h"
#include <cassert>

using namespace DirectX;

//-----------------------------------------------------------------------------
// CreateCube  ―  サイズ1.0の立方体を生成（重心が原点）
//   授業 UNIT11: 8コントロールポイント × 法線3方向 = 24頂点、36インデックス
//-----------------------------------------------------------------------------
bool Mesh::CreateCube()
{
    auto* dm = DeviceManager::Instance();
    ID3D12Device* device = dm->GetDevice();

    const float s = 0.5f; // 中心から各面まで（サイズ1.0）

    // 6面ぶんの頂点（各面4頂点・法線は面ごと）= 24頂点
    std::vector<Vertex> vertices =
    {
        // +Y（上）
        {{-s,+s,-s},{0,1,0}}, {{+s,+s,-s},{0,1,0}}, {{+s,+s,+s},{0,1,0}}, {{-s,+s,+s},{0,1,0}},
        // -Y（下）
        {{-s,-s,+s},{0,-1,0}},{{+s,-s,+s},{0,-1,0}},{{+s,-s,-s},{0,-1,0}},{{-s,-s,-s},{0,-1,0}},
        // +X（右）
        {{+s,-s,-s},{1,0,0}}, {{+s,-s,+s},{1,0,0}}, {{+s,+s,+s},{1,0,0}}, {{+s,+s,-s},{1,0,0}},
        // -X（左）
        {{-s,-s,+s},{-1,0,0}},{{-s,-s,-s},{-1,0,0}},{{-s,+s,-s},{-1,0,0}},{{-s,+s,+s},{-1,0,0}},
        // +Z（奥）
        {{+s,-s,+s},{0,0,1}}, {{-s,-s,+s},{0,0,1}}, {{-s,+s,+s},{0,0,1}}, {{+s,+s,+s},{0,0,1}},
        // -Z（手前）
        {{-s,-s,-s},{0,0,-1}},{{+s,-s,-s},{0,0,-1}},{{+s,+s,-s},{0,0,-1}},{{-s,+s,-s},{0,0,-1}},
    };

    // 各面を2三角形（6インデックス）× 6面 = 36インデックス。時計回りが表。
    std::vector<uint32_t> indices;
    for (uint32_t face = 0; face < 6; ++face)
    {
        uint32_t b = face * 4;
        indices.push_back(b + 0); indices.push_back(b + 1); indices.push_back(b + 2);
        indices.push_back(b + 0); indices.push_back(b + 2); indices.push_back(b + 3);
    }

    if (!CreateBuffers(vertices, indices))            return false;
    if (!CreateRootSignature(device))                 return false;
    if (!CreatePipelineState(device, dm->GetRTVFormat())) return false;
    if (!m_objectCB.Initialize(sizeof(ObjectConstants))) return false;

    return true;
}

//-----------------------------------------------------------------------------
// CreateBuffers  ―  頂点バッファとインデックスバッファを作る
//-----------------------------------------------------------------------------
bool Mesh::CreateBuffers(const std::vector<Vertex>& vertices,
    const std::vector<uint32_t>& indices)
{
    auto* device = DeviceManager::Instance()->GetDevice();
    m_indexCount = static_cast<uint32_t>(indices.size());

    const UINT vbSize = static_cast<UINT>(sizeof(Vertex) * vertices.size());
    m_vertexBuffer = GpuBuffer::CreateUploadWithData(vertices.data(), vbSize);
    if (!m_vertexBuffer) return false;
    m_vbView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vbView.SizeInBytes = vbSize;
    m_vbView.StrideInBytes = sizeof(Vertex);

    const UINT ibSize = static_cast<UINT>(sizeof(uint32_t) * indices.size());
    m_indexBuffer = GpuBuffer::CreateUploadWithData(indices.data(), ibSize);
    if (!m_indexBuffer) return false;
    m_ibView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_ibView.SizeInBytes = ibSize;
    m_ibView.Format = DXGI_FORMAT_R32_UINT;

    return true;
}

//-----------------------------------------------------------------------------
// CreateRootSignature
//   定数バッファを2つ使う:
//     b0 … オブジェクト定数（world・色）
//     b1 … シーン定数（view_projection・light）
//   どちらも Root Descriptor（CBV）として直接バインドする（シンプル）。
//-----------------------------------------------------------------------------
bool Mesh::CreateRootSignature(ID3D12Device* device)
{
    // b0: オブジェクト定数、b1: シーン定数（どちらもVSで使う）
    RootSignatureBuilder builder;
    builder.AddCBV(0, D3D12_SHADER_VISIBILITY_VERTEX)
        .AddCBV(1, D3D12_SHADER_VISIBILITY_VERTEX);
    m_rootSignature = builder.Build(L"MeshRootSignature");
    return m_rootSignature != nullptr;
}

//-----------------------------------------------------------------------------
// CreatePipelineState
//   2D と違い、深度テストを有効にする（3Dの前後関係）。
//-----------------------------------------------------------------------------
bool Mesh::CreatePipelineState(ID3D12Device* device, DXGI_FORMAT rtvFormat)
{
    auto vs = ShaderManager::Instance()->LoadCSO("Mesh_VS.cso");
    auto ps = ShaderManager::Instance()->LoadCSO("Mesh_PS.cso");
    if (vs.empty() || ps.empty()) return false;

    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
          D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
          D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    PipelineBuilder builder;
    builder.SetRootSignature(m_rootSignature.Get())
        .SetVS(vs).SetPS(ps)
        .SetInputLayout(inputLayout, _countof(inputLayout))
        .SetDepth(true)                // 3D：深度テスト有効
        .SetCull(D3D12_CULL_MODE_BACK) // 裏面カリング
        .SetFrontCounterClockwise(false); // 時計回りが表

    m_pipelineState = builder.Build(rtvFormat);
    if (!m_pipelineState)
    {
        OutputDebugStringW(L"[Mesh] PSO 生成失敗\n");
        return false;
    }
    m_pipelineState->SetName(L"MeshPSO");
    return true;
}

//-----------------------------------------------------------------------------
// Render
//-----------------------------------------------------------------------------
void Mesh::Render(ID3D12GraphicsCommandList* commandList,
    const XMFLOAT4X4& world,
    const XMFLOAT4& color,
    D3D12_GPU_VIRTUAL_ADDRESS sceneCB)
{
    assert(commandList);

    // オブジェクト定数(b0)を書き込む
    ObjectConstants data = {};
    data.world = world;
    data.material_color = color;
    m_objectCB.Update(data);

    commandList->SetPipelineState(m_pipelineState.Get());
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    // b0: オブジェクト定数、b1: シーン定数
    commandList->SetGraphicsRootConstantBufferView(
        0, m_objectCB.GetGpuAddress());
    commandList->SetGraphicsRootConstantBufferView(1, sceneCB);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &m_vbView);
    commandList->IASetIndexBuffer(&m_ibView);
    commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);

    commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
    RenderStats::Instance().AddDrawCall(DrawCategory::Mesh, m_indexCount / 3);
}