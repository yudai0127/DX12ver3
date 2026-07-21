#pragma once
#include <string>
#include <vector>

class ModelRenderer;

//-----------------------------------------------------------------------------
// ShaderEditor
//  ImGui 上でシェーダー(.hlsl/.hlsli)を編集する汎用エディタ。
//  HLSLフォルダ内のファイルを一覧から選んで編集・保存できる。
//  保存後、GltfModelが使うシェーダーならリロードも連動する。
//-----------------------------------------------------------------------------
class ShaderEditor
{
public:
    /// @brief 初期化。HLSLフォルダのパスと、リロード連動するモデルを設定
    /// @param hlslDirectory .hlslが入っているフォルダ（例: "D:/DX12/HLSL"）
    /// @param target        保存時にリロードするモデル（任意）
    void Initialize(const std::string& hlslDirectory, ModelRenderer* target);

    /// @brief ImGui ウィンドウを描画する（毎フレーム呼ぶ）
    void DrawImGui();

private:
    void ScanFiles();                 // フォルダ内の .hlsl/.hlsli を一覧化
    void LoadSelected();              // 選択中ファイル → 編集バッファ
    bool SaveSelected();              // 編集バッファ → 選択中ファイル
    static std::string ReadFile(const std::string& path);
    static bool        WriteFile(const std::string& path, const std::string& text);

    std::string    m_directory;
    ModelRenderer* m_target = nullptr;

    std::vector<std::string> m_files;   // フォルダ内のファイル名一覧
    int  m_selected = -1;               // 選択中のファイル（m_filesのindex）

    static const int BUFFER_SIZE = 1024 * 64; // 64KB
    std::string m_text;                 // 編集バッファ
    char m_status[256] = "";
};