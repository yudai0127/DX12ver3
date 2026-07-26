#include "Graphics/RaytracingRenderer.h"
#include "Graphics/ShaderManager.h"
#include "Graphics/GltfModel.h"
#include "RHI/DeviceManager.h"
#include "RHI/DLSS.h"
#include "RHI/GpuBuffer.h"
#include "Core/Scene.h"
#include "Core/GameObject.h"
#include "Component/ModelRenderer.h"
#include "Camera/Camera.h"
#include <cstring>

using namespace DirectX;

namespace
{
    constexpr UINT kShaderIdSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32
    constexpr UINT kRecordAlign  = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT; // 32
    constexpr UINT kTableAlign   = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;  // 64

    inline UINT64 Align(UINT64 size, UINT64 align)
    {
        return (size + align - 1) & ~(align - 1);
    }

    // Matches DenoiseCB in Denoise_CS.hlsl (32 bytes).
    struct DenoiseConstants
    {
        DirectX::XMUINT2 dim;
        uint32_t         sampleCount;
        int32_t          radius;

        float            exposure;
        float            normalPower;
        float            depthSigma;
        float            temporalAlpha;

        DirectX::XMFLOAT4X4 invViewProj;   // current frame (reconstruct world pos)
        DirectX::XMFLOAT4X4 prevViewProj;  // previous frame (reproject history)
        DirectX::XMFLOAT4   cameraPos;

        int32_t          temporalValid;    // 1 = history usable this frame
        float            pad0, pad1, pad2;
    };

    struct UpscaleConstants
    {
        DirectX::XMUINT2 dstDim; // output resolution
        DirectX::XMUINT2 srcDim; // rendered sub-rectangle
        DirectX::XMUINT2 texDim; // allocated source texture size
        uint32_t         tonemap; // 1 = source is linear HDR
        float            exposure;
    };
}

//-----------------------------------------------------------------------------
// Initialize
//-----------------------------------------------------------------------------
bool RaytracingRenderer::Initialize(Scene* scene, uint32_t width, uint32_t height)
{
    m_valid = false;
    m_width = width;
    m_height = height;

    auto* dm = DeviceManager::Instance();
    if (!dm->GetDeviceObj().SupportsRaytracing())
    {
        OutputDebugStringW(L"[RT] device does not support raytracing; skipping\n");
        return false;
    }

    if (!BuildPipeline())                     return false;
    if (!BuildDenoisePipeline())              return false;
    if (!BuildUpscalePipeline())              return false;
    if (!m_sceneCB.Initialize(sizeof(SceneConstants))) return false;
    if (!m_denoiseCB.Initialize(sizeof(DenoiseConstants))) return false;
    if (!m_upscaleCB.Initialize(sizeof(UpscaleConstants))) return false;
    // AS build also collects the per-model texture list used by the heap below.
    if (!BuildAccelerationStructures(scene))  return false;

    // Optional equirectangular environment map. Drop an image at one of these
    // paths (JPG/PNG, or a DDS converted from an .hdr with texconv). If none
    // loads, the shader falls back to the procedural sky.
    {
        const wchar_t* candidates[] = {
            L"Resources/env.dds", L"Resources/env.png", L"Resources/env.jpg"
        };
        for (const wchar_t* path : candidates)
        {
            auto tex = std::make_unique<Texture>();
            if (tex->Load(path) && tex->IsValid())
            {
                m_envTexIndex = static_cast<int>(m_textureResources.size());
                m_textureResources.push_back(tex->GetResource());
                m_envTexture = std::move(tex);
                OutputDebugStringW(L"[RT] environment map loaded\n");
                break;
            }
        }
    }

    if (!BuildDescriptorHeap())               return false;
    if (!CreateOutputResource(width, height)) return false;
    if (!BuildShaderTable())                  return false;

    m_valid = true;
    OutputDebugStringW(L"[RT] raytracing renderer ready\n");
    return true;
}

//-----------------------------------------------------------------------------
// BuildPipeline  -  compile the DXR library and create the state object
//-----------------------------------------------------------------------------
bool RaytracingRenderer::BuildPipeline()
{
    auto* dm = DeviceManager::Instance();
    ID3D12Device5* dev5 = dm->GetDeviceObj().GetDevice5();
    if (!dev5) return false;

    std::vector<char> lib =
        ShaderManager::Instance()->CompileLibrary(L"HLSL/PathTrace.hlsl");
    if (lib.empty())
    {
        OutputDebugStringW(L"[RT] PathTrace.hlsl compile failed\n");
        return false;
    }
    return m_pipeline.Initialize(dev5, lib);
}

//-----------------------------------------------------------------------------
// BuildDenoisePipeline  -  compute root signature + PSO for the denoiser.
//   Root sig: table { SRV t0-t1 (color, geo) + UAV u0 (output) }, CBV b0.
//-----------------------------------------------------------------------------
bool RaytracingRenderer::BuildDenoisePipeline()
{
    auto* dm = DeviceManager::Instance();
    ID3D12Device* device = dm->GetDevice();

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; // t0-t5: accum, geo, albedo, histColor, histGeo, motion
    ranges[0].NumDescriptors = 6;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; // u0-u2: output, histColorOut, histGeoOut
    ranges[1].NumDescriptors = 3;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 6;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 0; // b0
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err)))
    {
        if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
        OutputDebugStringW(L"[RT] denoise root signature serialize failed\n");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(),
        blob->GetBufferSize(), IID_PPV_ARGS(&m_denoiseRootSig))))
    {
        OutputDebugStringW(L"[RT] denoise root signature create failed\n");
        return false;
    }

    std::vector<char> cs = ShaderManager::Instance()->CompileFromFile(
        L"HLSL/Denoise_CS.hlsl", L"main", L"cs_6_0");
    if (cs.empty())
    {
        OutputDebugStringW(L"[RT] Denoise_CS.hlsl compile failed\n");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_denoiseRootSig.Get();
    pd.CS.pShaderBytecode = cs.data();
    pd.CS.BytecodeLength = cs.size();
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_denoisePSO))))
    {
        OutputDebugStringW(L"[RT] denoise PSO create failed\n");
        return false;
    }
    return true;
}

//-----------------------------------------------------------------------------
// BuildUpscalePipeline  -  compute root signature + PSO for the fallback
// bilinear upscale (render resolution -> output resolution).
//   Root sig: table { SRV t0 (source), UAV u0 (dest) }, CBV b0, static sampler.
//-----------------------------------------------------------------------------
bool RaytracingRenderer::BuildUpscalePipeline()
{
    auto* dm = DeviceManager::Instance();
    ID3D12Device* device = dm->GetDevice();

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; // t0 source
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; // u0 dest
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 0; // b0
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0; // s0
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &samp;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err)))
    {
        if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
        OutputDebugStringW(L"[RT] upscale root signature serialize failed\n");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(),
        blob->GetBufferSize(), IID_PPV_ARGS(&m_upscaleRootSig))))
    {
        OutputDebugStringW(L"[RT] upscale root signature create failed\n");
        return false;
    }

    std::vector<char> cs = ShaderManager::Instance()->CompileFromFile(
        L"HLSL/Upscale_CS.hlsl", L"main", L"cs_6_0");
    if (cs.empty())
    {
        OutputDebugStringW(L"[RT] Upscale_CS.hlsl compile failed\n");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_upscaleRootSig.Get();
    pd.CS.pShaderBytecode = cs.data();
    pd.CS.BytecodeLength = cs.size();
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_upscalePSO))))
    {
        OutputDebugStringW(L"[RT] upscale PSO create failed\n");
        return false;
    }
    return true;
}

//-----------------------------------------------------------------------------
// BuildAccelerationStructures
//   Walk every ModelRenderer in the scene: build one BLAS per primitive and
//   one TLAS instance per (node, primitive). InstanceContributionToHitGroupIndex
//   maps 1:1 to a shader-binding-table hit record, so SBT indexing stays trivial.
//-----------------------------------------------------------------------------
bool RaytracingRenderer::BuildAccelerationStructures(Scene* scene)
{
    if (!scene) return false;

    auto* dm = DeviceManager::Instance();
    ID3D12Device5* dev5 = dm->GetDeviceObj().GetDevice5();
    auto& cmdCtx = dm->GetCommand();
    ID3D12GraphicsCommandList4* cmd4 = cmdCtx.GetCommandList4();
    if (!dev5 || !cmd4) return false;

    // Start recording on the main command list (frame loop hasn't begun yet).
    cmdCtx.Reset(0);

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
    UINT recordIndex = 0;

    // Accumulate the scene's world-space bounds to place the reflective floor.
    XMVECTOR sceneMin = XMVectorReplicate(1e30f);
    XMVECTOR sceneMax = XMVectorReplicate(-1e30f);

    for (const auto& obj : scene->GetObjects())
    {
        if (!obj) continue;
        ModelRenderer* mr = obj->GetComponent<ModelRenderer>();
        if (!mr) continue;
        GltfModel* model = mr->GetModel();
        if (!model || !model->IsValid()) continue;

        XMFLOAT4X4 world = mr->GetWorldMatrix();
        XMMATRIX   worldM = XMLoadFloat4x4(&world);

        const auto& nodes = model->GetNodes();
        const auto& meshes = model->GetMeshes();
        const auto& materials = model->GetMaterials();

        // Append this model's textures to the shared bindless array and remember
        // where they start, so per-primitive texture indices can be offset.
        const int texBase = static_cast<int>(m_textureResources.size());
        {
            std::vector<ID3D12Resource*> modelTex = model->GetTextureResourcesForRT();
            m_textureResources.insert(
                m_textureResources.end(), modelTex.begin(), modelTex.end());
        }

        for (const auto& node : nodes)
        {
            if (node.mesh < 0 || (size_t)node.mesh >= meshes.size()) continue;
            const auto& mesh = meshes[node.mesh];

            XMMATRIX   instM = XMLoadFloat4x4(&node.global_transform) * worldM;
            XMFLOAT4X4 instWorld;
            XMStoreFloat4x4(&instWorld, instM);

            for (const auto& prim : mesh.primitives)
            {
                if (!prim.vertexBuffer || !prim.indexBuffer || prim.indexCount == 0)
                    continue;

                const UINT stride = prim.vbView.StrideInBytes;
                const UINT vertexCount = stride ? prim.vbView.SizeInBytes / stride : 0;
                if (vertexCount == 0) continue;

                auto blas = RaytracingAccel::BuildBottomLevel(dev5, cmd4,
                    prim.vbView.BufferLocation, vertexCount, stride,
                    prim.ibView.BufferLocation, prim.indexCount);
                if (!blas.IsValid()) continue;

                D3D12_RAYTRACING_INSTANCE_DESC inst = {};
                RaytracingAccel::FillInstanceTransform(inst.Transform, instWorld.m);
                inst.InstanceID = recordIndex;
                inst.InstanceMask = 0xFF;
                inst.InstanceContributionToHitGroupIndex = recordIndex;
                inst.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
                inst.AccelerationStructure = blas.Address();
                instances.push_back(inst);

                HitRecordData hd;
                hd.vbAddress = prim.vbView.BufferLocation;
                hd.ibAddress = prim.ibView.BufferLocation;
                if (prim.material >= 0 && (size_t)prim.material < materials.size())
                {
                    const auto& mat = materials[prim.material];
                    hd.baseColor = mat.basecolor_factor;
                    hd.emissive = mat.emissive_factor;
                    hd.metallic = mat.metallic_factor;
                    hd.roughness = mat.roughness_factor;
                    hd.normalScale = mat.normal_scale;
                    // Texture indices reference this model's textures; offset
                    // them into the shared bindless array (-1 stays -1).
                    auto remap = [texBase](int idx) { return idx >= 0 ? texBase + idx : -1; };
                    hd.baseColorTex = remap(mat.basecolor_texture);
                    hd.mrTex        = remap(mat.metallic_roughness_texture);
                    hd.normalTex    = remap(mat.normal_texture);
                    hd.emissiveTex  = remap(mat.emissive_texture);
                }
                m_hitData.push_back(hd);

                // Expand scene bounds by this primitive's transformed AABB.
                XMVECTOR amn = XMLoadFloat3(&prim.aabbMin);
                XMVECTOR amx = XMLoadFloat3(&prim.aabbMax);
                for (int corner = 0; corner < 8; ++corner)
                {
                    XMVECTOR c = XMVectorSet(
                        (corner & 1) ? XMVectorGetX(amx) : XMVectorGetX(amn),
                        (corner & 2) ? XMVectorGetY(amx) : XMVectorGetY(amn),
                        (corner & 4) ? XMVectorGetZ(amx) : XMVectorGetZ(amn), 1.0f);
                    XMVECTOR w = XMVector3Transform(c, instM);
                    sceneMin = XMVectorMin(sceneMin, w);
                    sceneMax = XMVectorMax(sceneMax, w);
                }

                m_blas.push_back(std::move(blas));
                ++recordIndex;
            }
        }
    }

    // ---- reflective floor -------------------------------------------
    // A large mirror quad at the model's feet, so reflections are obvious.
    if (recordIndex > 0)
    {
        XMFLOAT3 mn, mx;
        XMStoreFloat3(&mn, sceneMin);
        XMStoreFloat3(&mx, sceneMax);

        const float y  = mn.y;                       // floor at the lowest point
        const float cx = (mn.x + mx.x) * 0.5f;
        const float cz = (mn.z + mx.z) * 0.5f;
        const float dx = mx.x - mn.x;
        const float dz = mx.z - mn.z;
        float half = (dx > dz ? dx : dz) * 2.0f;     // extend well past the model
        if (half < 100.0f) half = 100.0f;

        auto setV = [](GltfModel::Vertex& v, float px, float py, float pz, float u, float w)
        {
            v.position = { px, py, pz };
            v.normal   = { 0.0f, 1.0f, 0.0f };
            v.tangent  = { 1.0f, 0.0f, 0.0f, 1.0f };
            v.texcoord = { u, w };
        };
        GltfModel::Vertex fv[4];
        setV(fv[0], cx - half, y, cz - half, 0.0f, 0.0f);
        setV(fv[1], cx - half, y, cz + half, 0.0f, 1.0f);
        setV(fv[2], cx + half, y, cz + half, 1.0f, 1.0f);
        setV(fv[3], cx + half, y, cz - half, 1.0f, 0.0f);
        const uint32_t fi[6] = { 0, 1, 2, 0, 2, 3 };

        m_floorVB = GpuBuffer::CreateUploadWithData(fv, sizeof(fv));
        m_floorIB = GpuBuffer::CreateUploadWithData(fi, sizeof(fi));
        if (m_floorVB && m_floorIB)
        {
            auto blas = RaytracingAccel::BuildBottomLevel(dev5, cmd4,
                m_floorVB->GetGPUVirtualAddress(), 4, sizeof(GltfModel::Vertex),
                m_floorIB->GetGPUVirtualAddress(), 6);
            if (blas.IsValid())
            {
                D3D12_RAYTRACING_INSTANCE_DESC inst = {};
                inst.Transform[0][0] = 1.0f; // identity (verts already in world space)
                inst.Transform[1][1] = 1.0f;
                inst.Transform[2][2] = 1.0f;
                inst.InstanceID = recordIndex;
                inst.InstanceMask = 0xFF;
                inst.InstanceContributionToHitGroupIndex = recordIndex;
                inst.AccelerationStructure = blas.Address();
                instances.push_back(inst);

                HitRecordData hd;
                hd.vbAddress = m_floorVB->GetGPUVirtualAddress();
                hd.ibAddress = m_floorIB->GetGPUVirtualAddress();
                hd.baseColor = { 0.55f, 0.55f, 0.60f, 1.0f };
                hd.metallic = 1.0f;      // mirror-like
                hd.roughness = 0.05f;
                m_hitData.push_back(hd);

                m_blas.push_back(std::move(blas));
                ++recordIndex;
            }
        }
    }

    if (instances.empty())
    {
        OutputDebugStringW(L"[RT] no geometry found for acceleration structures\n");
        cmdCtx.ExecuteAndWait();
        return false;
    }

    m_tlas = RaytracingAccel::BuildTopLevel(dev5, cmd4, instances);
    cmdCtx.ExecuteAndWait();

    if (!m_tlas.IsValid())
        return false;

    // Scratch buffers were only needed during the GPU build.
    for (auto& b : m_blas) b.scratch.Reset();
    m_tlas.scratch.Reset();
    m_tlas.instanceDesc.Reset();
    return true;
}

//-----------------------------------------------------------------------------
// BuildShaderTable
//   Layout in one upload buffer (each region aligned to 64 bytes):
//     [ raygen record ][ miss records x2 ][ hit records xN ]
//   A hit record is: shaderId(32) + vbVA(8) + ibVA(8) + baseColor(16) = 64.
//-----------------------------------------------------------------------------
bool RaytracingRenderer::BuildShaderTable()
{
    const void* idRayGen     = m_pipeline.GetShaderIdentifier(RaytracingPipeline::kRayGen);
    const void* idMiss       = m_pipeline.GetShaderIdentifier(RaytracingPipeline::kMiss);
    const void* idShadowMiss = m_pipeline.GetShaderIdentifier(RaytracingPipeline::kShadowMiss);
    const void* idHitGroup   = m_pipeline.GetShaderIdentifier(RaytracingPipeline::kHitGroup);
    if (!idRayGen || !idMiss || !idShadowMiss || !idHitGroup)
    {
        OutputDebugStringW(L"[RT] missing shader identifiers\n");
        return false;
    }

    const UINT numMiss = 2;
    const UINT numHit = static_cast<UINT>(m_hitData.size());

    const UINT raygenStride = (UINT)Align(kShaderIdSize, kRecordAlign);          // 32
    m_missStride = (UINT)Align(kShaderIdSize, kRecordAlign);                     // 32
    // hit record: shaderId + vbVA(8) + ibVA(8) + b1 constants(60)
    //   b1 = baseColor(16) + emissive(16) + metallic(4) + roughness(4)
    //        + normalScale(4) + baseColorTex(4) + mrTex(4) + normalTex(4)
    //        + emissiveTex(4)
    m_hitStride  = (UINT)Align(kShaderIdSize + 8 + 8 + 60, kRecordAlign);        // 128

    m_raygenRegionSize = raygenStride;
    m_missRegionSize   = numMiss * m_missStride;
    m_hitRegionSize    = numHit  * m_hitStride;

    m_raygenOffset = 0;
    m_missOffset = Align(m_raygenOffset + m_raygenRegionSize, kTableAlign);
    m_hitOffset  = Align(m_missOffset + m_missRegionSize, kTableAlign);
    const UINT64 total = m_hitOffset + m_hitRegionSize;

    void* mapped = nullptr;
    m_shaderTable = GpuBuffer::CreateUploadMapped(total, &mapped);
    if (!m_shaderTable || !mapped)
    {
        OutputDebugStringW(L"[RT] shader table allocation failed\n");
        return false;
    }
    uint8_t* base = static_cast<uint8_t*>(mapped);
    memset(base, 0, static_cast<size_t>(total));

    // raygen
    memcpy(base + m_raygenOffset, idRayGen, kShaderIdSize);

    // miss (index 0 = primary, index 1 = shadow)
    memcpy(base + m_missOffset + 0 * m_missStride, idMiss, kShaderIdSize);
    memcpy(base + m_missOffset + 1 * m_missStride, idShadowMiss, kShaderIdSize);

    // hit records
    for (UINT i = 0; i < numHit; ++i)
    {
        uint8_t* rec = base + m_hitOffset + (UINT64)i * m_hitStride;
        memcpy(rec, idHitGroup, kShaderIdSize);
        uint8_t* args = rec + kShaderIdSize;
        memcpy(args + 0,  &m_hitData[i].vbAddress, sizeof(D3D12_GPU_VIRTUAL_ADDRESS));
        memcpy(args + 8,  &m_hitData[i].ibAddress, sizeof(D3D12_GPU_VIRTUAL_ADDRESS));
        // b1 constants (must match HitCB layout in PathTrace.hlsl)
        uint8_t* cb = args + 16;
        memcpy(cb + 0,  &m_hitData[i].baseColor,    sizeof(XMFLOAT4)); // dwords 0-3
        memcpy(cb + 16, &m_hitData[i].emissive,     sizeof(XMFLOAT3)); // dwords 4-6 (7=pad)
        memcpy(cb + 32, &m_hitData[i].metallic,     sizeof(float));    // dword 8
        memcpy(cb + 36, &m_hitData[i].roughness,    sizeof(float));    // dword 9
        memcpy(cb + 40, &m_hitData[i].normalScale,  sizeof(float));    // dword 10
        memcpy(cb + 44, &m_hitData[i].baseColorTex, sizeof(int));      // dword 11
        memcpy(cb + 48, &m_hitData[i].mrTex,        sizeof(int));      // dword 12
        memcpy(cb + 52, &m_hitData[i].normalTex,    sizeof(int));      // dword 13
        memcpy(cb + 56, &m_hitData[i].emissiveTex,  sizeof(int));      // dword 14
    }
    return true;
}

//-----------------------------------------------------------------------------
// BuildDescriptorHeap  -  one shader-visible heap shared by the ray tracer:
//   slot 0      : output UAV (u0)          -> filled by CreateOutputResource
//   slots 1..T  : base-color texture SRVs  -> t3 bindless array
//   Must run after BuildAccelerationStructures (which fills m_textureResources).
//-----------------------------------------------------------------------------
bool RaytracingRenderer::BuildDescriptorHeap()
{
    auto* dm = DeviceManager::Instance();
    ID3D12Device* device = dm->GetDevice();

    const UINT texCount = static_cast<UINT>(m_textureResources.size());
    // Layout: 8 UAVs (output u0, accum u1, geo u2, albedo u3, motion u4,
    // normal+roughness u5, specular albedo u6, linear depth u7)
    // + texture SRVs + 18 denoiser descriptors (two 9-entry ping-pong tables)
    // + 4 upscale descriptors (two source SRV / destination UAV pairs: the
    //   bilinear fallback and the DLSS tonemap).
    if (!m_srvUavHeap.Initialize(device, 31 + texCount, /*shaderVisible*/ true))
    {
        OutputDebugStringW(L"[RT] descriptor heap init failed\n");
        return false;
    }

    // Views are created in CreateOutputResource; the RT UAV table base is slot 0.
    auto outSlot = m_srvUavHeap.Allocate();  // slot 0: output UAV (u0)
    m_outputUavCpu = outSlot.cpu;
    m_outputUavGpu = outSlot.gpu;
    auto accSlot = m_srvUavHeap.Allocate();  // slot 1: accum UAV (u1)
    m_accumUavCpu = accSlot.cpu;
    auto geoSlot = m_srvUavHeap.Allocate();  // slot 2: geo UAV (u2)
    m_geoUavCpu = geoSlot.cpu;
    auto albSlot = m_srvUavHeap.Allocate();  // slot 3: albedo UAV (u3)
    m_albedoUavCpu = albSlot.cpu;
    auto mvSlot = m_srvUavHeap.Allocate();   // slot 4: motion UAV (u4)
    m_motionUavCpu = mvSlot.cpu;
    auto nrSlot = m_srvUavHeap.Allocate();   // slot 5: normal+roughness UAV (u5)
    m_normalRoughUavCpu = nrSlot.cpu;
    auto saSlot = m_srvUavHeap.Allocate();   // slot 6: specular albedo UAV (u6)
    m_specAlbedoUavCpu = saSlot.cpu;
    auto ldSlot = m_srvUavHeap.Allocate();   // slot 7: linear depth UAV (u7)
    m_linearDepthUavCpu = ldSlot.cpu;

    // base-color texture SRVs (RT t3 bindless array)
    m_textureTableGpu = m_outputUavGpu;
    for (UINT i = 0; i < texCount; ++i)
    {
        auto slot = m_srvUavHeap.Allocate();
        if (i == 0) m_textureTableGpu = slot.gpu;

        ID3D12Resource* res = m_textureResources[i];
        if (!res) continue;

        D3D12_RESOURCE_DESC rd = res->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = rd.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = rd.MipLevels;
        device->CreateShaderResourceView(res, &srv, slot.cpu);
    }

    // Denoiser: two contiguous 9-entry tables (ping-pong). Each table is
    //   [accum SRV(t0), geo SRV(t1), albedo SRV(t2), histColor SRV(t3),
    //    histGeo SRV(t4), motion SRV(t5),
    //    output UAV(u0), histColor UAV(u1), histGeo UAV(u2)].
    // Phase 0 reads m_hist*[0] / writes m_hist*[1]; phase 1 the reverse. The
    // actual views are (re)created in CreateOutputResource.
    for (int t = 0; t < 2; ++t)
    {
        for (int i = 0; i < 9; ++i)
        {
            auto slot = m_srvUavHeap.Allocate();
            if (i == 0) m_denoiseTableGpu[t] = slot.gpu; // table base
            m_denoiseCpu[t][i] = slot.cpu;
        }
    }

    // Upscale table (contiguous): source SRV (t0), destination UAV (u0).
    auto srcSrv = m_srvUavHeap.Allocate();
    m_outputSrvCpu = srcSrv.cpu;
    m_upscaleTableGpu = srcSrv.gpu;          // table base
    auto dstUav = m_srvUavHeap.Allocate();
    m_upscaledUavCpu = dstUav.cpu;

    // Tonemap table (contiguous): DLSS HDR result SRV (t0), LDR UAV (u0).
    auto hdrSrv = m_srvUavHeap.Allocate();
    m_dlssColorSrvCpu = hdrSrv.cpu;
    m_tonemapTableGpu = hdrSrv.gpu;          // table base
    auto dstUav2 = m_srvUavHeap.Allocate();
    m_upscaledUav2Cpu = dstUav2.cpu;
    return true;
}

//-----------------------------------------------------------------------------
// CreateOutputResource  -  the images the ray tracer and denoiser write to.
//
//   Everything is allocated at the full output size. A render scale below 1
//   just traces into the top-left sub-rectangle, which avoids reallocating
//   whenever the slider moves and matches how DLSS expects to receive a
//   sub-rect of a fixed-size buffer. m_upscaled receives the full-size result.
//
//   The descriptor slots are owned by BuildDescriptorHeap; here we (re)create
//   the textures and (re)write the views into those fixed slots. Also used on
//   resize and whenever renderScale changes.
//-----------------------------------------------------------------------------
bool RaytracingRenderer::CreateOutputResource(uint32_t width, uint32_t height)
{
    auto* dm = DeviceManager::Instance();
    ID3D12Device* device = dm->GetDevice();
    const DXGI_FORMAT fmt = dm->GetRTVFormat();

    // Buffers are always full size; renderScale picks a sub-rect at draw time.
    m_renderWidth = width;
    m_renderHeight = height;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    m_output.Reset();
    if (FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_output))))
    {
        OutputDebugStringW(L"[RT] output image creation failed\n");
        return false;
    }
    m_output->SetName(L"RT_Output");

    // Output-resolution image: what actually reaches the back buffer. The
    // upscale pass stretches m_output's rendered sub-rect across all of it.
    m_upscaled.Reset();
    if (FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_upscaled))))
    {
        OutputDebugStringW(L"[RT] upscaled image creation failed\n");
        return false;
    }
    m_upscaled->SetName(L"RT_Upscaled");

    // Linear HDR image Ray Reconstruction writes into (it refuses to run on
    // LDR colour), tonemapped into m_upscaled afterwards.
    D3D12_RESOURCE_DESC hd = td;
    hd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    m_dlssColor.Reset();
    if (FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &hd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_dlssColor))))
    {
        OutputDebugStringW(L"[RT] DLSS colour buffer creation failed\n");
        return false;
    }
    m_dlssColor->SetName(L"RT_DlssColor");

    // HDR accumulation image (same size, RGBA32F).
    D3D12_RESOURCE_DESC ad = td;
    ad.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    m_accum.Reset();
    if (FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &ad,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_accum))))
    {
        OutputDebugStringW(L"[RT] accumulation image creation failed\n");
        return false;
    }
    m_accum->SetName(L"RT_Accum");

    // G-buffer image (primary-hit normal.xyz + depth.w).
    D3D12_RESOURCE_DESC gd = ad; // same as accum (RGBA32F, UAV)
    m_geo.Reset();
    if (FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &gd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_geo))))
    {
        OutputDebugStringW(L"[RT] G-buffer creation failed\n");
        return false;
    }
    m_geo->SetName(L"RT_Geo");

    // Albedo image (primary-hit base color, for demodulation).
    m_albedo.Reset();
    if (FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &gd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_albedo))))
    {
        OutputDebugStringW(L"[RT] albedo buffer creation failed\n");
        return false;
    }
    m_albedo->SetName(L"RT_Albedo");

    // Motion vectors (pixel space, prev - cur). RG32F is one of the formats
    // DLSS accepts for kBufferTypeMotionVectors.
    D3D12_RESOURCE_DESC md = td;
    md.Format = DXGI_FORMAT_R32G32_FLOAT;
    m_motion.Reset();
    if (FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &md,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_motion))))
    {
        OutputDebugStringW(L"[RT] motion vector buffer creation failed\n");
        return false;
    }
    m_motion->SetName(L"RT_Motion");

    // DLSS Ray Reconstruction guide buffers. The SDK asks for normals and
    // specular albedo in a 16-bit-float (or wider) linear format and takes
    // linear depth as a single channel.
    D3D12_RESOURCE_DESC nd = td;
    nd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    m_normalRough.Reset();
    if (FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &nd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_normalRough))))
    {
        OutputDebugStringW(L"[RT] normal/roughness buffer creation failed\n");
        return false;
    }
    m_normalRough->SetName(L"RT_NormalRoughness");

    m_specAlbedo.Reset();
    if (FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &nd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_specAlbedo))))
    {
        OutputDebugStringW(L"[RT] specular albedo buffer creation failed\n");
        return false;
    }
    m_specAlbedo->SetName(L"RT_SpecularAlbedo");

    D3D12_RESOURCE_DESC dd = td;
    dd.Format = DXGI_FORMAT_R32_FLOAT;
    m_linearDepth.Reset();
    if (FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &dd,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_linearDepth))))
    {
        OutputDebugStringW(L"[RT] linear depth buffer creation failed\n");
        return false;
    }
    m_linearDepth->SetName(L"RT_LinearDepth");

    // Ping-pong temporal history: color (lighting.rgb + len.a) and geometry
    // (world position.xyz, for geometry-based history rejection).
    for (int i = 0; i < 2; ++i)
    {
        m_histColor[i].Reset();
        if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &gd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_histColor[i]))))
        {
            OutputDebugStringW(L"[RT] history colour buffer creation failed\n");
            return false;
        }
        m_histColor[i]->SetName(i == 0 ? L"RT_HistColor0" : L"RT_HistColor1");

        m_histGeo[i].Reset();
        if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &gd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_histGeo[i]))))
        {
            OutputDebugStringW(L"[RT] history geometry buffer creation failed\n");
            return false;
        }
        m_histGeo[i]->SetName(i == 0 ? L"RT_HistGeo0" : L"RT_HistGeo1");
    }

    // ---- UAV views written by the ray generation shader (u0, u1, u2, u3) ----
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = fmt;
    device->CreateUnorderedAccessView(m_output.Get(), nullptr, &uav, m_outputUavCpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavF = {};
    uavF.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavF.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device->CreateUnorderedAccessView(m_accum.Get(),  nullptr, &uavF, m_accumUavCpu);
    device->CreateUnorderedAccessView(m_geo.Get(),    nullptr, &uavF, m_geoUavCpu);
    device->CreateUnorderedAccessView(m_albedo.Get(), nullptr, &uavF, m_albedoUavCpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavMV = {};
    uavMV.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavMV.Format = DXGI_FORMAT_R32G32_FLOAT;
    device->CreateUnorderedAccessView(m_motion.Get(), nullptr, &uavMV, m_motionUavCpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavH = {};
    uavH.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavH.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateUnorderedAccessView(m_normalRough.Get(), nullptr, &uavH, m_normalRoughUavCpu);
    device->CreateUnorderedAccessView(m_specAlbedo.Get(),  nullptr, &uavH, m_specAlbedoUavCpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavD = {};
    uavD.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavD.Format = DXGI_FORMAT_R32_FLOAT;
    device->CreateUnorderedAccessView(m_linearDepth.Get(), nullptr, &uavD, m_linearDepthUavCpu);

    // ---- denoiser views: two ping-pong tables -----------------------------
    // Each table: accum SRV(t0), geo SRV(t1), albedo SRV(t2), histColor SRV(t3),
    //             histGeo SRV(t4), motion SRV(t5),
    //             output UAV(u0), histColor UAV(u1), histGeo UAV(u2).
    // Table 0 reads m_hist*[0] / writes m_hist*[1]; table 1 the reverse.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvMV = srv;
    srvMV.Format = DXGI_FORMAT_R32G32_FLOAT;

    for (int t = 0; t < 2; ++t)
    {
        ID3D12Resource* colRead  = m_histColor[t].Get();
        ID3D12Resource* colWrite = m_histColor[1 - t].Get();
        ID3D12Resource* geoRead  = m_histGeo[t].Get();
        ID3D12Resource* geoWrite = m_histGeo[1 - t].Get();
        device->CreateShaderResourceView(m_accum.Get(),  &srv, m_denoiseCpu[t][0]);
        device->CreateShaderResourceView(m_geo.Get(),    &srv, m_denoiseCpu[t][1]);
        device->CreateShaderResourceView(m_albedo.Get(), &srv, m_denoiseCpu[t][2]);
        device->CreateShaderResourceView(colRead,        &srv, m_denoiseCpu[t][3]);
        device->CreateShaderResourceView(geoRead,        &srv, m_denoiseCpu[t][4]);
        device->CreateShaderResourceView(m_motion.Get(), &srvMV, m_denoiseCpu[t][5]);
        device->CreateUnorderedAccessView(m_output.Get(), nullptr, &uav,  m_denoiseCpu[t][6]);
        device->CreateUnorderedAccessView(colWrite,       nullptr, &uavF, m_denoiseCpu[t][7]);
        device->CreateUnorderedAccessView(geoWrite,       nullptr, &uavF, m_denoiseCpu[t][8]);
    }

    // ---- upscale views: source SRV (t0), destination UAV (u0) -------------
    D3D12_SHADER_RESOURCE_VIEW_DESC srvOut = {};
    srvOut.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvOut.Format = fmt;
    srvOut.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvOut.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_output.Get(), &srvOut, m_outputSrvCpu);
    device->CreateUnorderedAccessView(m_upscaled.Get(), nullptr, &uav, m_upscaledUavCpu);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvHdr = srvOut;
    srvHdr.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(m_dlssColor.Get(), &srvHdr, m_dlssColorSrvCpu);
    device->CreateUnorderedAccessView(m_upscaled.Get(), nullptr, &uav, m_upscaledUav2Cpu);

    // A new/resized accumulation image must start fresh.
    m_accumIndex = 0;
    m_hasPrevKey = false;
    m_hasPrevVP = false;
    m_histPhase = 0;
    return true;
}

//-----------------------------------------------------------------------------
// Resize
//-----------------------------------------------------------------------------
void RaytracingRenderer::Resize(uint32_t width, uint32_t height)
{
    if (!m_valid || width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;
    CreateOutputResource(width, height);
}

//-----------------------------------------------------------------------------
// Render  -  dispatch rays, denoise, upscale, and copy to the back buffer.
//-----------------------------------------------------------------------------
void RaytracingRenderer::Render(const Camera& camera)
{
    if (!m_valid) return;

    auto* dm = DeviceManager::Instance();
    auto& cmdCtx = dm->GetCommand();
    ID3D12GraphicsCommandList4* cmd4 = cmdCtx.GetCommandList4();
    if (!cmd4) return;

    // Every per-pixel buffer is allocated at the full output size; a lower
    // render scale simply uses the top-left sub-rectangle of them. That keeps
    // the slider free of any reallocation (and matches how DLSS wants to be
    // handed a sub-rect of a fixed-size buffer).
    const float scale = (renderScale < 0.25f) ? 0.25f
                      : (renderScale > 1.0f ? 1.0f : renderScale);
    m_renderWidth = (uint32_t)(m_width * scale);
    m_renderHeight = (uint32_t)(m_height * scale);
    if (m_renderWidth == 0) m_renderWidth = 1;
    if (m_renderHeight == 0) m_renderHeight = 1;

    // DLSS Ray Reconstruction replaces both our denoiser and the upscale, and
    // does its own temporal accumulation - so ours is switched off while it
    // runs and the ray tracer emits a single noisy sample per pixel instead.
    const bool wantDLSS = useDLSS && (renderMode == 1) &&
                          DLSS::Instance().SupportsRayReconstruction();

    // DLSS reconstructs from a fixed resolution per preset, so let it choose
    // rather than the renderScale slider.
    const int qi = (dlssQuality < 0) ? 0 : (dlssQuality > 4 ? 4 : dlssQuality);
    const DLSS::Quality quality = (DLSS::Quality)qi;
    if (wantDLSS)
    {
        uint32_t rw = 0, rh = 0;
        if (DLSS::Instance().GetRenderSize(quality, m_width, m_height, rw, rh))
        {
            m_renderWidth = rw;
            m_renderHeight = rh;
        }
    }

    // ---- scene constants (camera + light) ----------------------------
    SceneConstants sc = {};
    const float aspect = dm->GetScreenWidth() / dm->GetScreenHeight();

    XMVECTOR eye = XMLoadFloat3(&camera.eye);
    XMVECTOR focus = XMLoadFloat3(&camera.focus);
    XMVECTOR up = XMLoadFloat3(&camera.up);
    XMMATRIX view = XMMatrixLookAtLH(eye, focus, up);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(camera.fovDegree), aspect, camera.nearZ, camera.farZ);
    XMMATRIX viewProj = view * proj;
    XMVECTOR det;
    XMMATRIX invVP = XMMatrixInverse(&det, viewProj);
    XMStoreFloat4x4(&sc.invViewProj, invVP);
    XMFLOAT4X4 curViewProj;
    XMStoreFloat4x4(&curViewProj, viewProj); // for the denoiser's reprojection

    // .w carries the far plane so the shader can mark sky pixels at a depth
    // still inside the frustum DLSS was told about.
    sc.cameraPos = XMFLOAT4(camera.eye.x, camera.eye.y, camera.eye.z, camera.farZ);
    sc.lightDir = camera.lightDir;
    // Scale the light colour by the intensity so the ImGui slider affects RT.
    const float li = camera.lightIntensity;
    sc.lightColor = XMFLOAT4(camera.lightColor.x * li, camera.lightColor.y * li,
                             camera.lightColor.z * li, camera.lightColor.w);
    sc.ambient = camera.ambient;
    sc.ambient.w = exposure; // pass exposure to the tonemapper (ambient.rgb unused)
    const int spp = (samplesPerFrame < 1) ? 1 : (samplesPerFrame > 8 ? 8 : samplesPerFrame);
    sc.envParams = XMFLOAT4((float)m_envTexIndex, envIntensity, (float)spp, 0.0f);
    sc.prevViewProj = m_prevViewProj;

    // Under DLSS the whole frame is rendered with one known sub-pixel offset
    // (a Halton 2,3 sequence) which DLSS is told about, so it can resolve edges
    // across frames. Without DLSS each sample keeps its own random jitter.
    XMFLOAT2 jitter(0.0f, 0.0f);
    if (wantDLSS && dlssJitter)
    {
        auto halton = [](uint32_t i, uint32_t base) {
            float f = 1.0f, r = 0.0f;
            while (i > 0) { f /= base; r += f * (i % base); i /= base; }
            return r;
        };
        const uint32_t idx = (m_frameCounter % 16) + 1;
        jitter = XMFLOAT2(halton(idx, 2) - 0.5f, halton(idx, 3) - 0.5f);
    }
    sc.prevParams = XMFLOAT4(m_hasPrevVP ? 1.0f : 0.0f,
                             jitter.x, jitter.y, wantDLSS ? 1.0f : 0.0f);

    // Reset accumulation whenever anything that changes the image changes.
    const int depth = (maxBounces < 1) ? 1 : (maxBounces > 8 ? 8 : maxBounces);
    ResetKey key;
    key.eye = camera.eye; key.focus = camera.focus; key.fov = camera.fovDegree;
    key.lightDir = camera.lightDir; key.lightColor = camera.lightColor;
    key.ambient = camera.ambient; key.intensity = camera.lightIntensity;
    key.envIntensity = envIntensity;
    key.depth = depth; key.mode = renderMode; key.spp = spp;
    key.w = m_renderWidth; key.h = m_renderHeight;
    if (!m_hasPrevKey || memcmp(&key, &m_prevKey, sizeof(ResetKey)) != 0)
    {
        m_accumIndex = 0;
        m_prevKey = key;
        m_hasPrevKey = true;
    }

    // DLSS accumulates over time itself, so ours stays at zero and the ray
    // tracer keeps emitting this frame's raw (noisy) radiance.
    if (wantDLSS) m_accumIndex = 0;

    // frame: x = RNG seed, y = path depth, z = prior sample count, w = mode.
    sc.frame = XMUINT4(m_frameCounter++, (uint32_t)depth, m_accumIndex,
                       (uint32_t)renderMode);
    m_sceneCB.Update(sc);

    // ---- bind and dispatch -------------------------------------------
    ID3D12DescriptorHeap* heaps[] = { m_srvUavHeap.GetHeap() };
    cmd4->SetDescriptorHeaps(1, heaps);
    cmd4->SetComputeRootSignature(m_pipeline.GetGlobalRootSig());
    cmd4->SetComputeRootDescriptorTable(0, m_outputUavGpu);      // u0 output
    cmd4->SetComputeRootShaderResourceView(1, m_tlas.Address()); // t0 TLAS
    cmd4->SetComputeRootConstantBufferView(2, m_sceneCB.GetGpuAddress()); // b0 scene
    cmd4->SetComputeRootDescriptorTable(3, m_textureTableGpu);   // t3 textures
    cmd4->SetPipelineState1(m_pipeline.GetStateObject());

    const D3D12_GPU_VIRTUAL_ADDRESS sbt = m_shaderTable->GetGPUVirtualAddress();
    D3D12_DISPATCH_RAYS_DESC desc = {};
    desc.RayGenerationShaderRecord.StartAddress = sbt + m_raygenOffset;
    desc.RayGenerationShaderRecord.SizeInBytes = m_raygenRegionSize;
    desc.MissShaderTable.StartAddress = sbt + m_missOffset;
    desc.MissShaderTable.SizeInBytes = m_missRegionSize;
    desc.MissShaderTable.StrideInBytes = m_missStride;
    desc.HitGroupTable.StartAddress = sbt + m_hitOffset;
    desc.HitGroupTable.SizeInBytes = m_hitRegionSize;
    desc.HitGroupTable.StrideInBytes = m_hitStride;
    desc.Width = m_renderWidth;   // trace only the rendered sub-rectangle
    desc.Height = m_renderHeight;
    desc.Depth = 1;
    cmd4->DispatchRays(&desc);

    // ---- optional spatial denoise (path-tracing mode only) -----------
    const bool doDenoise = (renderMode == 1) && denoise && m_denoisePSO &&
                           denoiseRadius > 0 && !wantDLSS;
    if (doDenoise)
    {
        const int phase = m_histPhase;   // reads m_hist*[phase], writes m_hist*[1-phase]
        ID3D12Resource* colRead = m_histColor[phase].Get();
        ID3D12Resource* geoRead = m_histGeo[phase].Get();

        DenoiseConstants dc = {};
        dc.dim = XMUINT2(m_renderWidth, m_renderHeight);
        dc.sampleCount = m_accumIndex + 1; // samples now in gAccum
        dc.radius = denoiseRadius;
        dc.exposure = exposure;
        dc.normalPower = 32.0f;
        dc.depthSigma = 0.02f;
        dc.temporalAlpha = 0.1f;           // keep ~90% history once settled
        dc.invViewProj = sc.invViewProj;
        dc.prevViewProj = m_prevViewProj;
        dc.cameraPos = sc.cameraPos;
        dc.temporalValid = m_hasPrevVP ? 1 : 0;
        m_denoiseCB.Update(dc);

        // Order RayGen's output write before the denoiser overwrites it, and
        // make gAccum/gGeo/albedo + the history-read image readable as SRVs.
        D3D12_RESOURCE_BARRIER ob = {};
        ob.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ob.UAV.pResource = m_output.Get();
        cmd4->ResourceBarrier(1, &ob);

        cmdCtx.ResourceBarrier(m_accum.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdCtx.ResourceBarrier(m_geo.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdCtx.ResourceBarrier(m_albedo.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdCtx.ResourceBarrier(colRead,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdCtx.ResourceBarrier(geoRead,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdCtx.ResourceBarrier(m_motion.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        cmd4->SetComputeRootSignature(m_denoiseRootSig.Get());
        cmd4->SetPipelineState(m_denoisePSO.Get());
        cmd4->SetComputeRootDescriptorTable(0, m_denoiseTableGpu[phase]);
        cmd4->SetComputeRootConstantBufferView(1, m_denoiseCB.GetGpuAddress());
        cmd4->Dispatch((m_renderWidth + 7) / 8, (m_renderHeight + 7) / 8, 1);

        cmdCtx.ResourceBarrier(m_accum.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdCtx.ResourceBarrier(m_geo.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdCtx.ResourceBarrier(m_albedo.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdCtx.ResourceBarrier(colRead,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdCtx.ResourceBarrier(geoRead,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdCtx.ResourceBarrier(m_motion.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        m_histPhase = 1 - m_histPhase;
    }

    // ---- resolve to the full output: DLSS, or the bilinear fallback --
    m_dlssActive = false;
    if (wantDLSS)
    {
        // Order the ray tracer's writes before DLSS reads them.
        ID3D12Resource* uavs[] = {
            m_accum.Get(), m_albedo.Get(), m_specAlbedo.Get(),
            m_normalRough.Get(), m_linearDepth.Get(), m_motion.Get()
        };
        for (ID3D12Resource* r : uavs)
        {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = r;
            cmd4->ResourceBarrier(1, &b);
        }

        DLSS::RRFrame f;
        // With accumulation forced off, m_accum holds this frame's raw linear
        // HDR radiance - exactly what Ray Reconstruction wants.
        f.colorIn = m_accum.Get();           // noisy linear HDR, render res
        f.colorOut = m_dlssColor.Get();      // denoised + upscaled, still HDR
        f.diffuseAlbedo = m_albedo.Get();
        f.specularAlbedo = m_specAlbedo.Get();
        f.normalRoughness = m_normalRough.Get();
        f.linearDepth = m_linearDepth.Get();
        f.motionVectors = m_motion.Get();
        f.renderWidth = m_renderWidth;
        f.renderHeight = m_renderHeight;
        f.outputWidth = m_width;
        f.outputHeight = m_height;

        XMStoreFloat4x4(&f.viewToClip, proj);
        XMStoreFloat4x4(&f.clipToView, XMMatrixInverse(&det, proj));
        XMStoreFloat4x4(&f.worldToView, view);
        XMStoreFloat4x4(&f.viewToWorld, XMMatrixInverse(&det, view));

        // Row-vector convention: a current-clip point goes back to world with
        // invVP, then forward through the previous frame's view-projection.
        XMMATRIX prevVP = XMLoadFloat4x4(&m_prevViewProj);
        if (m_hasPrevVP)
        {
            XMStoreFloat4x4(&f.clipToPrevClip, XMMatrixMultiply(invVP, prevVP));
            XMStoreFloat4x4(&f.prevClipToClip,
                XMMatrixMultiply(XMMatrixInverse(&det, prevVP), viewProj));
        }
        else
        {
            XMStoreFloat4x4(&f.clipToPrevClip, XMMatrixIdentity());
            XMStoreFloat4x4(&f.prevClipToClip, XMMatrixIdentity());
        }

        f.cameraPos = camera.eye;
        XMVECTOR fwd = XMVector3Normalize(XMVectorSubtract(focus, eye));
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, fwd));
        XMVECTOR realUp = XMVector3Cross(fwd, right);
        XMStoreFloat3(&f.cameraFwd, fwd);
        XMStoreFloat3(&f.cameraRight, right);
        XMStoreFloat3(&f.cameraUp, realUp);
        f.nearZ = camera.nearZ;
        f.farZ = camera.farZ;
        f.fovY = XMConvertToRadians(camera.fovDegree);
        f.aspect = aspect;
        // DLSS's jitter offset runs opposite to the offset we apply to the ray:
        // established by testing all four sign combinations, only the fully
        // negated one holds a static image still. The ray tracer keeps
        // rendering with `jitter` itself.
        f.jitter = XMFLOAT2(-jitter.x, -jitter.y);
        f.reset = !m_hasPrevVP;
        f.quality = quality;

        m_dlssActive = DLSS::Instance().EvaluateRR(cmd4, f);

        // Streamline binds its own descriptor heaps and does not restore ours,
        // so anything we dispatch afterwards must re-bind them.
        cmd4->SetDescriptorHeaps(1, heaps);
    }

    // Resolve into the LDR back-buffer image: either tonemap DLSS's HDR result
    // (already at output resolution) or bilinearly stretch our own LDR image.
    {
        ID3D12Resource* src = m_dlssActive ? m_dlssColor.Get() : m_output.Get();

        UpscaleConstants uc = {};
        uc.dstDim = XMUINT2(m_width, m_height);
        uc.srcDim = m_dlssActive ? XMUINT2(m_width, m_height)
                                 : XMUINT2(m_renderWidth, m_renderHeight);
        uc.texDim = XMUINT2(m_width, m_height); // buffers are always full size
        uc.tonemap = m_dlssActive ? 1u : 0u;    // DLSS returns linear HDR
        uc.exposure = exposure;
        m_upscaleCB.Update(uc);

        // Finish writing the source before reading it as a texture.
        D3D12_RESOURCE_BARRIER ob = {};
        ob.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ob.UAV.pResource = src;
        cmd4->ResourceBarrier(1, &ob);

        cmdCtx.ResourceBarrier(src,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        cmd4->SetComputeRootSignature(m_upscaleRootSig.Get());
        cmd4->SetPipelineState(m_upscalePSO.Get());
        cmd4->SetComputeRootDescriptorTable(
            0, m_dlssActive ? m_tonemapTableGpu : m_upscaleTableGpu);
        cmd4->SetComputeRootConstantBufferView(1, m_upscaleCB.GetGpuAddress());
        cmd4->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

        cmdCtx.ResourceBarrier(src,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // ---- copy the full-size image into the back buffer ---------------
    ID3D12Resource* backbuffer = dm->GetSwapChain().GetCurrentBackBuffer();
    cmdCtx.ResourceBarrier(m_upscaled.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdCtx.ResourceBarrier(backbuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);

    cmd4->CopyResource(backbuffer, m_upscaled.Get());

    cmdCtx.ResourceBarrier(backbuffer,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdCtx.ResourceBarrier(m_upscaled.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Remember this frame's camera for next frame's motion vectors. This must
    // happen every frame, not just when the denoiser runs.
    m_prevViewProj = curViewProj;
    m_hasPrevVP = true;

    // One more sample has been accumulated this frame.
    ++m_accumIndex;
}
