#include "RHI/RaytracingPipeline.h"
#include "d3dx12.h"          // CD3DX12_STATE_OBJECT_DESC (DirectXTK12/Src)
#include <cstdio>

//-----------------------------------------------------------------------------
// Payload / attribute sizes (must match PathTrace.hlsl)
//   Payload : float3 color + float  hitT   = 16 bytes
//   Attrib  : float2 barycentrics          = 8 bytes (built-in triangle attr)
//-----------------------------------------------------------------------------
static constexpr UINT kPayloadSize = 4 * sizeof(float); // 16
static constexpr UINT kAttribSize  = 2 * sizeof(float); // 8
static constexpr UINT kMaxRecursion = 2;                // primary + shadow

//-----------------------------------------------------------------------------
// Global root signature
//   param0 : descriptor table { UAV u0 }  -> output image
//   param1 : SRV  t0 (root)               -> TLAS
//   param2 : CBV  b0 (root)               -> scene constants
//-----------------------------------------------------------------------------
bool RaytracingPipeline::CreateGlobalRootSignature(ID3D12Device5* device)
{
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;      // u0
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3] = {};
    // param0: UAV table
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &uavRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // param1: TLAS SRV (t0)
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0; // t0
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // param2: scene CBV (b0)
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor.ShaderRegister = 0; // b0
    params[2].Descriptor.RegisterSpace = 0;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = _countof(params);
    desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err)))
    {
        if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
        OutputDebugStringW(L"[RT] global root signature serialize failed\n");
        return false;
    }
    if (FAILED(device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&m_globalRootSig))))
    {
        OutputDebugStringW(L"[RT] global root signature create failed\n");
        return false;
    }
    m_globalRootSig->SetName(L"RT_GlobalRootSig");
    return true;
}

//-----------------------------------------------------------------------------
// Local root signature (per hit-group record)
//   param0 : SRV t1 (root) -> vertex buffer (StructuredBuffer<Vertex>)
//   param1 : SRV t2 (root) -> index buffer  (StructuredBuffer<uint>)
//   param2 : 4x 32-bit root constants b1 -> base color (rgba)
//-----------------------------------------------------------------------------
bool RaytracingPipeline::CreateLocalRootSignature(ID3D12Device5* device)
{
    D3D12_ROOT_PARAMETER params[3] = {};
    // param0: vertex buffer SRV (t1)
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[0].Descriptor.ShaderRegister = 1; // t1
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // param1: index buffer SRV (t2)
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 2; // t2
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // param2: base color as 4 root constants (b1)
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 1; // b1
    params[2].Constants.RegisterSpace = 0;
    params[2].Constants.Num32BitValues = 4;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = _countof(params);
    desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err)))
    {
        if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
        OutputDebugStringW(L"[RT] local root signature serialize failed\n");
        return false;
    }
    if (FAILED(device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&m_localRootSig))))
    {
        OutputDebugStringW(L"[RT] local root signature create failed\n");
        return false;
    }
    m_localRootSig->SetName(L"RT_LocalRootSig");
    return true;
}

//-----------------------------------------------------------------------------
// Initialize  -  assemble the DXR state object
//-----------------------------------------------------------------------------
bool RaytracingPipeline::Initialize(ID3D12Device5* device,
    const std::vector<char>& dxilLibrary)
{
    if (!device || dxilLibrary.empty())
    {
        OutputDebugStringW(L"[RT] pipeline init: no device or empty DXIL\n");
        return false;
    }
    if (!CreateGlobalRootSignature(device)) return false;
    if (!CreateLocalRootSignature(device))  return false;

    CD3DX12_STATE_OBJECT_DESC pso{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };

    // ---- DXIL library + exports --------------------------------------
    auto* lib = pso.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    D3D12_SHADER_BYTECODE code{ dxilLibrary.data(), dxilLibrary.size() };
    lib->SetDXILLibrary(&code);
    lib->DefineExport(kRayGen);
    lib->DefineExport(kMiss);
    lib->DefineExport(kShadowMiss);
    lib->DefineExport(kClosestHit);

    // ---- hit group ----------------------------------------------------
    auto* hit = pso.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hit->SetClosestHitShaderImport(kClosestHit);
    hit->SetHitGroupExport(kHitGroup);
    hit->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

    // ---- shader config (payload / attribute sizes) -------------------
    auto* shaderConfig = pso.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    shaderConfig->Config(kPayloadSize, kAttribSize);

    // ---- local root signature + association to the hit group ---------
    auto* localRoot = pso.CreateSubobject<CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();
    localRoot->SetRootSignature(m_localRootSig.Get());
    auto* localAssoc = pso.CreateSubobject<CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
    localAssoc->SetSubobjectToAssociate(*localRoot);
    localAssoc->AddExport(kHitGroup);

    // ---- global root signature ---------------------------------------
    auto* globalRoot = pso.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    globalRoot->SetRootSignature(m_globalRootSig.Get());

    // ---- pipeline config (recursion depth) ---------------------------
    auto* pipelineConfig = pso.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    pipelineConfig->Config(kMaxRecursion);

    if (FAILED(device->CreateStateObject(pso, IID_PPV_ARGS(&m_stateObject))))
    {
        OutputDebugStringW(L"[RT] CreateStateObject failed\n");
        return false;
    }
    if (FAILED(m_stateObject.As(&m_props)))
    {
        OutputDebugStringW(L"[RT] query state object properties failed\n");
        return false;
    }

    OutputDebugStringW(L"[RT] raytracing state object created\n");
    return true;
}

//-----------------------------------------------------------------------------
// GetShaderIdentifier
//-----------------------------------------------------------------------------
const void* RaytracingPipeline::GetShaderIdentifier(const wchar_t* exportName) const
{
    if (!m_props) return nullptr;
    return m_props->GetShaderIdentifier(exportName);
}
