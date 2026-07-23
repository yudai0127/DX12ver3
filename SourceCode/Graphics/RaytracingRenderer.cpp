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
    if (!m_sceneCB.Initialize(sizeof(SceneConstants))) return false;
    // AS build also collects the per-model texture list used by the heap below.
    if (!BuildAccelerationStructures(scene))  return false;
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
                    hd.metallic = mat.metallic_factor;
                    hd.roughness = mat.roughness_factor;
                    hd.normalScale = mat.normal_scale;
                    // Texture indices reference this model's textures; offset
                    // them into the shared bindless array (-1 stays -1).
                    auto remap = [texBase](int idx) { return idx >= 0 ? texBase + idx : -1; };
                    hd.baseColorTex = remap(mat.basecolor_texture);
                    hd.mrTex        = remap(mat.metallic_roughness_texture);
                    hd.normalTex    = remap(mat.normal_texture);
                }
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
    // hit record: shaderId + vbVA(8) + ibVA(8) + b1 constants(40)
    //   b1 = baseColor(16) + texIdx(4) + metallic(4) + roughness(4)
    //        + mrTex(4) + normalTex(4) + normalScale(4)
    m_hitStride  = (UINT)Align(kShaderIdSize + 8 + 8 + 40, kRecordAlign);        // 96

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
        memcpy(cb + 0,  &m_hitData[i].baseColor,    sizeof(XMFLOAT4)); // 16
        memcpy(cb + 16, &m_hitData[i].baseColorTex, sizeof(int));      // 4
        memcpy(cb + 20, &m_hitData[i].metallic,     sizeof(float));    // 4
        memcpy(cb + 24, &m_hitData[i].roughness,    sizeof(float));    // 4
        memcpy(cb + 28, &m_hitData[i].mrTex,        sizeof(int));      // 4
        memcpy(cb + 32, &m_hitData[i].normalTex,    sizeof(int));      // 4
        memcpy(cb + 36, &m_hitData[i].normalScale,  sizeof(float));    // 4
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
    if (!m_srvUavHeap.Initialize(device, 1 + texCount, /*shaderVisible*/ true))
    {
        OutputDebugStringW(L"[RT] descriptor heap init failed\n");
        return false;
    }

    // slot 0: reserve for the output UAV (view created in CreateOutputResource).
    auto uavSlot = m_srvUavHeap.Allocate();
    m_outputUavCpu = uavSlot.cpu;
    m_outputUavGpu = uavSlot.gpu;

    // slots 1..T: base-color texture SRVs. The texture table starts here even
    // when there are no textures (the handle is then never dereferenced).
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

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = fmt;
    device->CreateUnorderedAccessView(m_output.Get(), nullptr, &uav, m_outputUavCpu);
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

    sc.cameraPos = XMFLOAT4(camera.eye.x, camera.eye.y, camera.eye.z, 1.0f);
    sc.lightDir = camera.lightDir;
    // Scale the light colour by the intensity so the ImGui slider affects RT.
    const float li = camera.lightIntensity;
    sc.lightColor = XMFLOAT4(camera.lightColor.x * li, camera.lightColor.y * li,
                             camera.lightColor.z * li, camera.lightColor.w);
    sc.ambient = camera.ambient;
    // frame.x = frame counter, frame.y = reflection bounce count.
    // Clamp to the pipeline's recursion budget (kMaxRecursion = 5 -> max 3).
    const uint32_t bounces = (uint32_t)((maxBounces < 0) ? 0 : (maxBounces > 3 ? 3 : maxBounces));
    sc.frame = XMUINT4(m_frameCounter++, bounces, 0, 0);
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
}
