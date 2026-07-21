#include "ImGui/ImGuiManager.h"


#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

#include <cassert>

//-----------------------------------------------------------------------------
// Initialize
//-----------------------------------------------------------------------------
bool ImGuiManager::Initialize(HWND                hwnd,
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    uint32_t            frameCount,
    DXGI_FORMAT         rtvFormat)
{
    assert(device && queue && hwnd);

    // ---- ImGui のフォントテクスチャ用 SRV DescriptorHeap を作成 --------
    // DX12 では ImGui がフォント画像を SRV として持つためヒープが要る。
    // SHADER_VISIBLE フラグが必須（シェーダーから参照するため）。
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;  // フォント1枚分（最低限）
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(
        &heapDesc, IID_PPV_ARGS(&m_srvHeap))))
    {
        OutputDebugStringW(L"[ImGui] SRV DescriptorHeap 生成失敗\n");
        return false;
    }
    m_srvHeap->SetName(L"ImGuiSrvHeap");

    // ---- ImGui コンテキスト生成 --------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // キーボード操作を有効化

    ImGui::StyleColorsDark(); // ダークテーマ

    // ---- 日本語フォントを読み込む ------------------------------------
    {
        static ImFontConfig config; // staticで寿命を確保
        config.MergeMode = false;
        const ImWchar* ranges = io.Fonts->GetGlyphRangesJapanese();
        ImFont* font = io.Fonts->AddFontFromFileTTF(
            "C:/Windows/Fonts/meiryo.ttc", 18.0f, &config, ranges);
        if (!font)
            font = io.Fonts->AddFontFromFileTTF(
                "C:/Windows/Fonts/msgothic.ttc", 18.0f, &config, ranges);
        if (!font)
        {
            io.Fonts->AddFontDefault();
            OutputDebugStringW(L"[ImGui] 日本語フォント読み込み失敗（ファイルが無い）\n");
        }
        else
        {
            OutputDebugStringW(L"[ImGui] 日本語フォント読み込み成功\n");
        }
    }

    // ---- Win32 バックエンド初期化 ------------------------------------
    if (!ImGui_ImplWin32_Init(hwnd))
    {
        OutputDebugStringW(L"[ImGui] Win32 バックエンド初期化失敗\n");
        return false;
    }

    // ---- DX12 バックエンド初期化（1.92系の新 API: InitInfo 構造体）----
    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device = device;
    initInfo.CommandQueue = queue;
    initInfo.NumFramesInFlight = static_cast<int>(frameCount);
    initInfo.RTVFormat = rtvFormat;
    initInfo.SrvDescriptorHeap = m_srvHeap.Get();

    // SRV ディスクリプタの確保／解放コールバック。
    // フォント1枚しか使わないので、ヒープ先頭をそのまま返す単純な実装。
    initInfo.SrvDescriptorAllocFn =
        [](ImGui_ImplDX12_InitInfo* info,
            D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
            D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
        {
            ID3D12DescriptorHeap* heap = info->SrvDescriptorHeap;
            *outCpu = heap->GetCPUDescriptorHandleForHeapStart();
            *outGpu = heap->GetGPUDescriptorHandleForHeapStart();
        };
    initInfo.SrvDescriptorFreeFn =
        [](ImGui_ImplDX12_InitInfo*,
            D3D12_CPU_DESCRIPTOR_HANDLE,
            D3D12_GPU_DESCRIPTOR_HANDLE)
        {
            // 単一ディスクリプタなので解放処理は不要
        };

    if (!ImGui_ImplDX12_Init(&initInfo))
    {
        OutputDebugStringW(L"[ImGui] DX12 バックエンド初期化失敗\n");
        return false;
    }

    m_initialized = true;
    return true;
}

//-----------------------------------------------------------------------------
// Uninitialize
//-----------------------------------------------------------------------------
void ImGuiManager::Uninitialize()
{
    if (!m_initialized) return;

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    m_srvHeap = nullptr;
    m_initialized = false;
}

//-----------------------------------------------------------------------------
// NewFrame  ―  毎フレームの最初に呼ぶ
//-----------------------------------------------------------------------------
void ImGuiManager::NewFrame()
{
    if (!m_initialized) return;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

//-----------------------------------------------------------------------------
// Render  ―  RTV をセットした後に呼ぶ
//-----------------------------------------------------------------------------
void ImGuiManager::Render(ID3D12GraphicsCommandList* commandList)
{
    if (!m_initialized) return;

    // ImGui の描画データを確定
    ImGui::Render();

    // SRV ヒープをコマンドリストにセット（DX12 では必須）
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);

    // ImGui の描画コマンドを積む
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
}