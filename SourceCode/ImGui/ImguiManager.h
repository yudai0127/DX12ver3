#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// ImGuiManager
//  Dear ImGui (DX12版) の初期化・フレーム処理・描画・解放をまとめるクラス。
//
//  授業資料(DX11)では USE_IMGUI マクロで以下を呼んでいた:
//    ImGui::Render();
//    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
//
//  DX12 では DX11 と違い、フォントテクスチャ用に
//  SRV (Shader Resource View) の DescriptorHeap が必要になる。
//  このクラスがその DescriptorHeap も内部で確保する。
//
//  使い方（Framework から呼ぶ）:
//    初期化:           imgui.Initialize(hwnd, device, queue, frameCount, rtvFormat);
//    毎フレーム冒頭:   imgui.NewFrame();
//                      ここで ImGui::Begin() などの UI を構築
//    描画時(RTVセット後): imgui.Render(commandList);
//    終了:             imgui.Uninitialize();
//-----------------------------------------------------------------------------
class ImGuiManager
{
public:
    ImGuiManager() = default;
    ~ImGuiManager() { Uninitialize(); }

    ImGuiManager(const ImGuiManager&) = delete;
    ImGuiManager& operator=(const ImGuiManager&) = delete;

    //-------------------------------------------------------------------
    // 初期化 / 解放
    //-------------------------------------------------------------------

    /// @brief ImGui を初期化する
    /// @param hwnd        ウィンドウハンドル
    /// @param device      D3D12 デバイス
    /// @param queue       コマンドキュー（フォントテクスチャのアップロードに使う）
    /// @param frameCount  フレームバッファ数（SwapChain と揃える）
    /// @param rtvFormat   バックバッファのフォーマット
    bool Initialize(HWND                hwnd,
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        uint32_t            frameCount,
        DXGI_FORMAT         rtvFormat);

    void Uninitialize();

    //-------------------------------------------------------------------
    // フレーム処理
    //-------------------------------------------------------------------

    /// @brief フレーム開始。Update より前に呼ぶ
    ///        この後 ImGui::Begin() などで UI を構築できる
    void NewFrame();

    /// @brief ImGui の描画コマンドをコマンドリストに積む
    ///        RTV をセットした後（BeginFrame 後）に呼ぶこと
    void Render(ID3D12GraphicsCommandList* commandList);

    bool IsValid() const { return m_initialized; }

private:
    ComPtr<ID3D12DescriptorHeap> m_srvHeap; // ImGui のフォントテクスチャ用
    bool m_initialized = false;
};
