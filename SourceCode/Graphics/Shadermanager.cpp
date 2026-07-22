#include "ShaderManager.h"
#include <windows.h>
#include <fstream>
#include <iterator>
#include <cstdio>

// dxc（実行時コンパイル）
#include <dxcapi.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// LoadCSO  ―  ビルド済み .cso をバイト列で読む
//-----------------------------------------------------------------------------
std::vector<char> ShaderManager::LoadCSO(const char* csoName)
{
    std::string path = std::string("Shader/") + csoName;
    std::ifstream file(path, std::ios::binary);  
    if (!file)
    {
        char buf[256];
        sprintf_s(buf, "[ShaderManager] CSO が開けません: %s\n", path.c_str());  
        OutputDebugStringA(buf);
        return {};
    }
    return std::vector<char>(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
}

//-----------------------------------------------------------------------------
// CompileFromFile  ―  実行時に .hlsl を dxc でコンパイル
//-----------------------------------------------------------------------------
std::vector<char> ShaderManager::CompileFromFile(const wchar_t* hlslPath,
    const wchar_t* entryPoint,
    const wchar_t* target)
{
    OutputDebugStringW(L"[ShaderManager] 読み込みパス: ");
    OutputDebugStringW(hlslPath);
    OutputDebugStringW(L"\n");

    // dxc のオブジェクトを作る（utils=補助, compiler=本体）
    ComPtr<IDxcUtils>          utils;
    ComPtr<IDxcCompiler3>      compiler;
    ComPtr<IDxcIncludeHandler> includeHandler;
    if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
        FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
    {
        OutputDebugStringW(L"[ShaderManager] dxc 生成失敗（dllが見つからない可能性）\n");
        return {};
    }
    utils->CreateDefaultIncludeHandler(&includeHandler);

    // .hlsl ファイルを読み込む
    ComPtr<IDxcBlobEncoding> source;
    if (FAILED(utils->LoadFile(hlslPath, nullptr, &source)))
    {
        OutputDebugStringW(L"[ShaderManager] .hlsl が開けません: ");
        OutputDebugStringW(hlslPath);
        OutputDebugStringW(L"\n");
        return {};
    }

    // コンパイル引数（-E エントリ, -T ターゲット）
    LPCWSTR args[] =
    {
        hlslPath,                  // 表示用ファイル名
        L"-E", entryPoint,         // エントリポイント
        L"-T", target,             // ターゲット（vs_6_2 等）
        L"-Zi",                    // デバッグ情報（任意）
        L"-Qembed_debug",
    };

    DxcBuffer sourceBuffer = {};
    sourceBuffer.Ptr = source->GetBufferPointer();
    sourceBuffer.Size = source->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_ACP;

    // コンパイル実行
    ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(
        &sourceBuffer, args, _countof(args),
        includeHandler.Get(), IID_PPV_ARGS(&result))))
    {
        OutputDebugStringW(L"[ShaderManager] コンパイル呼び出し失敗\n");
        return {};
    }

    // エラーメッセージを確認
    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0)
    {
        OutputDebugStringA("[ShaderManager] コンパイルエラー/警告:\n");
        OutputDebugStringA(errors->GetStringPointer());
        OutputDebugStringA("\n");
    }

    // コンパイル成否
    HRESULT status;
    result->GetStatus(&status);
    if (FAILED(status))
    {
        OutputDebugStringW(L"[ShaderManager] コンパイル失敗\n");
        return {};
    }

    // コンパイル結果（CSOバイト列）を取り出す
    ComPtr<IDxcBlob> shaderBlob;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderBlob), nullptr);
    if (!shaderBlob)
        return {};

    const char* p = static_cast<const char*>(shaderBlob->GetBufferPointer());
    
    // デバッグ追加
    char dbg[256];
    sprintf_s(dbg, "[ShaderManager] コンパイル成功 サイズ=%zu バイト\n", (size_t)shaderBlob->GetBufferSize());
    OutputDebugStringA(dbg);
    
    return std::vector<char>(p, p + shaderBlob->GetBufferSize());
}

//-----------------------------------------------------------------------------
// CompileLibrary  -  Compile a DXR shader library (lib_6_3) with dxc.
//   A library has no single entry point; all [shader("...")] functions in the
//   file are exported. The resulting DXIL blob is fed to a state object.
//-----------------------------------------------------------------------------
std::vector<char> ShaderManager::CompileLibrary(const wchar_t* hlslPath,
    const wchar_t* target)
{
    OutputDebugStringW(L"[ShaderManager] compile library: ");
    OutputDebugStringW(hlslPath);
    OutputDebugStringW(L"\n");

    ComPtr<IDxcUtils>          utils;
    ComPtr<IDxcCompiler3>      compiler;
    ComPtr<IDxcIncludeHandler> includeHandler;
    if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
        FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
    {
        OutputDebugStringW(L"[ShaderManager] dxc init failed\n");
        return {};
    }
    utils->CreateDefaultIncludeHandler(&includeHandler);

    ComPtr<IDxcBlobEncoding> source;
    if (FAILED(utils->LoadFile(hlslPath, nullptr, &source)))
    {
        OutputDebugStringW(L"[ShaderManager] cannot open .hlsl: ");
        OutputDebugStringW(hlslPath);
        OutputDebugStringW(L"\n");
        return {};
    }

    // No -E for a library; just the library target.
    LPCWSTR args[] =
    {
        hlslPath,
        L"-T", target,
        L"-Zi",
        L"-Qembed_debug",
    };

    DxcBuffer sourceBuffer = {};
    sourceBuffer.Ptr = source->GetBufferPointer();
    sourceBuffer.Size = source->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_ACP;

    ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(
        &sourceBuffer, args, _countof(args),
        includeHandler.Get(), IID_PPV_ARGS(&result))))
    {
        OutputDebugStringW(L"[ShaderManager] library compile call failed\n");
        return {};
    }

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0)
    {
        OutputDebugStringA("[ShaderManager] library compile errors/warnings:\n");
        OutputDebugStringA(errors->GetStringPointer());
        OutputDebugStringA("\n");
    }

    HRESULT status;
    result->GetStatus(&status);
    if (FAILED(status))
    {
        OutputDebugStringW(L"[ShaderManager] library compile failed\n");
        return {};
    }

    ComPtr<IDxcBlob> shaderBlob;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderBlob), nullptr);
    if (!shaderBlob)
        return {};

    const char* p = static_cast<const char*>(shaderBlob->GetBufferPointer());
    return std::vector<char>(p, p + shaderBlob->GetBufferSize());
}