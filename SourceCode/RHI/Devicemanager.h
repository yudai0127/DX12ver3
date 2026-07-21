#pragma once
#include <windows.h>
#include "RHI/Device.h"
#include "RHI/CommandContext.h"
#include "RHI/SwapChain.h"
#include "RHI/DepthBuffer.h"
#include "RHI/DescriptorHeap.h"

//-----------------------------------------------------------------------------
// DeviceManager
//  DX12 の GPU リソースを一元管理するシングルトン。
//  （過去作の DX11 版 DeviceManager の DX12 版）
//
//  Device / CommandContext / SwapChain / DepthBuffer / SrvHeap などを
//  まとめて持ち、どこからでも DeviceManager::Instance() でアクセスできる。
//  これにより Sprite などの描画クラスは、コンストラクタで device 等を
//  毎回引数で受け取らずに済む:
//
//    auto sprite = std::make_unique<Sprite>(L"resources/title.png");
//    // 中で DeviceManager::Instance()->GetDevice() などを使う
//-----------------------------------------------------------------------------
class DeviceManager
{
public:
    /// @brief 唯一のインスタンスを返す
    static DeviceManager* Instance()
    {
        static DeviceManager inst;
        return &inst;
    }

    /// @brief 全 GPU リソースを初期化する（Framework から1回だけ呼ぶ）
    bool Initialize(HWND hwnd,
        uint32_t width,
        uint32_t height,
        uint32_t frameCount = 2);

    /// @brief 解放する
    void Uninitialize();

    //-------------------------------------------------------------------
    // アクセサ（各クラスはここから必要なものを取る）
    //-------------------------------------------------------------------
    ID3D12Device* GetDevice() { return m_device.GetDevice(); }
    Device& GetDeviceObj() { return m_device; }
    CommandContext& GetCommand() { return m_command; }
    SwapChain& GetSwapChain() { return m_swapChain; }
    DepthBuffer& GetDepthBuffer() { return m_depthBuffer; }
    DescriptorHeap& GetSrvHeap() { return m_srvHeap; }

    ID3D12GraphicsCommandList* GetCommandList()
    {
        return m_command.GetCommandList();
    }

    DXGI_FORMAT GetRTVFormat() { return m_swapChain.GetFormat(); }
    float       GetScreenWidth()  const { return m_screenWidth; }
    float       GetScreenHeight() const { return m_screenHeight; }
    uint32_t    GetFrameCount()   const { return m_frameCount; }

    /// @brief リサイズ（ウィンドウサイズ変更時）
    void Resize(uint32_t width, uint32_t height);

private:
    DeviceManager() = default;
    ~DeviceManager() = default;
    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    Device         m_device;
    CommandContext m_command;
    SwapChain      m_swapChain;
    DepthBuffer    m_depthBuffer;
    DescriptorHeap m_srvHeap;

    float    m_screenWidth = 0.0f;
    float    m_screenHeight = 0.0f;
    uint32_t m_frameCount = 2;
};