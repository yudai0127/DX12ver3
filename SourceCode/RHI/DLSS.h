#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>
#include <string>

//-----------------------------------------------------------------------------
// DLSS  -  thin wrapper around the NVIDIA Streamline SDK.
//
//  Streamline is optional: when the SDK headers are not on the include path the
//  whole class compiles down to "unavailable" stubs, so the project always
//  builds. Enable it by adding the SDK's include/ and lib/x64 folders to the
//  project and linking sl.interposer.lib.
//
//  Call order (Streamline must be initialised *before* the D3D12 device):
//      Startup()            -- slInit, before D3D12CreateDevice
//      SetDevice(device)    -- slSetD3DDevice, right after the device exists
//      QuerySupport()       -- ask the driver about DLSS / Ray Reconstruction
//      Shutdown()           -- slShutdown, at app exit
//
//  This first stage only establishes the connection and reports capability;
//  upscaling and denoising are wired up on top of it.
//-----------------------------------------------------------------------------
class DLSS
{
public:
    static DLSS& Instance();

    bool Startup();
    void SetDevice(ID3D12Device* device);
    void QuerySupport();
    void Shutdown();

    // True when the project was built with the Streamline SDK available.
    static bool IsCompiledIn();

    bool IsInitialized() const { return m_initialized; }
    bool SupportsUpscaling() const { return m_srSupported; }          // DLSS-SR
    bool SupportsRayReconstruction() const { return m_rrSupported; }  // DLSS-RR

    // Human-readable state for the UI / debug output.
    const char* StatusText() const { return m_status.c_str(); }

    //-------------------------------------------------------------------
    // Interposer proxies
    //
    //  DLSS only works on a device Streamline itself handed out. We keep
    //  linking the normal d3d12.lib / dxgi.lib (the engine needs entry points
    //  such as D3D12SerializeRootSignature that the interposer does not
    //  export) and instead load sl.interposer.dll dynamically, routing just
    //  the factory and device creation through it - the "load the interposer
    //  dynamically and redirect the calls you need" path from the SDK docs.
    //
    //  Both helpers fall back to the real D3D12/DXGI functions when
    //  Streamline is absent, so behaviour is unchanged without the SDK.
    //-------------------------------------------------------------------
    HRESULT CreateFactory(UINT flags, REFIID riid, void** out);
    HRESULT CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minLevel,
                         REFIID riid, void** out);

    //-------------------------------------------------------------------
    // DLSS Ray Reconstruction
    //-------------------------------------------------------------------

    // Quality presets, in increasing order of render resolution (and cost).
    // DLAA renders at the output resolution and only denoises / anti-aliases.
    enum class Quality { UltraPerformance, Performance, Balanced, Quality, DLAA };

    //  Everything the AI denoiser needs for one frame. Deliberately built
    //  from plain D3D12/DirectXMath types so callers never have to include
    //  the Streamline headers.
    //
    //  All the render-resolution buffers are the top-left renderWidth x
    //  renderHeight sub-rectangle of full-size textures; colorOut is the
    //  full output resolution. Every buffer is expected in the
    //  UNORDERED_ACCESS state.
    //-------------------------------------------------------------------
    struct RRFrame
    {
        // Buffers
        ID3D12Resource* colorIn = nullptr;        // noisy tonemapped colour
        ID3D12Resource* colorOut = nullptr;       // upscaled, denoised result
        ID3D12Resource* diffuseAlbedo = nullptr;
        ID3D12Resource* specularAlbedo = nullptr;
        ID3D12Resource* normalRoughness = nullptr; // normal.xyz + roughness.w
        ID3D12Resource* linearDepth = nullptr;
        ID3D12Resource* motionVectors = nullptr;   // pixel space, prev - cur

        uint32_t renderWidth = 0, renderHeight = 0;
        uint32_t outputWidth = 0, outputHeight = 0;

        // Camera, all row-major and jitter-free (Streamline's requirement).
        DirectX::XMFLOAT4X4 viewToClip{};      // projection
        DirectX::XMFLOAT4X4 clipToView{};      // inverse projection
        DirectX::XMFLOAT4X4 clipToPrevClip{};
        DirectX::XMFLOAT4X4 prevClipToClip{};
        DirectX::XMFLOAT4X4 worldToView{};
        DirectX::XMFLOAT4X4 viewToWorld{};

        DirectX::XMFLOAT3 cameraPos{}, cameraUp{}, cameraRight{}, cameraFwd{};
        float nearZ = 0.1f, farZ = 1000.0f, fovY = 1.0f, aspect = 1.777f;

        DirectX::XMFLOAT2 jitter{};  // sub-pixel offset used this frame, in pixels
        bool  reset = false;         // true to discard DLSS's history
        Quality quality = Quality::Balanced;

        // Flip the direction of the motion vectors as DLSS reads them. Ours
        // point from the current pixel back to where the surface was; the
        // opposite convention exists, and the jitter offset already turned out
        // to need negating, so this stays selectable until motion is verified.
        bool invertMotion = false;
    };

    // The render resolution DLSS expects for this preset and output size.
    // DLSS reconstructs from a fixed ratio per preset, so feeding it anything
    // else costs performance and stops it cancelling the sub-pixel jitter.
    // Returns false if DLSS cannot answer, leaving the outputs untouched.
    bool GetRenderSize(Quality q, uint32_t outWidth, uint32_t outHeight,
                       uint32_t& renderWidth, uint32_t& renderHeight);

    // Run Ray Reconstruction for this frame. Returns false (leaving colorOut
    // untouched) when DLSS is unavailable, so the caller can fall back.
    bool EvaluateRR(ID3D12GraphicsCommandList* cmdList, const RRFrame& f);

private:
    DLSS() = default;
    ~DLSS() = default;
    DLSS(const DLSS&) = delete;
    DLSS& operator=(const DLSS&) = delete;

    // sl.interposer.dll and the proxied entry points it exports.
    void* LoadInterposer();

    bool        m_initialized = false;
    bool        m_srSupported = false;
    bool        m_rrSupported = false;
    bool        m_proxied = false;   // true once creation went through SL
    bool        m_rrOptionsSet = false;
    uint32_t    m_rrOutW = 0, m_rrOutH = 0; // options are re-sent when these change
    int         m_rrQuality = -1;
    std::string m_status = "Streamline: not initialized";
};
