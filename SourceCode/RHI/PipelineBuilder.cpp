#include "RHI/PipelineBuilder.h"
#include "RHI/DeviceManager.h"

//-----------------------------------------------------------------------------
// コンストラクタ  ―  共通のデフォルト値を埋める
//-----------------------------------------------------------------------------
PipelineBuilder::PipelineBuilder()
{
    m_desc = {};

    // ラスタライザ（既定: ソリッド・裏面カリング・時計回りが表）
    m_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    m_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    m_desc.RasterizerState.FrontCounterClockwise = FALSE;
    m_desc.RasterizerState.DepthClipEnable = TRUE;

    // ブレンド（既定: 不透明）
    m_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;

    // 深度（既定: 有効）
    m_desc.DepthStencilState.DepthEnable = TRUE;
    m_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    m_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    m_desc.DepthStencilState.StencilEnable = FALSE;

    // 共通の固定値
    m_desc.SampleMask = UINT_MAX;
    m_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    m_desc.NumRenderTargets = 1;
    m_desc.SampleDesc.Count = 1;
}

//-----------------------------------------------------------------------------
// Setter 群（自分自身を返して連結できるようにする）
//-----------------------------------------------------------------------------
PipelineBuilder& PipelineBuilder::SetRootSignature(ID3D12RootSignature* rootSig)
{
    m_desc.pRootSignature = rootSig;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetVS(const std::vector<char>& vs)
{
    m_desc.VS = { vs.data(), vs.size() };
    return *this;
}

PipelineBuilder& PipelineBuilder::SetPS(const std::vector<char>& ps)
{
    m_desc.PS = { ps.data(), ps.size() };
    return *this;
}

PipelineBuilder& PipelineBuilder::SetInputLayout(
    const D3D12_INPUT_ELEMENT_DESC* elements, UINT count)
{
    m_desc.InputLayout = { elements, count };
    return *this;
}

PipelineBuilder& PipelineBuilder::SetDepth(bool enable)
{
    m_desc.DepthStencilState.DepthEnable = enable ? TRUE : FALSE;
    m_desc.DepthStencilState.DepthWriteMask =
        enable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetCull(D3D12_CULL_MODE mode)
{
    m_desc.RasterizerState.CullMode = mode;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetFrontCounterClockwise(bool ccw)
{
    m_desc.RasterizerState.FrontCounterClockwise = ccw ? TRUE : FALSE;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetAlphaBlend(bool enable)
{
    auto& rt = m_desc.BlendState.RenderTarget[0];
    rt.BlendEnable = enable ? TRUE : FALSE;
    if (enable)
    {
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    return *this;
}

PipelineBuilder& PipelineBuilder::SetFillMode(D3D12_FILL_MODE mode)
{
    m_desc.RasterizerState.FillMode = mode;
    return *this;
}

//-----------------------------------------------------------------------------
// Build  ―  PSO を生成
//-----------------------------------------------------------------------------
ComPtr<ID3D12PipelineState> PipelineBuilder::Build(
    DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat)
{
    m_desc.RTVFormats[0] = rtvFormat;

    // 深度が無効なら DSVFormat は UNKNOWN にする（2Dスプライト等）
    m_desc.DSVFormat = m_desc.DepthStencilState.DepthEnable
        ? dsvFormat : DXGI_FORMAT_UNKNOWN;

    auto* device = DeviceManager::Instance()->GetDevice();
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(device->CreateGraphicsPipelineState(
        &m_desc, IID_PPV_ARGS(&pso))))
    {
        OutputDebugStringW(L"[PipelineBuilder] PSO 生成失敗\n");
        return nullptr;
    }
    return pso;
}