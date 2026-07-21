#include "ImGui/ShaderEditor.h"
#include "Component/ModelRenderer.h"
#include "imgui.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

//-----------------------------------------------------------------------------
// Initialize
//-----------------------------------------------------------------------------
void ShaderEditor::Initialize(const std::string& hlslDirectory, ModelRenderer* target)
{
    m_directory = hlslDirectory;
    m_target = target;
    m_text.resize(BUFFER_SIZE);
    ScanFiles();
}

//-----------------------------------------------------------------------------
// ScanFiles  ―  HLSLフォルダ内の .hlsl / .hlsli を一覧化
//-----------------------------------------------------------------------------
void ShaderEditor::ScanFiles()
{
    m_files.clear();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(m_directory, ec))
    {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        if (ext == ".hlsl" || ext == ".hlsli")
            m_files.push_back(entry.path().filename().string());
    }
    std::sort(m_files.begin(), m_files.end());
}

//-----------------------------------------------------------------------------
// ReadFile / WriteFile
//-----------------------------------------------------------------------------
std::string ShaderEditor::ReadFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool ShaderEditor::WriteFile(const std::string& path, const std::string& text)
{
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << text;
    return true;
}

//-----------------------------------------------------------------------------
// LoadSelected  ―  選択中ファイルを編集バッファに読み込む
//-----------------------------------------------------------------------------
void ShaderEditor::LoadSelected()
{
    if (m_selected < 0 || m_selected >= (int)m_files.size()) return;
    std::string path = m_directory + "/" + m_files[m_selected];
    std::string content = ReadFile(path);

    m_text.assign(BUFFER_SIZE, '\0');
    memcpy(m_text.data(), content.c_str(),
        (std::min)(content.size(), (size_t)BUFFER_SIZE - 1));
}

//-----------------------------------------------------------------------------
// SaveSelected  ―  編集バッファを選択中ファイルに書き込む
//-----------------------------------------------------------------------------
bool ShaderEditor::SaveSelected()
{
    if (m_selected < 0 || m_selected >= (int)m_files.size()) return false;
    std::string path = m_directory + "/" + m_files[m_selected];
    return WriteFile(path, std::string(m_text.c_str()));
}

//-----------------------------------------------------------------------------
// DrawImGui
//-----------------------------------------------------------------------------
void ShaderEditor::DrawImGui()
{
    ImGui::Begin("Shader Editor");

    // ---- ファイル選択（ドロップダウン）-----------------------------
    std::string current = (m_selected >= 0 && m_selected < (int)m_files.size())
        ? m_files[m_selected] : "(select a file)";
    if (ImGui::BeginCombo("File", current.c_str()))
    {
        for (int i = 0; i < (int)m_files.size(); ++i)
        {
            bool selected = (m_selected == i);
            if (ImGui::Selectable(m_files[i].c_str(), selected))
            {
                m_selected = i;
                LoadSelected();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) ScanFiles(); // フォルダを再スキャン

    // ---- 保存・リロード --------------------------------------------
    if (ImGui::Button("Save & Reload"))
    {
        if (m_selected < 0)
            sprintf_s(m_status, "No file selected");
        else if (SaveSelected())
        {
            // GltfModel が使うシェーダーをリロード（連動）
            if (m_target) m_target->ReloadShaders();
            sprintf_s(m_status, "Saved and reloaded: %s",
                m_files[m_selected].c_str());
        }
        else
            sprintf_s(m_status, "Save failed");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload from File")) LoadSelected();

    if (m_status[0]) ImGui::TextWrapped("%s", m_status);

    ImGui::Separator();


    // ---- テキストエディタ ------------------------------------------
    if (m_selected >= 0)
    {
        ImVec2 size = ImVec2(-FLT_MIN, -FLT_MIN);
        ImGui::InputTextMultiline("##editor", m_text.data(), BUFFER_SIZE, size);
    }
    else
    {
        ImGui::TextWrapped("Select a shader to edit from the dropdown above");
    }

    ImGui::End();
}