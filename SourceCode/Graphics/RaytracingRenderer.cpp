#include "Graphics/RaytracingRenderer.h"
#include "Graphics/ShaderManager.h"
#include "Graphics/GltfModel.h"
#include "RHI/DeviceManager.h"
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
    if (!m_sceneCB.Initialize(sizeof(SceneConstants))) return false;
    if (!m_denoiseCB.Initialize(sizeof(DenoiseConstants))) return false;
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
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; // t0-t4: accum, geo, albedo, histColor, histGeo
    ranges[0].NumDescriptors = 5;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; // u0-u2: output, histColorOut, histGeoOut
    ranges[1].NumDescriptors = 3;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 5;

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
    // Layout: 4 UAVs (output u0, accum u1, geo u2, albedo u3) + texture SRVs
    // + 16 denoiser descriptors (two 8-entry ping-pong tables).
    if (!m_srvUavHeap.Initialize(device, 20 + texCount, /*shaderVisible*/ true))
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

    // Denoiser: two contiguous 8-entry tables (ping-pong). Each table is
    //   [accum SRV(t0), geo SRV(t1), albedo SRV(t2), histColor SRV(t3),
    //    histGeo SRV(t4), output UAV(u0), histColor UAV(u1), histGeo UAV(u2)].
    // Phase 0 reads m_hist*[0] / writes m_hist*[1]; phase 1 the reverse. The
    // actual views are (re)created in CreateOutputResource.
    for (int t = 0; t < 2; ++t)
    {
        for (int i = 0; i < 8; ++i)
        {
            auto slot = m_srvUavHeap.Allocate();
            if (i == 0) m_denoiseTableGpu[t] = slot.gpu; // table base
            m_denoiseCpu[t][i] = slot.cpu;
        }
    }
    return true;
}

//-----------------------------------------------------------------------------
// CreateOutputResource  -  UAV image the ray generation shader writes to.
//   Format matches the back buffer so it can be CopyResource'd directly.
//   The descriptor slot is owned by BuildDescriptorHeap; here we (re)create the
//   texture and (re)write the UAV into that fixed slot (also used on resize).
//-----------------------------------------------------------------------------
bool RaytracingRenderer::CreateOutputResource(uint32_t width, uint32_t height)
{
    auto* dm = DeviceManager::Instance();
    ID3D12Device* device = dm->GetDevice();
    const DXGI_FORMAT fmt = dm->GetRTVFormat();

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

    // ---- denoiser views: two ping-pong tables -----------------------------
    // Each table: accum SRV(t0), geo SRV(t1), albedo SRV(t2), histColor SRV(t3),
    //             histGeo SRV(t4), output UAV(u0), histColor UAV(u1), histGeo UAV(u2).
    // Table 0 reads m_hist*[0] / writes m_hist*[1]; table 1 the reverse.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
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
        device->CreateUnorderedAccessView(m_output.Get(), nullptr, &uav,  m_denoiseCpu[t][5]);
        device->CreateUnorderedAccessView(colWrite,       nullptr, &uavF, m_denoiseCpu[t][6]);
        device->CreateUnorderedAccessView(geoWrite,       nullptr, &uavF, m_denoiseCpu[t][7]);
    }

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
// Render  -  dispatch rays and copy the result into the current back buffer.
//-----------------------------------------------------------------------------
void RaytracingRenderer::Render(const Camera& camera)
{
    if (!m_valid) return;

    auto* dm = DeviceManager::Instance();
    auto& cmdCtx = dm->GetCommand();
    ID3D12GraphicsCommandList4* cmd4 = cmdCtx.GetCommandList4();
    if (!cmd4) return;

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

    sc.cameraPos = XMFLOAT4(camera.eye.x, camera.eye.y, camera.eye.z, 1.0f);
    sc.lightDir = camera.lightDir;
    // Scale the light colour by the intensity so the ImGui slider affects RT.
    const float li = camera.lightIntensity;
    sc.lightColor = XMFLOAT4(camera.lightColor.x * li, camera.lightColor.y * li,
                             camera.lightColor.z * li, camera.lightColor.w);
    sc.ambient = camera.ambient;
    sc.ambient.w = exposure; // pass exposure to the tonemapper (ambient.rgb unused)
    const int spp = (samplesPerFrame < 1) ? 1 : (samplesPerFrame > 8 ? 8 : samplesPerFrame);
    sc.envParams = XMFLOAT4((float)m_envTexIndex, envIntensity, (float)spp, 0.0f);

    // Reset accumulation whenever anything that changes the image changes.
    const int depth = (maxBounces < 1) ? 1 : (maxBounces > 8 ? 8 : maxBounces);
    ResetKey key;
    key.eye = camera.eye; key.focus = camera.focus; key.fov = camera.fovDegree;
    key.lightDir = camera.lightDir; key.lightColor = camera.lightColor;
    key.ambient = camera.ambient; key.intensity = camera.lightIntensity;
    key.envIntensity = envIntensity;
    key.depth = depth; key.mode = renderMode; key.w = m_width; key.h = m_height;
    if (!m_hasPrevKey || memcmp(&key, &m_prevKey, sizeof(ResetKey)) != 0)
    {
        m_accumIndex = 0;
        m_prevKey = key;
        m_hasPrevKey = true;
    }

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
    desc.Width = m_width;
    desc.Height = m_height;
    desc.Depth = 1;
    cmd4->DispatchRays(&desc);

    // ---- optional spatial denoise (path-tracing mode only) -----------
    const bool doDenoise = (renderMode == 1) && denoise && m_denoisePSO &&
                           denoiseRadius > 0;
    if (doDenoise)
    {
        const int phase = m_histPhase;   // reads m_hist*[phase], writes m_hist*[1-phase]
        ID3D12Resource* colRead = m_histColor[phase].Get();
        ID3D12Resource* geoRead = m_histGeo[phase].Get();

        DenoiseConstants dc = {};
        dc.dim = XMUINT2(m_width, m_height);
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

        cmd4->SetComputeRootSignature(m_denoiseRootSig.Get());
        cmd4->SetPipelineState(m_denoisePSO.Get());
        cmd4->SetComputeRootDescriptorTable(0, m_denoiseTableGpu[phase]);
        cmd4->SetComputeRootConstantBufferView(1, m_denoiseCB.GetGpuAddress());
        cmd4->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

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

        // Advance temporal state for next frame.
        m_prevViewProj = curViewProj;
        m_hasPrevVP = true;
        m_histPhase = 1 - m_histPhase;
    }

    // ---- copy output image into the back buffer ----------------------
    ID3D12Resource* backbuffer = dm->GetSwapChain().GetCurrentBackBuffer();
    cmdCtx.ResourceBarrier(m_output.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdCtx.ResourceBarrier(backbuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);

    cmd4->CopyResource(backbuffer, m_output.Get());

    cmdCtx.ResourceBarrier(backbuffer,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdCtx.ResourceBarrier(m_output.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // One more sample has been accumulated this frame.
    ++m_accumIndex;
}
