#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include "Graphics/Texture.h"

using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// Sprite
//  テクスチャ付き 2D 矩形を描画する。
//  device 等は DeviceManager から取得するので、コンストラクタは画像ファイル名だけ。
//
//  使い方:
//    auto sprite = std::make_unique<Sprite>(L"resources/title.png");
//    sprite->Render(commandList, dx, dy, dw, dh, r,g,b,a, angle, sx,sy,sw,sh);
//-----------------------------------------------------------------------------
class Sprite
{
public:
    struct Vertex
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT4 color;
        DirectX::XMFLOAT2 texcoord;
    };

    /// @brief 画像ファイルを指定して生成する（授業の sprite コンストラクタ風）
    explicit Sprite(const wchar_t* textureFile);
    ~Sprite() = default;

    Sprite(const Sprite&) = delete;
    Sprite& operator=(const Sprite&) = delete;

    /// @brief 生成に成功したか（コンストラクタは例外を投げないのでこれで確認）
    bool IsValid() const { return m_pipelineState != nullptr; }

    //-------------------------------------------------------------------
    // 描画  ―  授業資料 UNIT03?06 の sprite::render に相当
    //-------------------------------------------------------------------

    /// @param dx,dy,dw,dh 表示する矩形（スクリーン座標・ピクセル）
    /// @param r,g,b,a     表示色
    /// @param angle       回転角（度）
    /// @param sx,sy,sw,sh 切り出すテクスチャ領域（テクセル）。sw<=0で全体
    void Render(ID3D12GraphicsCommandList* commandList,
        float dx, float dy,
        float dw, float dh,
        float r, float g, float b, float a,
        float angle = 0.0f,
        float sx = 0, float sy = 0, float sw = 0, float sh = 0);

    /// @brief 簡易版（位置とサイズだけ・色は白・テクスチャ全体）授業 UNIT10 手順5
    void Render(ID3D12GraphicsCommandList* commandList,
        float dx, float dy, float dw, float dh);

    /// @brief フォント画像を使って文字列を描画する 授業 UNIT10 手順8
    ///   フォント画像はアスキーコード順に 16x16 で文字が並んだもの。
    /// @param text 表示する文字列
    /// @param x,y  表示開始位置（左上）
    /// @param w,h  1文字あたりのサイズ
    void TextOut(ID3D12GraphicsCommandList* commandList,
        const std::string& text,
        float x, float y, float w, float h,
        float r = 1, float g = 1, float b = 1, float a = 1);

private:
    bool CreateRootSignature(ID3D12Device* device);
    bool CreatePipelineState(ID3D12Device* device, DXGI_FORMAT rtvFormat);
    bool CreateVertexBuffer(ID3D12Device* device);

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;
    ComPtr<ID3D12Resource>      m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW    m_vertexBufferView = {};

    Texture m_texture;

    Vertex* m_mappedVertices = nullptr;
    float   m_screenWidth = 1280.0f;
    float   m_screenHeight = 720.0f;

    static constexpr uint32_t VERTEX_COUNT = 4;
};