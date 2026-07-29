#include "RHI/DLSS.h"
#include <windows.h>

//-----------------------------------------------------------------------------
// The Streamline SDK is optional. When its headers are not on the include path
// every entry point below becomes a no-op that reports "unavailable", so the
// engine still builds and runs without it.
//-----------------------------------------------------------------------------
#if __has_include(<sl.h>)
  #define DX12_HAS_STREAMLINE 1
  #include <sl.h>
  #if __has_include(<sl_dlss.h>)
    #include <sl_dlss.h>
  #endif
  #if __has_include(<sl_dlss_d.h>)
    #include <sl_dlss_d.h>
  #endif
#else
  #define DX12_HAS_STREAMLINE 0
#endif

namespace
{
    void Log(const char* msg)
    {
        OutputDebugStringA(msg);
        OutputDebugStringA("\n");
    }
}

DLSS& DLSS::Instance()
{
    static DLSS instance;
    return instance;
}

bool DLSS::IsCompiledIn()
{
#if DX12_HAS_STREAMLINE
    return true;
#else
    return false;
#endif
}

//-----------------------------------------------------------------------------
// Startup  -  slInit. Must run before the D3D12 device is created so the
// Streamline interposer can hook the API.
//-----------------------------------------------------------------------------
bool DLSS::Startup()
{
#if DX12_HAS_STREAMLINE
    if (m_initialized) return true;

    sl::Feature featuresToLoad[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_RR };

    sl::Preferences pref{};
    pref.featuresToLoad = featuresToLoad;
    pref.numFeaturesToLoad = _countof(featuresToLoad);
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = nullptr;      // look next to the executable
    pref.numPathsToPlugins = 0;
    pref.pathToLogsAndData = nullptr;
    pref.allocateCallback = nullptr;
    pref.releaseCallback = nullptr;
    pref.logMessageCallback = nullptr;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                 sl::PreferenceFlags::eAllowOTA |
                 sl::PreferenceFlags::eLoadDownloadedPlugins |
                 // Required before slSetTagForFrame may be used; without it
                 // Streamline rejects every per-frame resource tag.
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    // NGX refuses to initialise unless the app identifies itself. We have no
    // NVIDIA-assigned application id, so take the documented alternative: an
    // engine type plus version, and a project GUID.
    pref.applicationId = 0;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0.0";
    pref.projectId = "b395d6bd-7c30-4542-98b8-6b85f0f77329";
    pref.renderAPI = sl::RenderAPI::eD3D12;
#ifdef _DEBUG
    pref.showConsole = true;            // Streamline's own log window
#else
    pref.showConsole = false;
#endif

    sl::Result res = slInit(pref);
    if (res != sl::Result::eOk)
    {
        m_status = "Streamline: slInit failed (are the sl.*.dll / nvngx_*.dll "
                   "next to the .exe?)";
        Log("[DLSS] slInit failed");
        return false;
    }

    m_initialized = true;
    m_status = "Streamline: initialized (support not queried yet)";
    Log("[DLSS] Streamline initialized");
    return true;
#else
    m_status = "Streamline: SDK not compiled in (include path not set)";
    return false;
#endif
}

//-----------------------------------------------------------------------------
// LoadInterposer  -  sl.interposer.dll, loaded once.
//   Streamline proxies D3D12/DXGI through this DLL. DLSS can only run on a
//   device that came out of those proxies, which is why factory and device
//   creation go through it instead of the statically linked entry points.
//-----------------------------------------------------------------------------
void* DLSS::LoadInterposer()
{
#if DX12_HAS_STREAMLINE
    static HMODULE mod = LoadLibraryW(L"sl.interposer.dll");
    return mod;
#else
    return nullptr;
#endif
}

//-----------------------------------------------------------------------------
// CreateFactory / CreateDevice  -  interposed creation with a safe fallback.
//-----------------------------------------------------------------------------
HRESULT DLSS::CreateFactory(UINT flags, REFIID riid, void** out)
{
    using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);

    if (HMODULE mod = (HMODULE)LoadInterposer())
    {
        auto fn = (PFN_CreateDXGIFactory2)GetProcAddress(mod, "CreateDXGIFactory2");
        if (fn)
        {
            HRESULT hr = fn(flags, riid, out);
            if (SUCCEEDED(hr)) return hr;
            Log("[DLSS] interposed CreateDXGIFactory2 failed; using the real one");
        }
    }
    return ::CreateDXGIFactory2(flags, riid, out);
}

HRESULT DLSS::CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minLevel,
                           REFIID riid, void** out)
{
    using PFN_D3D12CreateDevice =
        HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

    if (HMODULE mod = (HMODULE)LoadInterposer())
    {
        auto fn = (PFN_D3D12CreateDevice)GetProcAddress(mod, "D3D12CreateDevice");
        if (fn)
        {
            HRESULT hr = fn(adapter, minLevel, riid, out);
            if (SUCCEEDED(hr))
            {
                m_proxied = true;   // DLSS can attach to this device
                return hr;
            }
        }
    }
    return ::D3D12CreateDevice(adapter, minLevel, riid, out);
}

//-----------------------------------------------------------------------------
// SetDevice  -  hand the D3D12 device to Streamline.
//-----------------------------------------------------------------------------
void DLSS::SetDevice(ID3D12Device* device)
{
#if DX12_HAS_STREAMLINE
    if (!m_initialized || !device) return;

    if (slSetD3DDevice(device) != sl::Result::eOk)
    {
        m_status = "Streamline: slSetD3DDevice failed";
        Log("[DLSS] slSetD3DDevice failed");
    }
#else
    (void)device;
#endif
}

//-----------------------------------------------------------------------------
// QuerySupport  -  ask the driver whether this GPU can run DLSS upscaling and
// DLSS Ray Reconstruction. RR is the AI denoiser that replaces our hand-written
// one; on GeForce RTX cards both should report supported, while frame
// generation (not requested here) is Ada-only.
//-----------------------------------------------------------------------------
void DLSS::QuerySupport()
{
#if DX12_HAS_STREAMLINE
    if (!m_initialized) return;

    sl::AdapterInfo adapterInfo{};
    m_srSupported = (slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo) == sl::Result::eOk);
    m_rrSupported = (slIsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo) == sl::Result::eOk);

    if (m_srSupported && m_rrSupported)
        m_status = "Streamline: DLSS + Ray Reconstruction available";
    else if (m_srSupported)
        m_status = "Streamline: DLSS available, Ray Reconstruction NOT available";
    else if (m_rrSupported)
        m_status = "Streamline: Ray Reconstruction available, DLSS NOT available";
    else if (!m_proxied)
        m_status = "Streamline: device was not created through sl.interposer.dll "
                   "- DLSS cannot attach (is the DLL next to the .exe?)";
    else
        m_status = "Streamline: no DLSS feature supported on this adapter";

    Log(m_status.c_str());
#endif
}

//-----------------------------------------------------------------------------
// EvaluateRR  -  run DLSS Ray Reconstruction for one frame.
//
//   Tags every buffer the feature needs, hands Streamline the camera for this
//   frame, and evaluates. RR both denoises the 1-sample-per-pixel image and
//   upscales it to the output resolution, replacing our spatial denoiser and
//   the bilinear upscale in a single step.
//-----------------------------------------------------------------------------
#if DX12_HAS_STREAMLINE
namespace
{
    // DirectXMath stores row-major, which is what Streamline expects.
    sl::float4x4 ToSL(const DirectX::XMFLOAT4X4& m)
    {
        sl::float4x4 r{};
        for (int i = 0; i < 4; ++i)
            r.row[i] = { m.m[i][0], m.m[i][1], m.m[i][2], m.m[i][3] };
        return r;
    }

    sl::Resource Tex(ID3D12Resource* res)
    {
        // We initialise Streamline with eDisableCLStateTracking, so the state
        // has to be supplied here. Every buffer we hand over is a UAV.
        return sl::Resource{ sl::ResourceType::eTex2d, res, nullptr, nullptr,
                             (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    }
}
#endif

#if DX12_HAS_STREAMLINE
namespace
{
    sl::DLSSMode ToSLMode(DLSS::Quality q)
    {
        switch (q)
        {
        case DLSS::Quality::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
        case DLSS::Quality::Performance:      return sl::DLSSMode::eMaxPerformance;
        case DLSS::Quality::Quality:          return sl::DLSSMode::eMaxQuality;
        case DLSS::Quality::DLAA:             return sl::DLSSMode::eDLAA;
        default:                              return sl::DLSSMode::eBalanced;
        }
    }
}
#endif

//-----------------------------------------------------------------------------
// GetRenderSize  -  ask DLSS what input resolution this preset expects.
//-----------------------------------------------------------------------------
bool DLSS::GetRenderSize(Quality q, uint32_t outWidth, uint32_t outHeight,
                         uint32_t& renderWidth, uint32_t& renderHeight)
{
#if DX12_HAS_STREAMLINE
    if (!m_initialized || !m_rrSupported) return false;

    sl::DLSSDOptions opt{};
    opt.mode = ToSLMode(q);
    opt.outputWidth = outWidth;
    opt.outputHeight = outHeight;

    sl::DLSSDOptimalSettings settings{};
    if (slDLSSDGetOptimalSettings(opt, settings) != sl::Result::eOk) return false;
    if (settings.optimalRenderWidth == 0 || settings.optimalRenderHeight == 0)
        return false;

    renderWidth = settings.optimalRenderWidth;
    renderHeight = settings.optimalRenderHeight;
    return true;
#else
    (void)q; (void)outWidth; (void)outHeight;
    (void)renderWidth; (void)renderHeight;
    return false;
#endif
}

bool DLSS::EvaluateRR(ID3D12GraphicsCommandList* cmdList, const RRFrame& f)
{
#if DX12_HAS_STREAMLINE
    if (!m_initialized || !m_rrSupported || !cmdList) return false;
    if (!f.colorIn || !f.colorOut) return false;

    const sl::ViewportHandle viewport(0);

    // ---- options (re-sent when the output size or preset changes) ---------
    if (!m_rrOptionsSet || m_rrOutW != f.outputWidth || m_rrOutH != f.outputHeight ||
        m_rrQuality != (int)f.quality)
    {
        sl::DLSSDOptions opt{};
        opt.mode = ToSLMode(f.quality);
        opt.outputWidth = f.outputWidth;
        opt.outputHeight = f.outputHeight;
        // Ray Reconstruction only runs on linear HDR colour ("HDR Color
        // required" from the NGX denoiser otherwise), so tonemapping happens
        // after it rather than before.
        opt.colorBuffersHDR = sl::Boolean::eTrue;
        // Our normal buffer carries roughness in .w.
        opt.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
        opt.worldToCameraView = ToSL(f.worldToView);
        opt.cameraViewToWorld = ToSL(f.viewToWorld);

        if (slDLSSDSetOptions(viewport, opt) != sl::Result::eOk)
        {
            m_status = "Streamline: slDLSSDSetOptions failed";
            Log("[DLSS] slDLSSDSetOptions failed");
            return false;
        }
        m_rrOptionsSet = true;
        m_rrOutW = f.outputWidth;
        m_rrOutH = f.outputHeight;
        m_rrQuality = (int)f.quality;
    }

    sl::FrameToken* frame = nullptr;
    if (slGetNewFrameToken(frame) != sl::Result::eOk || !frame) return false;

    // ---- per-frame camera constants ---------------------------------------
    sl::Constants c{};
    c.cameraViewToClip = ToSL(f.viewToClip);
    c.clipToCameraView = ToSL(f.clipToView);
    c.clipToPrevClip = ToSL(f.clipToPrevClip);
    c.prevClipToClip = ToSL(f.prevClipToClip);
    c.jitterOffset = { f.jitter.x, f.jitter.y };     // pixel space
    // Our motion vectors are in pixels, so scale them into clip space.
    c.mvecScale = { 1.0f / (float)f.renderWidth, 1.0f / (float)f.renderHeight };
    c.cameraPos = { f.cameraPos.x, f.cameraPos.y, f.cameraPos.z };
    c.cameraUp = { f.cameraUp.x, f.cameraUp.y, f.cameraUp.z };
    c.cameraRight = { f.cameraRight.x, f.cameraRight.y, f.cameraRight.z };
    c.cameraFwd = { f.cameraFwd.x, f.cameraFwd.y, f.cameraFwd.z };
    c.cameraNear = f.nearZ;
    c.cameraFar = f.farZ;
    c.cameraFOV = f.fovY;
    c.cameraAspectRatio = f.aspect;
    c.depthInverted = sl::Boolean::eFalse;      // linear distance, grows with range
    c.cameraMotionIncluded = sl::Boolean::eTrue;// our vectors already include it
    c.motionVectors3D = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.reset = f.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;

    if (slSetConstants(c, *frame, viewport) != sl::Result::eOk)
    {
        m_status = "Streamline: slSetConstants failed";
        Log("[DLSS] slSetConstants failed");
        return false;
    }

    // ---- tag the buffers ---------------------------------------------------
    // Inputs live in the top-left sub-rectangle of full-size textures; only the
    // output covers its whole surface.
    sl::Extent inExt{ 0, 0, f.renderWidth, f.renderHeight };
    sl::Extent outExt{ 0, 0, f.outputWidth, f.outputHeight };

    sl::Resource rColorIn = Tex(f.colorIn);
    sl::Resource rColorOut = Tex(f.colorOut);
    sl::Resource rAlbedo = Tex(f.diffuseAlbedo);
    sl::Resource rSpec = Tex(f.specularAlbedo);
    sl::Resource rNormRough = Tex(f.normalRoughness);
    sl::Resource rDepth = Tex(f.linearDepth);
    sl::Resource rMvec = Tex(f.motionVectors);

    const sl::ResourceLifecycle life = sl::ResourceLifecycle::eValidUntilPresent;
    sl::ResourceTag tags[] = {
        { &rColorIn,   sl::kBufferTypeScalingInputColor,  life, &inExt  },
        { &rColorOut,  sl::kBufferTypeScalingOutputColor, life, &outExt },
        { &rAlbedo,    sl::kBufferTypeAlbedo,             life, &inExt  },
        { &rSpec,      sl::kBufferTypeSpecularAlbedo,     life, &inExt  },
        { &rNormRough, sl::kBufferTypeNormalRoughness,    life, &inExt  },
        { &rDepth,     sl::kBufferTypeLinearDepth,        life, &inExt  },
        { &rMvec,      sl::kBufferTypeMotionVectors,      life, &inExt  },
    };

    if (slSetTagForFrame(*frame, viewport, tags, _countof(tags), cmdList)
        != sl::Result::eOk)
    {
        m_status = "Streamline: slSetTagForFrame failed";
        Log("[DLSS] slSetTagForFrame failed");
        return false;
    }

    // ---- evaluate ----------------------------------------------------------
    const sl::BaseStructure* inputs[] = { &viewport };
    if (slEvaluateFeature(sl::kFeatureDLSS_RR, *frame, inputs, _countof(inputs),
                          cmdList) != sl::Result::eOk)
    {
        m_status = "Streamline: slEvaluateFeature(DLSS_RR) failed";
        Log("[DLSS] slEvaluateFeature(DLSS_RR) failed");
        return false;
    }
    return true;
#else
    (void)cmdList; (void)f;
    return false;
#endif
}

//-----------------------------------------------------------------------------
// Shutdown
//-----------------------------------------------------------------------------
void DLSS::Shutdown()
{
#if DX12_HAS_STREAMLINE
    if (!m_initialized) return;
    slShutdown();
    m_initialized = false;
    m_srSupported = false;
    m_rrSupported = false;
    m_rrOptionsSet = false;
    m_status = "Streamline: shut down";
#endif
}
