#include "Graphics/Sprite.h"
#include "Graphics/ShaderManager.h"
#include "RHI/DeviceManager.h"
#include "RHI/CommandContext.h"
#include "RHI/DescriptorHeap.h"
#include "RHI/PipelineBuilder.h"
#include "RHI/RootSignatureBuilder.h"
#include "RHI/GpuBuffer.h"
#include "Core/RenderStats.h"
#include <cassert>
#include <vector>

using namespace DirectX;

//-----------------------------------------------------------------------------
// コンストラクタ  ―  授業資料の sprite(device, filename) の DX12 版
//   必要なものは DeviceManager から取得する。
//-----------------------------------------------------------------------------
Sprite::Sprite(const wchar_t* textureFile)
{
    auto* dm = DeviceManager::Instance();
    ID3D12Device* device = dm->GetDevice();
    m_screenWidth = dm->GetScreenWidth();
    m_screenHeight = dm->GetScreenHeight();

    // テクスチャ読み込み（中で DeviceManager から command/srvHeap を取る）
    if (!m_texture.Load(textureFile))
    {
        OutputDebugStringW(L"[Sprite] テクスチャ読み込み失敗\n");
        return;
    }

    if (!CreateRootSignature(device))                        return;
    if (!CreatePipelineState(device, dm->GetRTVFormat()))    return;
    if (!CreateVertexBuffer(device))                         return;
}

//-----------------------------------------------------------------------------
// CreateRootSignature
//  授業 UNIT05 ではテクスチャ(SRV)とサンプラーをシェーダーに渡す。
//  DX12 では RootSignature にそれらを定義する:
//    ・SRV   … ディスクリプタテーブル（t0）
//    ・サンプラー … StaticSampler（s0）として埋め込む（簡単）
//-----------------------------------------------------------------------------
bool Sprite::CreateRootSignature(ID3D12Device* device)
{
    // t0: テクスチャ（PS）、s0: ポイントサンプラー（授業UNIT05相当）
    RootSignatureBuilder builder;
    builder.AddSRVTable(0)
        .AddStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT);
    m_rootSignature = builder.Build(L"SpriteRootSignature");
    return m_rootSignature != nullptr;
}

//-----------------------------------------------------------------------------
// CreatePipelineState
//-----------------------------------------------------------------------------
bool Sprite::CreatePipelineState(ID3D12Device* device,
    DXGI_FORMAT   rtvFormat)
{
    std::vector<char> vs = ShaderManager::Instance()->LoadCSO("Sprite_VS.cso");
    std::vector<char> ps = ShaderManager::Instance()->LoadCSO("Sprite_PS.cso");
    if (vs.empty() || ps.empty())
    {
        OutputDebugStringW(L"[Sprite] CSO 読み込み失敗\n");
        return false;
    }

    // 入力レイアウト（授業 UNIT05: TEXCOORD を追加）
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
        .SetCull(D3D12_CULL_MODE_NONE) // カリングなし
        .SetAlphaBlend(true);          // 透過（PNG等）

    m_pipelineState = builder.Build(rtvFormat);
    if (!m_pipelineState)
    {
        OutputDebugStringW(L"[Sprite] PSO 生成失敗\n");
        return false;
    }
    m_pipelineState->SetName(L"SpritePSO");
    return true;
}

//-----------------------------------------------------------------------------
// CreateVertexBuffer  ―  4頂点の箱を用意（中身は Render で書き換え）
//-----------------------------------------------------------------------------
bool Sprite::CreateVertexBuffer(ID3D12Device* device)
{
    const UINT bufferSize = sizeof(Vertex) * VERTEX_COUNT;

    // 毎フレーム書き換えるので Map を保持する（CreateUploadMapped）
    m_vertexBuffer = GpuBuffer::CreateUploadMapped(
        bufferSize, reinterpret_cast<void**>(&m_mappedVertices));
    if (!m_vertexBuffer)
    {
        OutputDebugStringW(L"[Sprite] 頂点バッファ生成失敗\n");
        return false;
    }
    m_vertexBuffer->SetName(L"SpriteVertexBuffer");

    m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vertexBufferView.SizeInBytes = bufferSize;
    m_vertexBufferView.StrideInBytes = sizeof(Vertex);
    return true;
}

//-----------------------------------------------------------------------------
// Render
//-----------------------------------------------------------------------------
void Sprite::Render(ID3D12GraphicsCommandList* commandList,
    float dx, float dy,
    float dw, float dh,
    float r, float g, float b, float a,
    float angle,
    float sx, float sy, float sw, float sh)
{
    assert(commandList);

    // ---- 切り出し範囲が未指定(sw<=0)ならテクスチャ全体を使う --------
    if (sw <= 0.0f) sw = static_cast<float>(m_texture.GetWidth());
    if (sh <= 0.0f) sh = static_cast<float>(m_texture.GetHeight());

    // ---- テクセル座標 → UV 座標（0?1に正規化）授業 UNIT06 ---------
    const float texW = static_cast<float>(m_texture.GetWidth());
    const float texH = static_cast<float>(m_texture.GetHeight());
    const float u0 = sx / texW;
    const float v0 = sy / texH;
    const float u1 = (sx + sw) / texW;
    const float v1 = (sy + sh) / texH;

    // ---- 4頂点の位置（スクリーン座標）を計算 ------------------------
    float x0 = dx;        float y0 = dy;        // 左上
    float x1 = dx + dw;   float y1 = dy;        // 右上
    float x2 = dx;        float y2 = dy + dh;   // 左下
    float x3 = dx + dw;   float y3 = dy + dh;   // 右下

    // ---- 回転（UNIT04）---------------------------------------------
    if (angle != 0.0f)
    {
        auto rotate = [](float& x, float& y, float cx, float cy, float angle)
            {
                x -= cx; y -= cy;
                float cos = cosf(DirectX::XMConvertToRadians(angle));
                float sin = sinf(DirectX::XMConvertToRadians(angle));
                float tx = x, ty = y;
                x = cos * tx + -sin * ty;
                y = sin * tx + cos * ty;
                x += cx; y += cy;
            };
        float cx = dx + dw * 0.5f;
        float cy = dy + dh * 0.5f;
        rotate(x0, y0, cx, cy, angle);
        rotate(x1, y1, cx, cy, angle);
        rotate(x2, y2, cx, cy, angle);
        rotate(x3, y3, cx, cy, angle);
    }

    // ---- スクリーン座標 → NDC ---------------------------------------
    auto toNDC_X = [&](float x) { return 2.0f * x / m_screenWidth - 1.0f; };
    auto toNDC_Y = [&](float y) { return 1.0f - 2.0f * y / m_screenHeight; };
    x0 = toNDC_X(x0); y0 = toNDC_Y(y0);
    x1 = toNDC_X(x1); y1 = toNDC_Y(y1);
    x2 = toNDC_X(x2); y2 = toNDC_Y(y2);
    x3 = toNDC_X(x3); y3 = toNDC_Y(y3);

    // ---- 頂点バッファを書き換え（位置・色・texcoord）----------------
    if (m_mappedVertices)
    {
        const XMFLOAT4 color = { r, g, b, a };
        m_mappedVertices[0] = { { x0, y0, 0 }, color, { u0, v0 } }; // 左上
        m_mappedVertices[1] = { { x1, y1, 0 }, color, { u1, v0 } }; // 右上
        m_mappedVertices[2] = { { x2, y2, 0 }, color, { u0, v1 } }; // 左下
        m_mappedVertices[3] = { { x3, y3, 0 }, color, { u1, v1 } }; // 右下
    }

    // ---- 描画コマンド -----------------------------------------------
    commandList->SetPipelineState(m_pipelineState.Get());
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    // SRV ヒープをセットして、テクスチャの SRV をシェーダー(t0)に渡す
    // SRV ヒープをセットして、テクスチャの SRV をシェーダー(t0)に渡す
    DescriptorHeap& srvHeap = DeviceManager::Instance()->GetSrvHeap();
    ID3D12DescriptorHeap* heaps[] = { srvHeap.GetHeap() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootDescriptorTable(0, m_texture.GetGpuHandle());

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    commandList->DrawInstanced(VERTEX_COUNT, 1, 0, 0);

    commandList->DrawInstanced(VERTEX_COUNT, 1, 0, 0);
    RenderStats::Instance().AddDrawCall(DrawCategory::Sprite, 2);
}

//-----------------------------------------------------------------------------
// Render（簡易版）授業 UNIT10 手順5
//   位置とサイズだけ指定。色は白、テクスチャ全体、回転なし。
//-----------------------------------------------------------------------------
void Sprite::Render(ID3D12GraphicsCommandList* commandList,
    float dx, float dy, float dw, float dh)
{
    Render(commandList, dx, dy, dw, dh, 1, 1, 1, 1, 0, 0, 0, 0, 0);
}

//-----------------------------------------------------------------------------
// TextOut  授業 UNIT10 手順8
//   フォント画像（アスキーコード順に16x16で文字が並ぶ）から
//   1文字ずつ切り出して並べ、文字列を描画する。
//-----------------------------------------------------------------------------
void Sprite::TextOut(ID3D12GraphicsCommandList* commandList,
    const std::string& text,
    float x, float y, float w, float h,
    float r, float g, float b, float a)
{
    // フォント画像1文字あたりのテクセルサイズ（画像を16x16分割）
    const float sw = static_cast<float>(m_texture.GetWidth()) / 16.0f;
    const float sh = static_cast<float>(m_texture.GetHeight()) / 16.0f;

    float carriage = 0.0f; // 横方向の送り
    for (const char c : text)
    {
        // 文字コード c の下位4bitが列、上位4bitが行
        const float srcX = sw * (c & 0x0F);
        const float srcY = sh * ((c >> 4) & 0x0F);

        Render(commandList,
            x + carriage, y, w, h,
            r, g, b, a, 0,
            srcX, srcY, sw, sh);

        carriage += w; // 次の文字へ
    }
}