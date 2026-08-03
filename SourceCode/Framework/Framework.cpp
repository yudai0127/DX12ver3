
#include "Framework/Framework.h"
#include "RHI/DLSS.h"
#include <stdexcept>
#include <cassert>
#include <dxgi1_6.h>
#include <psapi.h>

#include "imgui.h"
#include "imgui_impl_win32.h"

#include "Core/RenderStats.h"
#include "Component/SpriteRenderer.h"
#include "Component/SpriteBatchRenderer.h"
#include "Component/MeshRenderer.h"
#include "Component/ModelRenderer.h"
#include "Camera/Camera.h"
#include "Core/Scene.h"
#include "Core/GameObject.h"

//=============================================================================
//  ゲーム処理（Initialize / Update / Render / Uninitialize）
//=============================================================================

//-----------------------------------------------------------------------------
// Initialize  
//-----------------------------------------------------------------------------
void Framework::Initialize()
{
    m_scene = std::make_unique<Scene>("MainScene");

    // カメラ（シーン定数バッファ）を初期化
    m_camera.Initialize();
    MeshRenderer::SetCamera(&m_camera);
    ModelRenderer::SetCamera(&m_camera);

    // glTF モデルを表示（段階1：静的メッシュ）
    GameObject* model = m_scene->CreateObject("GltfModel");
    ModelRenderer* mr =
        model->AddComponent<ModelRenderer>("Resources/Dragon/Dragon_knight_UE4.gltf");
 
    mr->SetColor(0.8f, 0.8f, 0.8f, 1.0f);

    // スプライトを表示
    GameObject* spriteObj = m_scene->CreateObject("Sprite");
    SpriteRenderer* sr =
        spriteObj->AddComponent<SpriteRenderer>(L"Resources/ue.DDS");
    sr->SetPosition(50.0f, 50.0f);      // 画面上の位置
    sr->SetSize(200.0f, 200.0f);        // 表示サイズ

    m_modelRenderer = mr; // F5でホットリロードするため保持


    // シェーダーエディタを初期化（HLSLフォルダの.hlslを編集できる）
    m_shaderEditor.Initialize("HLSL", mr);

    // Raytracing (DXR): build acceleration structures + pipeline from the
    // scene. If the GPU/driver has no DXR support this quietly fails and the
    // app keeps using the rasterizer.
    if (m_raytracer.Initialize(m_scene.get(), m_config.width, m_config.height))
    {
        m_renderMode = 1; // default to realtime raytracing (clean, no noise)
    }
}

//-----------------------------------------------------------------------------
// Update
//-----------------------------------------------------------------------------
void Framework::Update(float elapsedTime)
{
    m_frameTimer.Update(elapsedTime);

    // シェーダーのホットリロード
    if (m_modelRenderer)
    {
        // 保存を検知して自動リロード
        m_modelRenderer->CheckAutoReload();

        // F5 でも手動リロードできる
        static bool prevF5 = false;
        bool nowF5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (nowF5 && !prevF5)
            m_modelRenderer->ReloadShaders();
        prevF5 = nowF5;
    }

    // カメラ（ビュー・射影行列）を更新してシーン定数バッファに反映
    // F6: toggle rasterizer / raytracing (only when DXR is available)
    if (m_raytracer.IsValid())
    {
        static bool prevF6 = false;
        bool nowF6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (nowF6 && !prevF6)
            m_renderMode = (m_renderMode + 1) % 3;
        prevF6 = nowF6;
    }

    const float aspect = DM()->GetScreenWidth() / DM()->GetScreenHeight();
    m_camera.Update(aspect);

    m_scene->Update();
    m_scene->FlushDestroyQueue();
}

//-----------------------------------------------------------------------------
// Render
//-----------------------------------------------------------------------------
void Framework::Render(float elapsedTime)
{
    // カメラ・ライト調整（授業 UNIT11 手順12）
    ImGui::Begin("Camera / Light");
    // With vsync on a 144 Hz panel the frame rate quantises to 144/72/48:
    // one vblank missed (e.g. DLSS's ~3ms) reads as a 144->65 collapse.
    // Turn it off to see what a change really costs.
    ImGui::Checkbox("VSync", &m_config.vsync);
    if (!m_config.vsync && !DM()->GetSwapChain().IsTearingSupported())
        ImGui::TextDisabled("(tearing unsupported: compositor may still cap)");
    ImGui::Separator();
    ImGui::DragFloat3("Eye", &m_camera.eye.x, 0.1f);
    ImGui::DragFloat3("Focus", &m_camera.focus.x, 0.1f);
    ImGui::DragFloat("FOV", &m_camera.fovDegree, 0.5f, 10.0f, 120.0f);
    ImGui::DragFloat3("Light", &m_camera.lightDir.x, 0.05f, -1.0f, 1.0f);
    ImGui::DragFloat("Light Intensity", &m_camera.lightIntensity, 0.05f, 0.0f, 20.0f);
    ImGui::Separator();
    if (m_raytracer.IsValid())
    {
    // ---- Transform editor -------------------------------------------
    //   Editing a transform moves the object in both paths: the rasterizer
    //   picks it up through the world matrix, and the ray tracer rebuilds its
    //   top-level acceleration structure the same frame.
    if (m_scene && ImGui::CollapsingHeader("Objects"))
    {
        const auto& objects = m_scene->GetObjects();
        for (size_t i = 0; i < objects.size(); ++i)
        {
            GameObject* obj = objects[i].get();
            if (!obj) continue;
            Transform* tr = obj->GetTransform();
            if (!tr) continue;

            ImGui::PushID((int)i);
            if (ImGui::TreeNode(obj->GetName().c_str()))
            {
                XMFLOAT3 p = tr->GetPosition();
                if (ImGui::DragFloat3("Position", &p.x, 0.5f))
                    tr->SetPosition(p);

                XMFLOAT3 r = tr->GetRotation();
                if (ImGui::DragFloat3("Rotation", &r.x, 1.0f))
                    tr->SetRotation(r);

                XMFLOAT3 s = tr->GetScale();
                if (ImGui::DragFloat3("Scale", &s.x, 0.01f, 0.01f, 100.0f))
                    tr->SetScale(s);

                // Uniform scaling is the common case, so give it its own slider.
                float uniform = s.x;
                if (ImGui::DragFloat("Scale (uniform)", &uniform, 0.01f, 0.01f, 100.0f))
                    tr->SetScale(uniform);

                if (ImGui::Button("Reset"))
                {
                    tr->SetPosition(0.0f, 0.0f, 0.0f);
                    tr->SetRotation(0.0f, 0.0f, 0.0f);
                    tr->SetScale(1.0f);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

        const char* modes[] = { "Rasterizer", "Raytracing", "Path Tracing" };
        ImGui::Combo("Render Mode [F6]", &m_renderMode, modes, 3);
        if (m_renderMode != 0)
        {
            ImGui::Text("RT instances: %zu", m_raytracer.GetInstanceCount());
            ImGui::SliderInt("RT Path Depth", &m_raytracer.maxBounces, 1, 8);
            ImGui::SliderFloat("Render Scale", &m_raytracer.renderScale, 0.25f, 1.0f);
            ImGui::Text("Render res: %u x %u", m_raytracer.GetRenderWidth(),
                        m_raytracer.GetRenderHeight());
            if (m_renderMode == 2)
            {
                ImGui::Text("Accumulated: %u spp", m_raytracer.GetAccumulatedSamples());
                ImGui::Checkbox("Denoise", &m_raytracer.denoise);
                ImGui::SliderInt("Denoise Radius", &m_raytracer.denoiseRadius, 0, 4);
                if (m_raytracer.useDLSS)
                    ImGui::TextDisabled("Ray Reconstruction uses 1 sample / frame");
                else
                    ImGui::SliderInt("Samples / Frame", &m_raytracer.samplesPerFrame, 1, 8);
            }
            else
            {
                // Raytracing mode has no antialiasing without this.
                ImGui::Checkbox("TAA", &m_raytracer.taa);
            }
            // Use the cheaper Super Resolution feature for deterministic RT;
            // reserve the denoising Ray Reconstruction feature for noisy PT.
            const bool dlssAvailable = (m_renderMode == 2)
                ? DLSS::Instance().SupportsRayReconstruction()
                : DLSS::Instance().SupportsUpscaling();
            if (dlssAvailable)
            {
                const char* dlssLabel = (m_renderMode == 2)
                    ? "DLSS Ray Reconstruction"
                    : "DLSS Super Resolution";
                ImGui::Checkbox(dlssLabel, &m_raytracer.useDLSS);
                if (m_raytracer.useDLSS)
                {
                    const char* q[] = { "Ultra Performance", "Performance",
                                        "Balanced", "Quality", "DLAA (native)" };
                    ImGui::Combo("DLSS Quality", &m_raytracer.dlssQuality, q, 5);
                    ImGui::Text("DLSS: %s",
                                m_raytracer.WasDLSSActive() ? "active" : "FAILED (fallback)");
                    ImGui::TextDisabled("Render Scale is ignored while DLSS is on");
                }
            }
            ImGui::SliderFloat("RT Exposure", &m_raytracer.exposure, 0.05f, 2.0f);
            ImGui::SliderFloat("Env Intensity", &m_raytracer.envIntensity, 0.0f, 5.0f);
            ImGui::Separator();
            ImGui::TextWrapped("%s", DLSS::Instance().StatusText());
        }
    }
    else
    {
        ImGui::TextDisabled("Raytracing (DXR): not available");
        m_renderMode = 0;
    }
    ImGui::End();

    // シェーダーエディタ（アプリ内で編集→保存＆リロード）
    m_shaderEditor.DrawImGui();

    // QueryVideoMemoryInfo has a measurable CPU cost, so the VRAM readout
    // refreshes a few times a second instead of every frame.
    static uint32_t s_vramPoll = 0;
    if ((s_vramPoll++ % 30) == 0)
    {
        ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(DM()->GetDeviceObj().GetAdapter()->QueryInterface(
            IID_PPV_ARGS(&adapter3))))
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            {
                const float toMB = 1.0f / (1024.0f * 1024.0f);
                m_frameTimer.SetVramUsageMB(
                    info.CurrentUsage * toMB, info.Budget * toMB);
            }
        }
    }

    {
        PROCESS_MEMORY_COUNTERS pmc = {};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        {
            const float toMB = 1.0f / (1024.0f * 1024.0f);
            m_frameTimer.SetRamUsageMB(
                static_cast<float>(pmc.WorkingSetSize) * toMB);
        }
    }

    m_frameTimer.DrawImGui();

    const float clearColor[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
    BeginFrame(clearColor);

    if (m_renderMode != 0 && m_raytracer.IsValid())
    {
        // DXR: trace into an offscreen image and copy it into the back buffer.
        m_raytracer.renderMode = (m_renderMode == 2) ? 1 : 0; // 1=path tracing
        m_raytracer.Render(m_camera);
    }
    else
    {
        m_scene->Render(GetCmdList());
    }
    m_imgui.Render(GetCmdList());       // ImGui の描画

    EndFrame();
}

//-----------------------------------------------------------------------------
// Uninitialize
//-----------------------------------------------------------------------------
void Framework::Uninitialize()
{
    m_scene.reset();
}

//=============================================================================
//  フレームワーク内部処理
//=============================================================================

//-----------------------------------------------------------------------------
// Run
//-----------------------------------------------------------------------------
int Framework::Run(const FrameworkConfig& config)
{
    m_config = config;

    // ---- Win32 ウィンドウ生成 -----------------------------------------
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DX12FrameworkWindow";
    RegisterClassExW(&wc);

    DWORD windowStyle = WS_OVERLAPPEDWINDOW;
    DWORD windowExStyle = 0;
    int windowX = CW_USEDEFAULT;
    int windowY = CW_USEDEFAULT;
    int windowWidth = (int)config.width;
    int windowHeight = (int)config.height;
    bool borderlessFullscreen = false;

    if (config.fullscreen)
    {
        // Borderless fullscreen is the reliable flip-model path on modern
        // Windows. Size the client area to the primary monitor and omit the
        // title bar/taskbar instead of relying on DXGI exclusive fullscreen.
        POINT origin = { 0, 0 };
        HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(monitor, &mi))
        {
            windowStyle = WS_POPUP;
            windowExStyle = WS_EX_APPWINDOW;
            windowX = mi.rcMonitor.left;
            windowY = mi.rcMonitor.top;
            windowWidth = mi.rcMonitor.right - mi.rcMonitor.left;
            windowHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;
            borderlessFullscreen = true;
        }
    }
    if (!borderlessFullscreen)
    {
        RECT rect = { 0, 0, (LONG)config.width, (LONG)config.height };
        AdjustWindowRectEx(&rect, windowStyle, FALSE, windowExStyle);
        windowWidth = rect.right - rect.left;
        windowHeight = rect.bottom - rect.top;
    }

    // WM_SIZE can be delivered during window creation. Seed the intended
    // client size first so it cannot try to resize an uninitialized device.
    if (borderlessFullscreen)
    {
        m_config.width = (uint32_t)windowWidth;
        m_config.height = (uint32_t)windowHeight;
    }

    m_hwnd = CreateWindowExW(
        windowExStyle, L"DX12FrameworkWindow", config.title,
        windowStyle, windowX, windowY,
        windowWidth, windowHeight,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);

    if (!m_hwnd)
    {
        OutputDebugStringW(L"[Framework] ウィンドウ生成失敗\n");
        return -1;
    }

    // Use the actual client size for the swap chain and all render targets.
    // In fullscreen this is the monitor resolution, not config.width/height.
    RECT client = {};
    GetClientRect(m_hwnd, &client);
    m_config.width = (uint32_t)(client.right - client.left);
    m_config.height = (uint32_t)(client.bottom - client.top);

    InitializeD3D(m_hwnd);
    Initialize();

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    // ---- メインループ -------------------------------------------------
    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        else
        {
            QueryPerformanceCounter(&now);
            float elapsed = static_cast<float>(now.QuadPart - prev.QuadPart)
                / static_cast<float>(freq.QuadPart);
            prev = now;

            m_imgui.NewFrame();
            Update(elapsed);
            Render(elapsed);
        }
    }

    DM()->GetCommand().GetFence().WaitForIdle(DM()->GetCommand().GetQueue());
    Uninitialize();
    UninitializeD3D();

    return static_cast<int>(msg.wParam);
}

//-----------------------------------------------------------------------------
// InitializeD3D  ―  DeviceManager で全 GPU リソースを初期化
//-----------------------------------------------------------------------------
void Framework::InitializeD3D(HWND hwnd)
{
    if (!DM()->Initialize(hwnd, m_config.width, m_config.height, m_config.frameCount))
        throw std::runtime_error("DeviceManager 初期化失敗");

    // ImGui 初期化
    if (!m_imgui.Initialize(
        hwnd,
        DM()->GetDevice(),
        DM()->GetCommand().GetQueue(),
        m_config.frameCount,
        DM()->GetRTVFormat()))
        throw std::runtime_error("ImGui 初期化失敗");

    m_frameFenceValues.assign(m_config.frameCount, 0);
    m_frameIndex = 0;

    m_gpuTimer.Initialize(DM()->GetDevice(),
        DM()->GetCommand().GetQueue(),
        m_config.frameCount);
}

//-----------------------------------------------------------------------------
// UninitializeD3D
//-----------------------------------------------------------------------------
void Framework::UninitializeD3D()
{
    m_gpuTimer.Uninitialize();
    m_imgui.Uninitialize();
    DM()->Uninitialize();
}

//-----------------------------------------------------------------------------
// BeginFrame
//-----------------------------------------------------------------------------
void Framework::BeginFrame(const float clearColor[4])
{
    auto& swapChain = DM()->GetSwapChain();
    auto& command = DM()->GetCommand();
    auto& depthBuffer = DM()->GetDepthBuffer();

    m_frameIndex = swapChain.GetCurrentBackBufferIndex();
    WaitForFrame(m_frameIndex);
    command.Reset(m_frameIndex);

    auto* cmd = command.GetCommandList();


    if (m_gpuTimer.IsValid())
    {
        m_gpuTimer.Resolve(m_frameIndex);
        m_frameTimer.SetGpuTimeMs(m_gpuTimer.GetGpuTimeMs());
        m_frameTimer.SetGpuScopeMs(0, "Trace",
            m_gpuTimer.GetScopeMs(GpuTimer::ScopeTrace));
        m_frameTimer.SetGpuScopeMs(1, "Denoise/TAA",
            m_gpuTimer.GetScopeMs(GpuTimer::ScopeResolve));
        m_frameTimer.SetGpuScopeMs(2, "DLSS",
            m_gpuTimer.GetScopeMs(GpuTimer::ScopeDLSS));
        m_frameTimer.SetGpuScopeMs(3, "Post",
            m_gpuTimer.GetScopeMs(GpuTimer::ScopePost));
        m_gpuTimer.BeginFrame(cmd, m_frameIndex);
    }
    RenderStats::Instance().Reset();

    command.ResourceBarrier(
        swapChain.GetCurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);

    auto rtv = swapChain.GetCurrentRTV();
    auto dsv = depthBuffer.GetDSV();

    D3D12_VIEWPORT vp = {};
    vp.Width = DM()->GetScreenWidth();
    vp.Height = DM()->GetScreenHeight();
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    cmd->RSSetViewports(1, &vp);

    D3D12_RECT scissor = { 0, 0,
        static_cast<LONG>(DM()->GetScreenWidth()),
        static_cast<LONG>(DM()->GetScreenHeight()) };
    cmd->RSSetScissorRects(1, &scissor);

    cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    if (clearColor)
        cmd->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

    cmd->ClearDepthStencilView(
        dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, nullptr);
}

//-----------------------------------------------------------------------------
// EndFrame
//-----------------------------------------------------------------------------
void Framework::EndFrame()
{
    auto& swapChain = DM()->GetSwapChain();
    auto& command = DM()->GetCommand();

    command.ResourceBarrier(
        swapChain.GetCurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);


    if (m_gpuTimer.IsValid())
        m_gpuTimer.EndFrame(command.GetCommandList(), m_frameIndex);

    const uint64_t fenceValue = command.Execute();
    m_frameFenceValues[m_frameIndex] = fenceValue;

    swapChain.Present(m_config.vsync ? 1 : 0);
}

//-----------------------------------------------------------------------------
// Resize
//-----------------------------------------------------------------------------
void Framework::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;
    if (width == m_config.width && height == m_config.height) return;

    m_config.width = width;
    m_config.height = height;

    DM()->Resize(width, height);

    // Keep the ray-tracing output image in sync with the window size.
    m_raytracer.Resize(width, height);
    m_frameFenceValues.assign(m_config.frameCount, 0);
}

//-----------------------------------------------------------------------------
// WaitForFrame
//-----------------------------------------------------------------------------
void Framework::WaitForFrame(uint32_t frameIndex)
{
    DM()->GetCommand().GetFence().WaitForValue(m_frameFenceValues[frameIndex]);
}

//-----------------------------------------------------------------------------
// WndProc
//-----------------------------------------------------------------------------
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK Framework::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return true;

    Framework* fw = reinterpret_cast<Framework*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_CREATE:
    {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    case WM_SIZE:
        if (fw && wp != SIZE_MINIMIZED)
            fw->Resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
