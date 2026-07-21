#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// PipelineBuilder
//  グラフィックスパイプラインステート(PSO)を組み立てる共通ビルダー。
//  各クラスで重複していた PSO 作成コードをまとめたもの。
//
//  使い方:
//    PipelineBuilder builder;
//    builder.SetRootSignature(rootSig);
//    builder.SetVS(vs).SetPS(ps);
//    builder.SetInputLayout(layout, count);
//    builder.SetDepth(true);           // 3D:true / 2D:false
//    builder.SetCull(D3D12_CULL_MODE_BACK);
//    builder.SetFrontCounterClockwise(true); // glTFなら表が反時計回り
//    m_pso = builder.Build(rtvFormat);
//
//  共通部分（SampleMask, SampleDesc, トポロジ等）はビルダーが自動で埋める。
//  違う部分（シェーダー, 入力レイアウト, カリング, 深度）だけ指定すればよい。
//-----------------------------------------------------------------------------
class PipelineBuilder
{
public:
    PipelineBuilder();

    PipelineBuilder& SetRootSignature(ID3D12RootSignature* rootSig);
    PipelineBuilder& SetVS(const std::vector<char>& vs);
    PipelineBuilder& SetPS(const std::vector<char>& ps);
    PipelineBuilder& SetInputLayout(const D3D12_INPUT_ELEMENT_DESC* elements, UINT count);

    /// @brief 深度テストの有効/無効（3Dはtrue、2Dはfalse）
    PipelineBuilder& SetDepth(bool enable);
    /// @brief カリングモード（BACK/FRONT/NONE）
    PipelineBuilder& SetCull(D3D12_CULL_MODE mode);
    /// @brief 表面の向き（glTFは反時計回りが表なのでtrue）
    PipelineBuilder& SetFrontCounterClockwise(bool ccw);
    /// @brief アルファブレンドを有効にする（2Dスプライト等）
    PipelineBuilder& SetAlphaBlend(bool enable);
    /// @brief 塗りつぶしモード（SOLID/WIREFRAME）
    PipelineBuilder& SetFillMode(D3D12_FILL_MODE mode);

    /// @brief PSO を生成する
    /// @param rtvFormat レンダーターゲットのフォーマット
    /// @param dsvFormat 深度バッファのフォーマット（既定 D32_FLOAT）
    ComPtr<ID3D12PipelineState> Build(
        DXGI_FORMAT rtvFormat,
        DXGI_FORMAT dsvFormat = DXGI_FORMAT_D32_FLOAT);

private:
    D3D12_GRAPHICS_PIPELINE_STATE_DESC m_desc = {};
};
