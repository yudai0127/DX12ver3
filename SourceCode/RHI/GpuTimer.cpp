#include "RHI/GpuTimer.h"
#include <windows.h>

//-----------------------------------------------------------------------------
// Initialize
//-----------------------------------------------------------------------------
bool GpuTimer::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
    uint32_t frameCount)
{
    m_frameCount = frameCount;

    // GPUのタイムスタンプ周波数を取得（1秒あたりのカウント数）
    //   これがないと「カウント差」を「秒」に変換できない
    if (FAILED(queue->GetTimestampFrequency(&m_gpuFrequency)))
    {
        OutputDebugStringW(L"[GpuTimer] タイムスタンプ非対応のキューです\n");
        return false;
    }

    // ---- クエリヒープ（GPUが時刻を書き込む場所）--------------------
    D3D12_QUERY_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = frameCount * TIMESTAMPS_PER_FRAME; // フレーム分×2
    if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_queryHeap))))
    {
        OutputDebugStringW(L"[GpuTimer] クエリヒープ生成失敗\n");
        return false;
    }
    m_queryHeap->SetName(L"GpuTimerQueryHeap");

    // ---- Readback バッファ（CPUが結果を読む場所）------------------
    //   READBACK ヒープは「GPU→CPU」方向の転送用
    const UINT64 bufferSize =
        sizeof(uint64_t) * frameCount * TIMESTAMPS_PER_FRAME;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bufferSize;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_readbackBuf))))
    {
        OutputDebugStringW(L"[GpuTimer] Readbackバッファ生成失敗\n");
        return false;
    }
    m_readbackBuf->SetName(L"GpuTimerReadback");

    return true;
}

//-----------------------------------------------------------------------------
// Uninitialize
//-----------------------------------------------------------------------------
void GpuTimer::Uninitialize()
{
    m_readbackBuf = nullptr;
    m_queryHeap = nullptr;
}

//-----------------------------------------------------------------------------
// BeginFrame  ―  開始時刻を記録
//-----------------------------------------------------------------------------
void GpuTimer::BeginFrame(ID3D12GraphicsCommandList* cmd, uint32_t frameIndex)
{
    if (!m_queryHeap) return;

    // このフレームが使うクエリ番号（開始 = 2N, 終了 = 2N+1）
    const UINT queryIndex = frameIndex * TIMESTAMPS_PER_FRAME;

    // 「GPUがここを通った時刻を記録せよ」という命令を積む
    cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);
}

//-----------------------------------------------------------------------------
// EndFrame  ―  終了時刻を記録し、結果を Readback バッファへ解決
//-----------------------------------------------------------------------------
void GpuTimer::EndFrame(ID3D12GraphicsCommandList* cmd, uint32_t frameIndex)
{
    if (!m_queryHeap) return;

    const UINT beginIndex = frameIndex * TIMESTAMPS_PER_FRAME;
    const UINT endIndex = beginIndex + 1;

    // 終了時刻を記録
    cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, endIndex);

    // クエリヒープの内容を Readback バッファへコピーする命令を積む
    //   （GPUが実行したとき、結果がバッファに書かれる）
    cmd->ResolveQueryData(
        m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
        beginIndex, TIMESTAMPS_PER_FRAME,
        m_readbackBuf.Get(),
        sizeof(uint64_t) * beginIndex);
}

//-----------------------------------------------------------------------------
// Resolve  ―  結果を読み出して GPU 時間を計算
//   GPUがそのフレームを処理し終わった後に呼ぶこと（WaitForFrame の後）
//-----------------------------------------------------------------------------
void GpuTimer::Resolve(uint32_t frameIndex)
{
    if (!m_readbackBuf || m_gpuFrequency == 0) return;

    const UINT beginIndex = frameIndex * TIMESTAMPS_PER_FRAME;

    // 読み出す範囲だけ Map する
    const UINT64 offset = sizeof(uint64_t) * beginIndex;
    D3D12_RANGE readRange = { (SIZE_T)offset,
                              (SIZE_T)(offset + sizeof(uint64_t) * TIMESTAMPS_PER_FRAME) };

    void* mapped = nullptr;
    if (FAILED(m_readbackBuf->Map(0, &readRange, &mapped))) return;

    const uint64_t* timestamps =
        reinterpret_cast<const uint64_t*>(
            static_cast<const uint8_t*>(mapped) + offset);

    const uint64_t begin = timestamps[0];
    const uint64_t end = timestamps[1];

    // 書き込まれていない（初回など）なら無視
    if (end > begin)
    {
        const uint64_t delta = end - begin;
        // カウント差 ÷ 周波数 = 秒 → ミリ秒に
        m_gpuTimeMs = static_cast<float>(delta) /
            static_cast<float>(m_gpuFrequency) * 1000.0f;
    }

    // 書き込みはしていないので、空の範囲で Unmap
    D3D12_RANGE writeRange = { 0, 0 };
    m_readbackBuf->Unmap(0, &writeRange);
}