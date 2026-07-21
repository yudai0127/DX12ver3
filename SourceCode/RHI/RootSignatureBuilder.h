#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// RootSignatureBuilder
//  RootSignature を組み立てる共通ビルダー。
//  各クラスで重複していた「params配列→シリアライズ→CreateRootSignature」を
//  まとめたもの。パラメータの追加だけを指定すればよい。
//
//  使い方:
//    RootSignatureBuilder builder;
//    builder.AddCBV(0)          // b0
//           .AddCBV(1)          // b1
//           .AddSRV(0)          // t0（テクスチャ）
//           .AddStaticSampler(0); // s0
//    m_rootSig = builder.Build(L"MyRootSignature");
//
//  bindless（テクスチャ配列）など特殊な構成は、専用メソッドで対応する。
//-----------------------------------------------------------------------------
class RootSignatureBuilder
{
public:
    /// @brief 定数バッファ(CBV)を追加（register bN）
    /// @param shaderRegister レジスタ番号（b0なら0）
    /// @param visibility どのシェーダーステージで使うか
    RootSignatureBuilder& AddCBV(
        UINT shaderRegister,
        D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL);

    /// @brief テクスチャ等(SRV)を1枚、ディスクリプタテーブルとして追加（register tN）
    RootSignatureBuilder& AddSRVTable(
        UINT baseRegister, UINT count = 1,
        D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_PIXEL);

    /// @brief bindless 用：StructuredBuffer(t0) + unbounded テクスチャ配列(t1?) を
    ///        1つのテーブルとして追加（GltfModel の PBR 用）
    RootSignatureBuilder& AddBindlessTable(
        D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_PIXEL);

    /// @brief 静的サンプラーを追加（register sN）
    /// @param filter サンプリング方法（既定 LINEAR、ドット絵等は POINT）
    RootSignatureBuilder& AddStaticSampler(
        UINT shaderRegister,
        D3D12_FILTER filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR);

    /// @brief RootSignature を生成する
    ComPtr<ID3D12RootSignature> Build(const wchar_t* debugName = nullptr);

private:
    // ディスクリプタレンジは寿命を保つ必要があるので保持しておく
    std::vector<D3D12_DESCRIPTOR_RANGE>   m_ranges;
    std::vector<D3D12_ROOT_PARAMETER>     m_params;
    std::vector<D3D12_STATIC_SAMPLER_DESC> m_samplers;

    // AddSRVTable / AddBindlessTable はレンジを後から参照するので、
    // どのパラメータがどのレンジを使うかを後で結びつける
    struct PendingTable { size_t paramIndex; size_t rangeStart; size_t rangeCount; };
    std::vector<PendingTable> m_pendingTables;
};
