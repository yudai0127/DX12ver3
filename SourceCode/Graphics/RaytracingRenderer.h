#pragma once
#define NOMINMAX
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <memory>
#include "RHI/RaytracingPipeline.h"
#include "RHI/RaytracingAccel.h"
#include "RHI/DescriptorHeap.h"
#include "RHI/ConstantBuffer.h"
#include "Graphics/Texture.h"

using Microsoft::WRL::ComPtr;

class Scene;
class Camera;
class ModelRenderer;

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

    // Render mode: 0 = raytracing (realtime, deterministic, no accumulation),
    // 1 = path tracing (accumulating global illumination). Set by the app.
    int renderMode = 0;

    // Path length (number of bounces). 1 = direct lighting only; higher adds
    // global illumination. Clamped to [1, 8]. Adjustable from the UI.
    int maxBounces = 3;

    // Tonemap exposure multiplier (post-process; does not reset accumulation).
    float exposure = 0.6f;

    // Environment map brightness (0 = use the environment as-is is x1).
    float envIntensity = 1.0f;

    // Spatial denoiser (path-tracing mode only). radius 0 = off.
    bool denoise = true;
    int  denoiseRadius = 2;

    // Monte-Carlo samples traced per pixel per frame (path tracing). Higher =
    // less noise while moving, at a proportional GPU cost. Clamped to [1, 8].
    int  samplesPerFrame = 2;

    // Fraction of the output resolution the rays are actually traced at.
    // 1.0 = native; 0.5 quarters the pixel count (and roughly the cost) and
    // the image is scaled back up to the window size afterwards. This is the
    // knob DLSS upscaling is built on. Clamped to [0.25, 1.0].
    float renderScale = 1.0f;

    // Use DLSS Ray Reconstruction instead of the built-in denoiser + bilinear
    // upscale. Ignored when Streamline reports Ray Reconstruction unavailable.
    bool useDLSS = false;

    // DLSS preset. While DLSS is on this replaces renderScale: DLSS dictates
    // the render resolution for the chosen preset, and feeding it anything else
    // both costs performance and stops it cancelling the sub-pixel jitter.
    // 0 = Ultra Performance ... 4 = DLAA (native resolution).
    int dlssQuality = 2; // Balanced

    // Temporal antialiasing for raytracing mode. That mode renders with no
    // antialiasing at all, which shows up as sparkling specular highlights,
    // and DLSS costs far more than the mode takes to render. Ignored while
    // DLSS is on (it does this itself) and in path tracing (the denoiser does).
    bool taa = true;

    // True when DLSS actually ran on the last frame (the UI reports this, so a
    // silent fallback to the built-in path is visible).
    bool WasDLSSActive() const { return m_dlssActive; }

    // Resolution rays are traced at this frame (output res * renderScale).
    uint32_t GetRenderWidth() const { return m_renderWidth; }
    uint32_t GetRenderHeight() const { return m_renderHeight; }

    // Number of accumulated samples-per-pixel since the last reset (for the UI).
    uint32_t GetAccumulatedSamples() const { return m_accumIndex; }

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
        DirectX::XMFLOAT4   envParams; // x = env tex index (-1 = none), y = intensity, z = spp
        DirectX::XMFLOAT4X4 prevViewProj; // previous frame (motion vectors)
        DirectX::XMFLOAT4   camRight;     // xyz = right, w = tanHalfFov * aspect
        DirectX::XMFLOAT4   camUp;        // xyz = up,    w = tanHalfFov
        DirectX::XMFLOAT4   camFwd;       // xyz = forward
        DirectX::XMFLOAT4   prevParams;   // x = 1 when prevViewProj is valid
    };

    // Per-hit-record local root arguments (must match the local root signature
    // order: vertex SRV, index SRV, base-color constants + texture index).
    struct HitRecordData
    {
        D3D12_GPU_VIRTUAL_ADDRESS vbAddress = 0;
        D3D12_GPU_VIRTUAL_ADDRESS ibAddress = 0;
        // PBR material (matches the b1 constant layout in PathTrace.hlsl).
        DirectX::XMFLOAT4         baseColor = { 0.8f, 0.8f, 0.8f, 1.0f };
        DirectX::XMFLOAT3         emissive = { 0.0f, 0.0f, 0.0f };
        int                       baseColorTex = -1; // index into m_textureResources, or -1
        float                     metallic = 1.0f;
        float                     roughness = 1.0f;
        int                       mrTex = -1;
        int                       normalTex = -1;
        int                       emissiveTex = -1;
        float                     normalScale = 1.0f;
    };

    bool BuildPipeline();
    bool BuildDenoisePipeline();      // compute root sig + PSO for the denoiser
    bool BuildUpscalePipeline();      // compute root sig + PSO for the upscaler
    bool BuildAccelerationStructures(Scene* scene);
    bool BuildDescriptorHeap();       // output UAV + bindless base-color textures
    bool BuildShaderTable();
    bool CreateOutputResource(uint32_t width, uint32_t height);

    bool     m_valid = false;
    uint32_t m_width = 0;         // output (window) resolution
    uint32_t m_height = 0;
    uint32_t m_renderWidth = 0;   // resolution rays are traced at
    uint32_t m_renderHeight = 0;
    uint32_t m_frameCounter = 0;  // ever-increasing; drives the per-frame RNG
    uint32_t m_accumIndex = 0;    // samples accumulated since the last reset

    // Snapshot of the inputs that, when changed, must reset accumulation.
    struct ResetKey
    {
        DirectX::XMFLOAT3 eye = {}, focus = {};
        float             fov = 0.0f;
        DirectX::XMFLOAT4 lightDir = {}, lightColor = {}, ambient = {};
        float             intensity = 0.0f;
        float             envIntensity = 0.0f;
        int               depth = 0;
        int               mode = 0;
        int               spp = 0;
        uint32_t          w = 0, h = 0;
    } m_prevKey;
    bool m_hasPrevKey = false;

    RaytracingPipeline m_pipeline;

    // Acceleration structures (kept alive while rendering).
    std::vector<RaytracingAccel::AccelBuffers> m_blas;
    RaytracingAccel::AccelBuffers              m_tlas;
    std::vector<HitRecordData>                 m_hitData;

    // What each TLAS instance is placed by, so its transform can be recomputed
    // when the object moves. The geometry itself never changes, so only the
    // top level has to be rebuilt - the BLAS stay as they are.
    struct InstanceSource
    {
        ModelRenderer*      renderer = nullptr; // null for the procedural floor
        DirectX::XMFLOAT4X4 nodeTransform{};    // node transform inside the model
    };
    std::vector<InstanceSource>                      m_instanceSrc;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC>      m_instanceDescs;
    std::vector<DirectX::XMFLOAT4X4>                 m_prevWorld; // change detection

    // True when an object's transform differs from the one baked into the TLAS.
    bool InstancesNeedRebuild() const;

    // Procedural reflective floor geometry (kept alive for BLAS use).
    ComPtr<ID3D12Resource> m_floorVB;
    ComPtr<ID3D12Resource> m_floorIB;

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
    ComPtr<ID3D12Resource>          m_output;   // LDR (back-buffer format), u0
    ComPtr<ID3D12Resource>          m_accum;    // HDR accumulation (RGBA32F), u1
    ComPtr<ID3D12Resource>          m_geo;      // G-buffer normal+depth (RGBA32F), u2
    ComPtr<ID3D12Resource>          m_albedo;   // primary-hit base color (RGBA32F), u3
    ComPtr<ID3D12Resource>          m_motion;   // pixel-space motion vectors (RG32F), u4

    // Guide buffers for DLSS Ray Reconstruction (written by the ray tracer,
    // consumed by DLSS; our own denoiser does not read them).
    ComPtr<ID3D12Resource>          m_normalRough;  // normal.xyz + roughness (RGBA16F), u5
    ComPtr<ID3D12Resource>          m_specAlbedo;   // specular albedo F0 (RGBA16F), u6
    ComPtr<ID3D12Resource>          m_linearDepth;  // primary-hit distance (R32F), u7
    DescriptorHeap                  m_srvUavHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE     m_outputUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE     m_outputUavGpu = {}; // UAV table base (u0..u4)
    D3D12_CPU_DESCRIPTOR_HANDLE     m_accumUavCpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE     m_geoUavCpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE     m_albedoUavCpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE     m_motionUavCpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE     m_normalRoughUavCpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE     m_specAlbedoUavCpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE     m_linearDepthUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE     m_textureTableGpu = {};

    // Denoiser compute pass. Two descriptor tables (one per ping-pong phase),
    // each laid out as: accum SRV(t0), geo SRV(t1), albedo SRV(t2),
    // history SRV(t3), output UAV(u0), history UAV(u1).
    ComPtr<ID3D12RootSignature> m_denoiseRootSig;
    ComPtr<ID3D12PipelineState> m_denoisePSO;
    // Raytracing mode's temporal resolve. Same bindings and constants as the
    // denoiser, so it shares the root signature and descriptor tables above.
    ComPtr<ID3D12PipelineState> m_taaPSO;
    ConstantBuffer              m_denoiseCB;
    D3D12_GPU_DESCRIPTOR_HANDLE m_denoiseTableGpu[2] = {};   // table base per phase
    D3D12_CPU_DESCRIPTOR_HANDLE m_denoiseCpu[2][9] = {};     // views to (re)create

    // Ping-pong temporal history:
    //   m_histColor : demodulated lighting.rgb + history length.a
    //   m_histGeo   : world position.xyz (for geometry-based history rejection)
    ComPtr<ID3D12Resource>      m_histColor[2];
    ComPtr<ID3D12Resource>      m_histGeo[2];
    int                         m_histPhase = 0;             // 0/1, toggled per frame
    DirectX::XMFLOAT4X4         m_prevViewProj = {};
    bool                        m_hasPrevVP = false;
    // Raytracing and path tracing produce very different images, so DLSS's
    // history has to be dropped when the mode changes or it ghosts across.
    int                         m_prevRenderMode = -1;
    bool                        m_dlssActive = false; // DLSS ran last frame

    // Cached DLSS::GetRenderSize answer. slDLSSDGetOptimalSettings is a
    // Streamline round-trip, so it is re-asked only when the preset or the
    // output size changes, not per frame.
    int      m_dlssSizeQuality = -1;
    uint32_t m_dlssSizeOutW = 0, m_dlssSizeOutH = 0;
    uint32_t m_dlssRenderW = 0, m_dlssRenderH = 0;
    bool     m_dlssSizeValid = false;

    // Upscale pass: render-resolution image -> output-resolution image.
    // DLSS takes this job over once it is wired up; this is the fallback.
    ComPtr<ID3D12Resource>      m_upscaled;   // output-resolution LDR image
    ComPtr<ID3D12RootSignature> m_upscaleRootSig;
    ComPtr<ID3D12PipelineState> m_upscalePSO;
    ConstantBuffer              m_upscaleCB;
    D3D12_GPU_DESCRIPTOR_HANDLE m_upscaleTableGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_outputSrvCpu = {};   // t0: render-res image
    D3D12_CPU_DESCRIPTOR_HANDLE m_upscaledUavCpu = {}; // u0: output-res image

    // Ray Reconstruction only accepts linear HDR colour, so it writes here and
    // the upscale pass then tonemaps this into m_upscaled. A second table is
    // needed because the source texture differs from the fallback path's.
    ComPtr<ID3D12Resource>      m_dlssColor;  // output-resolution HDR (RGBA16F)
    D3D12_GPU_DESCRIPTOR_HANDLE m_tonemapTableGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_dlssColorSrvCpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_upscaledUav2Cpu = {};

    std::vector<ID3D12Resource*>    m_textureResources; // all models' textures, in order

    // Equirectangular environment map (optional). Appended to the texture
    // array; m_envTexIndex is its index there (-1 = none, use procedural sky).
    std::unique_ptr<Texture>        m_envTexture;
    int                             m_envTexIndex = -1;

    ConstantBuffer m_sceneCB;
};
