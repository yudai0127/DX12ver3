#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// RaytracingAccel
//  Helpers that build DXR acceleration structures (BLAS / TLAS).
//
//  DXR uses a two-level structure:
//    BLAS (Bottom Level) : the triangles of one mesh, in object space.
//    TLAS (Top Level)    : instances that each point at a BLAS with a
//                          per-instance world transform.
//
//  A build needs three GPU buffers:
//    - result  : the acceleration structure itself (keep alive while used).
//    - scratch : temporary work memory (keep alive only until the GPU
//                finishes the build, i.e. until Execute+Wait completes).
//    - instanceDesc (TLAS only) : upload buffer holding the instance array.
//
//  Usage (build once for a static scene):
//    cmd->Reset(...);
//    auto blas = RaytracingAccel::BuildBottomLevel(dev5, cmd4, ...);
//    // fill instance descs referencing blas.Result()->GetGPUVirtualAddress()
//    auto tlas = RaytracingAccel::BuildTopLevel(dev5, cmd4, instances);
//    cmd->ExecuteAndWait();     // <- scratch buffers can be freed after this
//-----------------------------------------------------------------------------
namespace RaytracingAccel
{
    // Bundle of GPU buffers produced by a build.
    struct AccelBuffers
    {
        ComPtr<ID3D12Resource> result;       // the BLAS/TLAS (persistent)
        ComPtr<ID3D12Resource> scratch;      // free after GPU build completes
        ComPtr<ID3D12Resource> instanceDesc; // TLAS only (upload buffer)

        ID3D12Resource* Result() const { return result.Get(); }
        D3D12_GPU_VIRTUAL_ADDRESS Address() const
        {
            return result ? result->GetGPUVirtualAddress() : 0;
        }
        bool IsValid() const { return result != nullptr; }
    };

    // Build a BLAS from a single indexed triangle geometry.
    //   vbAddress : GPU VA of the vertex buffer (position must be at offset 0,
    //               format R32G32B32_FLOAT, stride = vertexStride)
    //   ibAddress : GPU VA of a 32-bit index buffer (R32_UINT)
    AccelBuffers BuildBottomLevel(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* cmd,
        D3D12_GPU_VIRTUAL_ADDRESS vbAddress, uint32_t vertexCount, uint32_t vertexStride,
        D3D12_GPU_VIRTUAL_ADDRESS ibAddress, uint32_t indexCount);

    // Build a TLAS over the given instance descriptors.
    AccelBuffers BuildTopLevel(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* cmd,
        const std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& instances);

    // Fill a D3D12 instance transform (3x4, column-vector / row-major) from a
    // DirectXMath-style row-vector 4x4 world matrix (m[row][col]). This applies
    // the transpose that converts between the two conventions.
    void FillInstanceTransform(float dst[3][4], const float src[4][4]);
}
