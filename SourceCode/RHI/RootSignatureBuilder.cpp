#include "RHI/RootSignatureBuilder.h"
#include "RHI/DeviceManager.h"

//-----------------------------------------------------------------------------
// AddCBV  ―  定数バッファ(bN)を追加
//-----------------------------------------------------------------------------
RootSignatureBuilder& RootSignatureBuilder::AddCBV(
    UINT shaderRegister, D3D12_SHADER_VISIBILITY visibility)
{
    D3D12_ROOT_PARAMETER p = {};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p.ShaderVisibility = visibility;
    p.Descriptor.ShaderRegister = shaderRegister;
    m_params.push_back(p);
    return *this;
}

//-----------------------------------------------------------------------------
// AddSRVTable  ―  テクスチャ(tN)をディスクリプタテーブルで追加
//-----------------------------------------------------------------------------
RootSignatureBuilder& RootSignatureBuilder::AddSRVTable(
    UINT baseRegister, UINT count, D3D12_SHADER_VISIBILITY visibility)
{
    // レンジを1つ追加
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = count;
    range.BaseShaderRegister = baseRegister;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    size_t rangeStart = m_ranges.size();
    m_ranges.push_back(range);

    // パラメータ（テーブル）を追加。pDescriptorRanges は Build で結びつける
    D3D12_ROOT_PARAMETER p = {};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.ShaderVisibility = visibility;
    p.DescriptorTable.NumDescriptorRanges = 1;
    m_params.push_back(p);

    m_pendingTables.push_back({ m_params.size() - 1, rangeStart, 1 });
    return *this;
}

//-----------------------------------------------------------------------------
// AddBindlessTable  ―  StructuredBuffer(t0) + unbounded テクスチャ配列(t1?)
//-----------------------------------------------------------------------------
RootSignatureBuilder& RootSignatureBuilder::AddBindlessTable(
    D3D12_SHADER_VISIBILITY visibility)
{
    size_t rangeStart = m_ranges.size();

    // range0: t0（マテリアル StructuredBuffer）
    D3D12_DESCRIPTOR_RANGE r0 = {};
    r0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r0.NumDescriptors = 1;
    r0.BaseShaderRegister = 0; // t0
    r0.OffsetInDescriptorsFromTableStart = 0;
    m_ranges.push_back(r0);

    // range1: t1?（テクスチャ配列・unbounded）
    D3D12_DESCRIPTOR_RANGE r1 = {};
    r1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r1.NumDescriptors = UINT_MAX; // unbounded
    r1.BaseShaderRegister = 1; // t1
    r1.OffsetInDescriptorsFromTableStart = 1;
    m_ranges.push_back(r1);

    D3D12_ROOT_PARAMETER p = {};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.ShaderVisibility = visibility;
    p.DescriptorTable.NumDescriptorRanges = 2;
    m_params.push_back(p);

    m_pendingTables.push_back({ m_params.size() - 1, rangeStart, 2 });
    return *this;
}

//-----------------------------------------------------------------------------
// AddStaticSampler  ―  静的サンプラー(sN)
//-----------------------------------------------------------------------------
RootSignatureBuilder& RootSignatureBuilder::AddStaticSampler(
    UINT shaderRegister, D3D12_FILTER filter)
{
    D3D12_STATIC_SAMPLER_DESC s = {};
    s.Filter = filter;
    s.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ShaderRegister = shaderRegister;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    m_samplers.push_back(s);
    return *this;
}

//-----------------------------------------------------------------------------
// Build  ―  RootSignature を生成
//-----------------------------------------------------------------------------
ComPtr<ID3D12RootSignature> RootSignatureBuilder::Build(const wchar_t* debugName)
{
    // テーブルのレンジポインタを今ここで結びつける（vectorが確定したので安全）
    for (const auto& t : m_pendingTables)
    {
        m_params[t.paramIndex].DescriptorTable.pDescriptorRanges =
            &m_ranges[t.rangeStart];
    }

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = static_cast<UINT>(m_params.size());
    desc.pParameters = m_params.empty() ? nullptr : m_params.data();
    desc.NumStaticSamplers = static_cast<UINT>(m_samplers.size());
    desc.pStaticSamplers = m_samplers.empty() ? nullptr : m_samplers.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
    {
        if (error)
            OutputDebugStringA(static_cast<const char*>(error->GetBufferPointer()));
        OutputDebugStringW(L"[RootSignatureBuilder] シリアライズ失敗\n");
        return nullptr;
    }

    auto* device = DeviceManager::Instance()->GetDevice();
    ComPtr<ID3D12RootSignature> rootSig;
    if (FAILED(device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&rootSig))))
    {
        OutputDebugStringW(L"[RootSignatureBuilder] 生成失敗\n");
        return nullptr;
    }
    if (debugName) rootSig->SetName(debugName);
    return rootSig;
}