#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// RaytracingPipeline
//  Wraps an ID3D12StateObject (the DXR "pipeline") plus its root signatures.
//
//  The state object is assembled from these subobjects:
//    - DXIL library (compiled PathTrace.hlsl) exporting the shader functions
//    - a hit group binding the closest-hit shader
//    - shader config  : payload / attribute byte sizes
//    - local root sig  : per-hit-record data (vertex buffer, index buffer, color)
//    - global root sig : output UAV table, TLAS SRV, scene CBV
//    - pipeline config : max ray recursion depth
//
//  Shader export names (must match [shader("...")] functions in the .hlsl):
//    RayGen / Miss / ShadowMiss / ClosestHit, hit group "HitGroup".
//-----------------------------------------------------------------------------
class RaytracingPipeline
{
public:
    // Export / hit-group names, shared with the shader-binding-table builder.
    static constexpr const wchar_t* kRayGen     = L"RayGen";
    static constexpr const wchar_t* kMiss       = L"Miss";
    static constexpr const wchar_t* kShadowMiss = L"ShadowMiss";
    static constexpr const wchar_t* kClosestHit = L"ClosestHit";
    static constexpr const wchar_t* kHitGroup   = L"HitGroup";

    RaytracingPipeline() = default;
    ~RaytracingPipeline() = default;

    RaytracingPipeline(const RaytracingPipeline&) = delete;
    RaytracingPipeline& operator=(const RaytracingPipeline&) = delete;

    // Build the state object from a compiled DXIL library blob.
    bool Initialize(ID3D12Device5* device, const std::vector<char>& dxilLibrary);

    bool IsValid() const { return m_stateObject != nullptr; }

    ID3D12StateObject*   GetStateObject()  const { return m_stateObject.Get(); }
    ID3D12RootSignature* GetGlobalRootSig() const { return m_globalRootSig.Get(); }

    // Shader identifier (32 bytes) for a given export name, used to fill the
    // shader binding table. Returns nullptr if the state object is invalid.
    const void* GetShaderIdentifier(const wchar_t* exportName) const;

private:
    bool CreateGlobalRootSignature(ID3D12Device5* device);
    bool CreateLocalRootSignature(ID3D12Device5* device);

    ComPtr<ID3D12StateObject>           m_stateObject;
    ComPtr<ID3D12StateObjectProperties> m_props;
    ComPtr<ID3D12RootSignature>         m_globalRootSig;
    ComPtr<ID3D12RootSignature>         m_localRootSig;
};
