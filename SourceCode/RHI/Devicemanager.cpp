#include "DeviceManager.h"
#include <stdexcept>

//-----------------------------------------------------------------------------
// Initialize  ―  全 GPU リソースを順番に初期化
//-----------------------------------------------------------------------------
bool DeviceManager::Initialize(HWND hwnd,
    uint32_t width,
    uint32_t height,
    uint32_t frameCount)
{
    m_screenWidth = static_cast<float>(width);
    m_screenHeight = static_cast<float>(height);
    m_frameCount = frameCount;

    // ① デバイス
    if (!m_device.Initialize())
        return false;

    // ② コマンドキュー・リスト
    if (!m_command.Initialize(
        m_device.GetDevice(),
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        frameCount))
        return false;

    // ③ スワップチェーン + RTV
    if (!m_swapChain.Initialize(
        m_device.GetFactory(),
        m_device.GetDevice(),
        m_command.GetQueue(),
        hwnd, width, height, frameCount))
        return false;

    // ④ 深度バッファ
    if (!m_depthBuffer.Initialize(m_device.GetDevice(), width, height))
        return false;

    // ⑤ テクスチャ用 SRV ヒープ（最大128枚）
    if (!m_srvHeap.Initialize(m_device.GetDevice(), 128, true))
        return false;

    return true;
}

//-----------------------------------------------------------------------------
// Uninitialize
//-----------------------------------------------------------------------------
void DeviceManager::Uninitialize()
{
    m_srvHeap.Uninitialize();
    m_depthBuffer.Uninitialize();
    m_swapChain.Uninitialize();
    m_command.Uninitialize();
    m_device.Uninitialize();
}

//-----------------------------------------------------------------------------
// Resize
//-----------------------------------------------------------------------------
void DeviceManager::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;

    m_screenWidth = static_cast<float>(width);
    m_screenHeight = static_cast<float>(height);

    m_command.GetFence().WaitForIdle(m_command.GetQueue());
    m_swapChain.Resize(width, height);
    m_depthBuffer.Resize(width, height);
}