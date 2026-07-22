#include "RHI/RaytracingAccel.h"
#include <cstring>

namespace
{
    // Create a DEFAULT-heap buffer that allows unordered access. Used for both
    // scratch and result buffers of an acceleration structure.
    ComPtr<ID3D12Resource> CreateUavBuffer(
        ID3D12Device* device, UINT64 size, D3D12_RESOURCE_STATES initialState)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = 0;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ComPtr<ID3D12Resource> res;
        device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            initialState, nullptr, IID_PPV_ARGS(&res));
        return res;
    }

    // Create an UPLOAD-heap buffer and copy data into it.
    ComPtr<ID3D12Resource> CreateUploadWithData(
        ID3D12Device* device, const void* data, UINT64 size)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> res;
        if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res))))
            return nullptr;

        if (data)
        {
            void* mapped = nullptr;
            D3D12_RANGE noRead = { 0, 0 };
            if (SUCCEEDED(res->Map(0, &noRead, &mapped)))
            {
                memcpy(mapped, data, static_cast<size_t>(size));
                res->Unmap(0, nullptr);
            }
        }
        return res;
    }

    void InsertUavBarrier(ID3D12GraphicsCommandList4* cmd, ID3D12Resource* res)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = res;
        cmd->ResourceBarrier(1, &b);
    }
}

namespace RaytracingAccel
{
    AccelBuffers BuildBottomLevel(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* cmd,
        D3D12_GPU_VIRTUAL_ADDRESS vbAddress, uint32_t vertexCount, uint32_t vertexStride,
        D3D12_GPU_VIRTUAL_ADDRESS ibAddress, uint32_t indexCount)
    {
        AccelBuffers out;

        D3D12_RAYTRACING_GEOMETRY_DESC geo = {};
        geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geo.Triangles.VertexBuffer.StartAddress = vbAddress;
        geo.Triangles.VertexBuffer.StrideInBytes = vertexStride;
        geo.Triangles.VertexCount = vertexCount;
        geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT; // position @ offset 0
        geo.Triangles.IndexBuffer = ibAddress;
        geo.Triangles.IndexCount = indexCount;
        geo.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        geo.Triangles.Transform3x4 = 0;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        inputs.NumDescs = 1;
        inputs.pGeometryDescs = &geo;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        if (info.ResultDataMaxSizeInBytes == 0)
            return out;

        out.scratch = CreateUavBuffer(device, info.ScratchDataSizeInBytes,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        out.result = CreateUavBuffer(device, info.ResultDataMaxSizeInBytes,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        if (!out.scratch || !out.result)
        {
            out = {};
            return out;
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
        build.Inputs = inputs;
        build.ScratchAccelerationStructureData = out.scratch->GetGPUVirtualAddress();
        build.DestAccelerationStructureData = out.result->GetGPUVirtualAddress();

        cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        InsertUavBarrier(cmd, out.result.Get());
        return out;
    }

    AccelBuffers BuildTopLevel(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* cmd,
        const std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& instances)
    {
        AccelBuffers out;
        if (instances.empty())
            return out;

        // Upload the instance descriptor array.
        const UINT64 instBytes =
            sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instances.size();
        out.instanceDesc = CreateUploadWithData(device, instances.data(), instBytes);
        if (!out.instanceDesc)
            return out;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        inputs.NumDescs = static_cast<UINT>(instances.size());
        inputs.InstanceDescs = out.instanceDesc->GetGPUVirtualAddress();

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        if (info.ResultDataMaxSizeInBytes == 0)
        {
            out = {};
            return out;
        }

        out.scratch = CreateUavBuffer(device, info.ScratchDataSizeInBytes,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        out.result = CreateUavBuffer(device, info.ResultDataMaxSizeInBytes,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        if (!out.scratch || !out.result)
        {
            out = {};
            return out;
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
        build.Inputs = inputs;
        build.ScratchAccelerationStructureData = out.scratch->GetGPUVirtualAddress();
        build.DestAccelerationStructureData = out.result->GetGPUVirtualAddress();

        cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        InsertUavBarrier(cmd, out.result.Get());
        return out;
    }

    void FillInstanceTransform(float dst[3][4], const float src[4][4])
    {
        // src is row-vector (v' = v * M): translation lives in row 3.
        // DXR wants column-vector (v' = T * v): a standard 3x4 with translation
        // in the last column. That is the transpose of the upper 3x4 of M.
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                dst[r][c] = src[c][r];
    }
}
