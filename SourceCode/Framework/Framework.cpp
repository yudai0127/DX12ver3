
#include "Framework/Framework.h"
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
        spriteObj->AddComponent<SpriteRenderer>(L"Resources/ue.png");
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
        m_useRaytracing = true; // default to the ray-traced view when available
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
            m_useRaytracing = !m_useRaytracing;
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
    ImGui::DragFloat3("Eye", &m_camera.eye.x, 0.1f);
    ImGui::DragFloat3("Focus", &m_camera.focus.x, 0.1f);
    ImGui::DragFloat("FOV", &m_camera.fovDegree, 0.5f, 10.0f, 120.0f);
    ImGui::DragFloat3("Light", &m_camera.lightDir.x, 0.05f, -1.0f, 1.0f);
    ImGui::DragFloat("Light Intensity", &m_camera.lightIntensity, 0.05f, 0.0f, 20.0f);
    ImGui::Separator();
    if (m_raytracer.IsValid())
    {
        ImGui::Checkbox("Use Raytracing (DXR) [F6]", &m_useRaytracing);
        ImGui::Text("RT instances: %zu", m_raytracer.GetInstanceCount());
        ImGui::SliderInt("RT Reflect Bounces", &m_raytracer.maxBounces, 0, 3);
    }
    else
    {
        ImGui::TextDisabled("Raytracing (DXR): not available");
    }
    ImGui::End();

    // シェーダーエディタ（アプリ内で編集→保存＆リロード）
    m_shaderEditor.DrawImGui();

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

    if (m_useRaytracing && m_raytracer.IsValid())
    {
        // DXR: trace into an offscreen image and copy it into the back buffer.
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

    RECT rect = { 0, 0, (LONG)config.width, (LONG)config.height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(
        0, L"DX12FrameworkWindow", config.title,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);

    if (!m_hwnd)
    {
        OutputDebugStringW(L"[Framework] ウィンドウ生成失敗\n");
        return -1;
    }

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