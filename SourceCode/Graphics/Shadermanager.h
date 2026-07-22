#pragma once
#include <vector>
#include <string>

//-----------------------------------------------------------------------------
// ShaderManager
//  シェーダーの読み込み・コンパイルを担当するシングルトン。
//
//  2つの方式を持つ:
//    LoadCSO        … ビルド時にコンパイル済みの .cso をそのまま読む（従来）
//    CompileFromFile … 実行時に .hlsl を dxc でコンパイルする（ホットリロード用）
//
//  どちらも結果は「CSOバイト列」で、PSO に渡して使う。
//-----------------------------------------------------------------------------
class ShaderManager
{
public:
    static ShaderManager* Instance()
    {
        static ShaderManager inst;
        return &inst;
    }

    /// @brief コンパイル済みシェーダー (.cso) をバイト列で読み込む
    std::vector<char> LoadCSO(const char* csoName);

    /// @brief .hlsl を実行時に dxc でコンパイルして CSO バイト列を得る
    ///        ホットリロード用。失敗時は空の vector を返す。
    /// @param hlslPath  .hlsl ファイルのパス（例: "shaders/GltfModel_PS.hlsl"）
    /// @param entryPoint エントリ関数名（通常 "main"）
    /// @param target    シェーダーターゲット（例: "vs_6_2", "ps_6_2"）
    std::vector<char> CompileFromFile(const wchar_t* hlslPath,
        const wchar_t* entryPoint,
        const wchar_t* target);

    /// @brief Compile a DXR shader library (target "lib_6_3", no entry point).
    ///        Returns the DXIL library blob used to build an RT state object.
    ///        Empty vector on failure.
    /// @param hlslPath  path to the .hlsl containing [shader("...")] functions
    /// @param target    library target ("lib_6_3" by default)
    std::vector<char> CompileLibrary(const wchar_t* hlslPath,
        const wchar_t* target = L"lib_6_3");

private:
    ShaderManager() = default;
    ~ShaderManager() = default;
    ShaderManager(const ShaderManager&) = delete;
    ShaderManager& operator=(const ShaderManager&) = delete;
};
