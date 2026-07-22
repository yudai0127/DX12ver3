#pragma once
#define NOMINMAX
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include "RHI/RaytracingPipeline.h"
#include "RHI/RaytracingAccel.h"
#include "RHI/DescriptorHeap.h"
#include "RHI/ConstantBuffer.h"

using Microsoft::WRL::ComPtr;

class Scene;
class Camera;

//-----------------------------------------------------------------------------
// RaytracingRenderer  (DXR milestone 1)
//  Renders the scene's glTF models with hardware ray tracing:
//    primary rays through the camera + hard shadows from the directional light.
//
//  Pipeline:
//    Initialize(scene) : compile the RT library, build the state object,
//                        build BLAS/TLAS from all ModelRenderer geometry,
//                        build the shader binding table, create the output image.
//    Render(cmd4, cam) : dispatch rays into the output image and copy it into
//                        the current back buffer.
//
//  This runs alongside the existing rasterizer; Framework chooses which path to
//  use each frame. It requires Device::SupportsRaytracing() == true.
//-----------------------------------------------------------------------------
class RaytracingRenderer
{
public:
    RaytracingRenderer() = default;
    ~RaytracingRenderer() = default;

    RaytracingRenderer(const RaytracingRenderer&) = delete;
    RaytracingRenderer& operator=(const RaytracingRenderer&) = delete;

    // Build everything from the scene. Returns false if DXR is unavailable or a
    // build step fails (in which case IsValid() stays false and the app should
    // fall back to the rasterizer).
    bool Initialize(Scene* scene, uint32_t width, uint32_t height);

    // Trace one frame and copy the result into the current back buffer.
    // The back buffer must be in RENDER_TARGET state on entry (as left by
    // Framework::BeginFrame); it is returned in RENDER_TARGET state.
    void Render(const Camera& camera);

    // Recreate the output image after a window resize.
    void Resize(uint32_t width, uint32_t height);

    bool IsValid() const { return m_valid; }
    size_t GetInstanceCount() const { return m_hitData.size(); }

private:
    // Scene constants passed to the ray generation / shading shaders (b0).
    struct SceneConstants
    {
        DirectX::XMFLOAT4X4 invViewProj;
        DirectX::XMFLOAT4   cameraPos;
        DirectX::XMFLOAT4   lightDir;
        DirectX::XMFLOAT4   lightColor;
        DirectX::XMFLOAT4   ambient;
        DirectX::XMUINT4    frame;
    };

    // Per-hit-record local root arguments (must match the local root signature
    // order: vertex SRV, index SRV, base-color constants + texture index).
    struct HitRecordData
    {
        D3D12_GPU_VIRTUAL_ADDRESS vbAddress = 0;
        D3D12_GPU_VIRTUAL_ADDRESS ibAddress = 0;
        DirectX::XMFLOAT4         baseColor = { 0.8f, 0.8f, 0.8f, 1.0f };
        int                       baseColorTex = -1; // index into m_textureResources, or -1
    };

    bool BuildPipeline();
    bool BuildAccelerationStructures(Scene* scene);
    bool BuildDescriptorHeap();       // output UAV + bindless base-color textures
    bool BuildShaderTable();
    bool CreateOutputResource(uint32_t width, uint32_t height);

    bool     m_valid = false;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_frameCounter = 0;

    RaytracingPipeline m_pipeline;

    // Acceleration structures (kept alive while rendering).
    std::vector<RaytracingAccel::AccelBuffers> m_blas;
    RaytracingAccel::AccelBuffers              m_tlas;
    std::vector<HitRecordData>                 m_hitData;

    // Shader binding table (one upload buffer, mapped).
    ComPtr<ID3D12Resource> m_shaderTable;
    UINT m_raygenRegionSize = 0;
    UINT m_missRegionSize = 0;
    UINT m_missStride = 0;
    UINT m_hitRegionSize = 0;
    UINT m_hitStride = 0;
    UINT64 m_raygenOffset = 0;
    UINT64 m_missOffset = 0;
    UINT64 m_hitOffset = 0;

    // Output image (UAV) + bindless textures share one shader-visible heap:
    //   slot 0      : output UAV (u0)
    //   slots 1..T  : base-color texture SRVs (t3 array)
    ComPtr<ID3D12Resource>          m_output;
    DescriptorHeap                  m_srvUavHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE     m_outputUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE     m_outputUavGpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE     m_textureTableGpu = {};
    std::vector<ID3D12Resource*>    m_textureResources; // all models' textures, in order

    ConstantBuffer m_sceneCB;
};
