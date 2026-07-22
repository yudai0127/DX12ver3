#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>
#include "Core/FrameTimer.h"
#include "RHI/DeviceManager.h"
#include "ImGui/ImGuiManager.h"
#include "ImGui/ShaderEditor.h"
#include "RHI/GpuTimer.h" 
#include "Core/Scene.h"
#include "Camera/Camera.h"
#include "Graphics/RaytracingRenderer.h"
#include <memory>

//-----------------------------------------------------------------------------
// FrameworkConfig  ―  アプリ起動設定
//-----------------------------------------------------------------------------
struct FrameworkConfig
{
    const wchar_t* title = L"DX12 Framework";
    uint32_t       width = 1280;
    uint32_t       height = 720;
    uint32_t       frameCount = 2;
    bool           vsync = true;
    bool           fullscreen = false;
};

//-----------------------------------------------------------------------------
// Framework  ―  Win32 ウィンドウ + DX12 ループ
//
//  GPU リソースは DeviceManager（シングルトン）が一元管理する。
//  Framework は DeviceManager を初期化し、メインループを回すだけ。
//  ゲーム処理は Initialize / Update / Render / Uninitialize に直接書く。
//-----------------------------------------------------------------------------
class Framework
{
public:
    Framework() = default;
    ~Framework() { UninitializeD3D(); }

    Framework(const Framework&) = delete;
    Framework& operator=(const Framework&) = delete;

    int Run(const FrameworkConfig& config);

private:
    // ゲーム処理（Framework.cpp に直接書く）
    void Initialize();
    void Update(float elapsedTime);
    void Render(float elapsedTime);
    void Uninitialize();

    // フレームヘルパー
    void BeginFrame(const float clearColor[4] = nullptr);
    void EndFrame();

    // 内部処理
    void InitializeD3D(HWND hwnd);
    void UninitializeD3D();
    void Resize(uint32_t width, uint32_t height);
    void WaitForFrame(uint32_t frameIndex);

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    // DeviceManager から取得する便利アクセサ
    DeviceManager* DM() { return DeviceManager::Instance(); }
    ID3D12GraphicsCommandList* GetCmdList() { return DM()->GetCommandList(); }

    FrameworkConfig m_config;
    HWND            m_hwnd = nullptr;

    FrameTimer     m_frameTimer;
    GpuTimer  m_gpuTimer;
    ImGuiManager   m_imgui;
    ShaderEditor   m_shaderEditor;
    std::unique_ptr<Scene> m_scene;
    class ModelRenderer* m_modelRenderer = nullptr; // ホットリロード対象（非所有）
    Camera         m_camera;

    RaytracingRenderer m_raytracer;              // DXR path (optional)
    bool               m_useRaytracing = false;  // toggle: raster vs raytracing

    std::vector<uint64_t> m_frameFenceValues;
    uint32_t              m_frameIndex = 0;
};