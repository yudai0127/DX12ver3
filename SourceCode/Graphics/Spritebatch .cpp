#include "Graphics/SpriteBatch.h"
#include "Graphics/ShaderManager.h"
#include "RHI/DeviceManager.h"
#include "RHI/PipelineBuilder.h"
#include "RHI/RootSignatureBuilder.h"
#include "RHI/GpuBuffer.h"
#include "Core/RenderStats.h"
#include <cassert>
#include <cmath>

using namespace DirectX;

//-----------------------------------------------------------------------------
// コンストラクタ
//-----------------------------------------------------------------------------
SpriteBatch::SpriteBatch(const wchar_t* textureFile, size_t maxSprites)
    : m_maxVertices(maxSprites * 6) // 矩形1枚 = 三角形2枚 = 6頂点
{
    auto* dm = DeviceManager::Instance();
    ID3D12Device* device = dm->GetDevice();
    m_screenWidth = dm->GetScreenWidth();
    m_screenHeight = dm->GetScreenHeight();

    m_vertices.reserve(m_maxVertices);

    if (!m_texture.Load(textureFile))
    {
        OutputDebugStringW(L"[SpriteBatch] テクスチャ読み込み失敗\n");
        return;
    }

    if (!CreateRootSignature(device))                     return;
    if (!CreatePipelineState(device, dm->GetRTVFormat())) return;
    if (!CreateVertexBuffer(device))                      return;
}

//-----------------------------------------------------------------------------
// CreateRootSignature  ―  Sprite と同じ（SRV t0 + StaticSampler s0）
//-----------------------------------------------------------------------------
bool SpriteBatch::CreateRootSignature(ID3D12Device* device)
{
    // t0: テクスチャ（PS）、s0: ポイントサンプラー
    RootSignatureBuilder builder;
    builder.AddSRVTable(0)
        .AddStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT);
    m_rootSignature = builder.Build(L"SpriteBatchRootSignature");
    return m_rootSignature != nullptr;
}

//-----------------------------------------------------------------------------
// CreatePipelineState  ―  Sprite とほぼ同じだがトポロジが TRIANGLE LIST
//-----------------------------------------------------------------------------
bool SpriteBatch::CreatePipelineState(ID3D12Device* device, DXGI_FORMAT rtvFormat)
{
    // Sprite と同じシェーダーを流用する
    std::vector<char> vs = ShaderManager::Instance()->LoadCSO("Sprite_VS.cso");
    std::vector<char> ps = ShaderManager::Instance()->LoadCSO("Sprite_PS.cso");
    if (vs.empty() || ps.empty()) return false;

    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,
          D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
          D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0,
          D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    PipelineBuilder builder;
    builder.SetRootSignature(m_rootSignature.Get())
        .SetVS(vs).SetPS(ps)
        .SetInputLayout(inputLayout, _countof(inputLayout))
        .SetDepth(false)               // 2D：深度テスト無効
        .SetCull(D3D12_CULL_MODE_NONE)
        .SetAlphaBlend(true);

    m_pipelineState = builder.Build(rtvFormat);
    if (!m_pipelineState)
    {
        OutputDebugStringW(L"[SpriteBatch] PSO 生成失敗\n");
        return false;
    }
    m_pipelineState->SetName(L"SpriteBatchPSO");
    return true;
}

//-----------------------------------------------------------------------------
// CreateVertexBuffer  ―  最大頂点数ぶんの大きな箱を確保し Map 保持
//-----------------------------------------------------------------------------
bool SpriteBatch::CreateVertexBuffer(ID3D12Device* device)
{
    const UINT bufferSize = static_cast<UINT>(sizeof(Vertex) * m_maxVertices);

    // 毎フレーム書き換えるので Map を保持する
    m_vertexBuffer = GpuBuffer::CreateUploadMapped(
        bufferSize, reinterpret_cast<void**>(&m_mappedVertices));
    if (!m_vertexBuffer) return false;
    m_vertexBuffer->SetName(L"SpriteBatchVertexBuffer");

    m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vertexBufferView.SizeInBytes = bufferSize;
    m_vertexBufferView.StrideInBytes = sizeof(Vertex);
    return true;
}

//-----------------------------------------------------------------------------
// Begin  ―  溜めていた頂点をクリア
//-----------------------------------------------------------------------------
void SpriteBatch::Begin()
{
    m_vertices.clear();
}

//-----------------------------------------------------------------------------
// Render  ―  スプライト1枚分（6頂点）を CPU 側に溜める
//-----------------------------------------------------------------------------
void SpriteBatch::Render(float dx, float dy,
    float dw, float dh,
    float r, float g, float b, float a,
    float angle,
    float sx, float sy, float sw, float sh)
{
    if (sw <= 0.0f) sw = static_cast<float>(m_texture.GetWidth());
    if (sh <= 0.0f) sh = static_cast<float>(m_texture.GetHeight());

    // 4頂点の位置（スクリーン座標）
    float x0 = dx;      float y0 = dy;
    float x1 = dx + dw; float y1 = dy;
    float x2 = dx;      float y2 = dy + dh;
    float x3 = dx + dw; float y3 = dy + dh;

    // 回転（最適化: 三角関数は1回だけ計算してインライン展開）
    if (angle != 0.0f)
    {
        const float rad = DirectX::XMConvertToRadians(angle);
        const float c = cosf(rad);
        const float s = sinf(rad);
        const float cx = dx + dw * 0.5f;
        const float cy = dy + dh * 0.5f;
        auto rot = [c, s, cx, cy](float& x, float& y)
            {
                float tx = x - cx, ty = y - cy;
                x = c * tx - s * ty + cx;
                y = s * tx + c * ty + cy;
            };
        rot(x0, y0); rot(x1, y1); rot(x2, y2); rot(x3, y3);
    }

    // NDC へ変換
    auto nx = [&](float x) { return 2.0f * x / m_screenWidth - 1.0f; };
    auto ny = [&](float y) { return 1.0f - 2.0f * y / m_screenHeight; };
    x0 = nx(x0); y0 = ny(y0);
    x1 = nx(x1); y1 = ny(y1);
    x2 = nx(x2); y2 = ny(y2);
    x3 = nx(x3); y3 = ny(y3);

    // テクセル → UV
    const float texW = static_cast<float>(m_texture.GetWidth());
    const float texH = static_cast<float>(m_texture.GetHeight());
    const float u0 = sx / texW, v0 = sy / texH;
    const float u1 = (sx + sw) / texW, v1 = (sy + sh) / texH;

    const XMFLOAT4 col = { r, g, b, a };

    // 矩形を三角形2枚（6頂点）に分割して溜める
    //   三角形1: 左上, 右上, 左下
    //   三角形2: 左下, 右上, 右下
    m_vertices.push_back({ { x0, y0, 0 }, col, { u0, v0 } });
    m_vertices.push_back({ { x1, y1, 0 }, col, { u1, v0 } });
    m_vertices.push_back({ { x2, y2, 0 }, col, { u0, v1 } });
    m_vertices.push_back({ { x2, y2, 0 }, col, { u0, v1 } });
    m_vertices.push_back({ { x1, y1, 0 }, col, { u1, v0 } });
    m_vertices.push_back({ { x3, y3, 0 }, col, { u1, v1 } });
}

//-----------------------------------------------------------------------------
// End  ―  溜めた全頂点をまとめて1回のドローコールで描画
//-----------------------------------------------------------------------------
void SpriteBatch::End(ID3D12GraphicsCommandList* commandList)
{
    assert(commandList);

    const size_t vertexCount = m_vertices.size();
    if (vertexCount == 0) return;
    assert(m_maxVertices >= vertexCount && "SpriteBatch バッファオーバーフロー");

    // 溜めた頂点を Map 済みバッファへ一括コピー
    if (m_mappedVertices)
    {
        memcpy(m_mappedVertices, m_vertices.data(),
            vertexCount * sizeof(Vertex));
    }

    // 描画（ドローコールは1回だけ）
    commandList->SetPipelineState(m_pipelineState.Get());
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    DescriptorHeap& srvHeap = DeviceManager::Instance()->GetSrvHeap();
    ID3D12DescriptorHeap* heaps[] = { srvHeap.GetHeap() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootDescriptorTable(0, m_texture.GetGpuHandle());

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    commandList->DrawInstanced(static_cast<UINT>(vertexCount), 1, 0, 0);

    commandList->DrawInstanced(static_cast<UINT>(vertexCount), 1, 0, 0);
    RenderStats::Instance().AddDrawCall(DrawCategory::Batch,
        static_cast<uint32_t>(vertexCount / 3));
}