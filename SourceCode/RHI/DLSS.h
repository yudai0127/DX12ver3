#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
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
    std::string m_status = "Streamline: not initialized";
};
