#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <cstdint>
#include "RHI/ConstantBuffer.h"

using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// Mesh
//  3D メッシュ（頂点・インデックスを持つ形状）を描画するクラス。
//  授業資料 UNIT11 の geometric_primitive の DX12 版。
//
//  立方体などの基本形状をプログラムで生成して描画する。
//  将来 glTF/FBX ローダーを作るときも、このクラスに頂点・インデックスを
//  詰める形で再利用できる（描画の土台）。
//
//  3D描画の新要素:
//    ・インデックスバッファ … 頂点を使い回して三角形を作る
//    ・定数バッファ        … world行列・色をGPUに送る（b0）
//    ・深度テスト          … 前後関係を正しく描く
//-----------------------------------------------------------------------------
class Mesh
{
public:
    // 3D頂点（位置 + 法線）
    struct Vertex
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;
    };

    // オブジェクトごとの定数バッファ（b0）授業の constants に相当
    struct ObjectConstants
    {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4   material_color;
    };

    Mesh() = default;
    ~Mesh() = default;

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    /// @brief 立方体を生成する（サイズ1.0・重心が原点）
    bool CreateCube();

    bool IsValid() const { return m_pipelineState != nullptr; }

    /// @brief 描画する
    /// @param commandList コマンドリスト
    /// @param world       ワールド変換行列
    /// @param color       マテリアル色
    /// @param sceneCB     シーン定数バッファ(b1)のGPUアドレス
    void Render(ID3D12GraphicsCommandList* commandList,
        const DirectX::XMFLOAT4X4& world,
        const DirectX::XMFLOAT4& color,
        D3D12_GPU_VIRTUAL_ADDRESS  sceneCB);

private:
    bool CreateBuffers(const std::vector<Vertex>& vertices,
        const std::vector<uint32_t>& indices);
    bool CreateRootSignature(ID3D12Device* device);
    bool CreatePipelineState(ID3D12Device* device, DXGI_FORMAT rtvFormat);

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;

    ComPtr<ID3D12Resource>   m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vbView = {};
    ComPtr<ID3D12Resource>   m_indexBuffer;
    D3D12_INDEX_BUFFER_VIEW  m_ibView = {};
    uint32_t                 m_indexCount = 0;

    // オブジェクト定数バッファ(b0)
    ConstantBuffer m_objectCB;
};