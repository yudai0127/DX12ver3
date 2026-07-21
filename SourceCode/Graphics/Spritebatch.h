#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <cstdint>
#include "Graphics/Texture.h"

using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// SpriteBatch
//  授業資料 UNIT09 のスプライトバッチングの DX12 版。
//
//  通常の Sprite は「1枚描くごとに1ドローコール」だが、
//  SpriteBatch は同じテクスチャのスプライトを大量に描くとき、
//  全頂点を1つのバッファにまとめて「1回のドローコール」で描画する。
//  これにより大量描画（パーティクル・タイルマップ等）が高速になる。
//
//  使い方:
//    batch->Begin();
//    for (...) batch->Render(commandList, dx,dy,dw,dh, r,g,b,a, angle, sx,sy,sw,sh);
//    batch->End(commandList);   // ここで初めてまとめて描画
//
//  Sprite との違い:
//    Sprite     … 4頂点トライアングルストリップ、1枚ずつ Draw
//    SpriteBatch … 6頂点トライアングルリスト（矩形=三角形2枚）を溜めて一括 Draw
//-----------------------------------------------------------------------------
class SpriteBatch
{
public:
    struct Vertex
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT4 color;
        DirectX::XMFLOAT2 texcoord;
    };

    /// @param textureFile 貼り付ける画像ファイル
    /// @param maxSprites  一度に描ける最大スプライト数
    SpriteBatch(const wchar_t* textureFile, size_t maxSprites);
    ~SpriteBatch() = default;

    SpriteBatch(const SpriteBatch&) = delete;
    SpriteBatch& operator=(const SpriteBatch&) = delete;

    bool IsValid() const { return m_pipelineState != nullptr; }

    /// @brief バッチ開始（溜めていた頂点をクリア）
    void Begin();

    /// @brief スプライトを1枚分の頂点として溜める（まだ描画しない）
    void Render(float dx, float dy,
        float dw, float dh,
        float r, float g, float b, float a,
        float angle = 0.0f,
        float sx = 0, float sy = 0, float sw = 0, float sh = 0);

    /// @brief 溜めた全頂点をまとめて1回で描画する
    void End(ID3D12GraphicsCommandList* commandList);

private:
    bool CreateRootSignature(ID3D12Device* device);
    bool CreatePipelineState(ID3D12Device* device, DXGI_FORMAT rtvFormat);
    bool CreateVertexBuffer(ID3D12Device* device);

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;
    ComPtr<ID3D12Resource>      m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW    m_vertexBufferView = {};

    Texture m_texture;

    Vertex* m_mappedVertices = nullptr;  // Map 保持した書き込み先
    std::vector<Vertex> m_vertices;      // CPU 側で溜める頂点
    size_t  m_maxVertices = 0;           // 最大頂点数（maxSprites * 6）

    float m_screenWidth = 1280.0f;
    float m_screenHeight = 720.0f;
};
