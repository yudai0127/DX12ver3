#pragma once
#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// GpuBuffer ヘルパー
//  各クラスで重複していた「CreateCommittedResource でバッファを作る」
//  定型コードをまとめた関数群。
//
//  DX12 のバッファ種別:
//    UPLOAD  … CPUから書き込める（頂点・定数・毎フレーム変わるデータ）
//    DEFAULT … GPU専用で高速（変化しないデータ。アップロードには別途コピーが要る）
//-----------------------------------------------------------------------------
namespace GpuBuffer
{
    /// @brief UPLOAD バッファを作る（CPUから書き込み可能）
    /// @param byteSize バッファのサイズ
    /// @return 作成したリソース（失敗時 nullptr）
    ComPtr<ID3D12Resource> CreateUpload(UINT64 byteSize);

    /// @brief UPLOAD バッファを作り、Map したアドレスを返す
    /// @param byteSize  バッファのサイズ
    /// @param outMapped Map したCPUアドレスがここに入る
    ComPtr<ID3D12Resource> CreateUploadMapped(UINT64 byteSize, void** outMapped);

    /// @brief UPLOAD バッファを作り、データをコピーして返す（頂点/インデックス用）
    /// @param data     コピー元データ
    /// @param byteSize データのサイズ
    ComPtr<ID3D12Resource> CreateUploadWithData(const void* data, UINT64 byteSize);

    /// @brief DEFAULT バッファを作る（GPU専用・高速）
    ComPtr<ID3D12Resource> CreateDefault(
        UINT64 byteSize,
        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON);
}
