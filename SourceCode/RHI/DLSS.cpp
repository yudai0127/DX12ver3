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
                 sl::PreferenceFlags::eLoadDownloadedPlugins;
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
    m_status = "Streamline: shut down";
#endif
}
