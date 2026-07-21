#pragma once
#define NOMINMAX
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <string>
#include <memory>
#include "Graphics/Texture.h"
#include "RHI/ConstantBuffer.h"

using Microsoft::WRL::ComPtr;

//-----------------------------------------------------------------------------
// GltfModel（段階2：basecolorテクスチャ対応）
//   tinygltf で glTF を読み込み、position/normal/texcoord とインデックスを
//   取得し、マテリアルの basecolor テクスチャを貼って描画する。
//-----------------------------------------------------------------------------
class GltfModel
{
public:
    // 段階3で使う頂点（位置・法線・接線・テクスチャ座標）
    struct Vertex
    {
        DirectX::XMFLOAT3 position = { 0, 0, 0 };
        DirectX::XMFLOAT3 normal = { 0, 0, 1 };
        DirectX::XMFLOAT4 tangent = { 1, 0, 0, 1 };
        DirectX::XMFLOAT2 texcoord = { 0, 0 };
    };

    // シェーダーに送るマテリアル情報（GltfModel.hlsli の MaterialData と一致）
    struct MaterialData
    {
        DirectX::XMFLOAT4 basecolor_factor = { 1, 1, 1, 1 };
        float metallic_factor = 1.0f;
        float roughness_factor = 1.0f;
        int   basecolor_texture = -1;
        int   metallic_roughness_texture = -1;
        int   normal_texture = -1;
        int   emissive_texture = -1;
        int   occlusion_texture = -1;
        float normal_scale = 1.0f;
        DirectX::XMFLOAT3 emissive_factor = { 0, 0, 0 };
        float occlusion_strength = 1.0f;
    };

    // ノード（glTFの階層構造）
    struct Node
    {
        std::string name;
        int         mesh = -1;
        std::vector<int> children;

        DirectX::XMFLOAT4 rotation = { 0, 0, 0, 1 };
        DirectX::XMFLOAT3 scale = { 1, 1, 1 };
        DirectX::XMFLOAT3 translation = { 0, 0, 0 };
        DirectX::XMFLOAT4X4 global_transform =
        { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    };

    struct Scene
    {
        std::string name;
        std::vector<int> nodes;
    };

    // プリミティブ（1メッシュ内の描画単位）
    struct Primitive
    {
        ComPtr<ID3D12Resource>   vertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW vbView = {};
        ComPtr<ID3D12Resource>   indexBuffer;
        D3D12_INDEX_BUFFER_VIEW  ibView = {};
        uint32_t                 indexCount = 0;
        int                      material = -1;
        int                      has_tangent = 0;

        ConstantBuffer           objectCB; // オブジェクト定数(b0)
    };

    struct Mesh
    {
        std::string name;
        std::vector<Primitive> primitives;
    };

    // glTF の texture（どの画像を指すか）
    struct GltfTexture
    {
        int source = -1; // images のインデックス
    };

    explicit GltfModel(const std::string& filename);
    ~GltfModel() = default;

    GltfModel(const GltfModel&) = delete;
    GltfModel& operator=(const GltfModel&) = delete;

    bool IsValid() const { return m_pipelineState != nullptr; }

    /// @brief モデルを描画する
    /// @param world   モデル全体のワールド行列
    /// @param color   マテリアル色
    /// @param sceneCB シーン定数バッファ(b1)のGPUアドレス
    void Render(ID3D12GraphicsCommandList* commandList,
        const DirectX::XMFLOAT4X4& world,
        const DirectX::XMFLOAT4& color,
        D3D12_GPU_VIRTUAL_ADDRESS  sceneCB);

    /// @brief .hlsl を再コンパイルして PSO を作り直す（ホットリロード）
    void ReloadShaders();

    /// @brief .hlsl の更新を検知して、変わっていたら自動リロードする
    ///        毎フレーム呼ぶ。保存するだけで反映される。
    void CheckAutoReload();

    /// @brief 監視している VS/PS の .hlsl パス（シェーダーエディタ用）
    const std::wstring& GetVSPath() const { return m_vsPath; }
    const std::wstring& GetPSPath() const { return m_psPath; }

private:
    bool CreateRootSignature(ID3D12Device* device);
    bool CreatePipelineState(ID3D12Device* device, DXGI_FORMAT rtvFormat);
    void CumulateTransforms();  // ノード階層のワールド行列を計算

    std::vector<Scene> m_scenes;
    std::vector<Node>  m_nodes;
    std::vector<Mesh>  m_meshes;
    std::vector<MaterialData> m_materials;   // シェーダーに送るマテリアル
    std::vector<GltfTexture>  m_textures;
    int                m_defaultScene = 0;

    // 画像（images）ごとに作ったテクスチャ
    std::vector<std::unique_ptr<Texture>> m_images;
    std::unique_ptr<Texture> m_dummyWhite;

    std::string m_directory;

    // マテリアル配列の StructuredBuffer（t0）
    ComPtr<ID3D12Resource> m_materialBuffer;
    void* m_mappedMaterials = nullptr;

    // bindless 用：マテリアルバッファ + 全テクスチャを連続配置した SRV 範囲
    //   先頭ハンドル（GPU）をテーブルにバインドする
    ComPtr<ID3D12DescriptorHeap> m_srvTable;   // このモデル専用の連続SRVヒープ
    D3D12_GPU_DESCRIPTOR_HANDLE  m_materialSrvGpu = {}; // materials(t0)の位置
    D3D12_GPU_DESCRIPTOR_HANDLE  m_textureSrvGpu = {}; // material_textures[0](t1)の位置

    void BuildResourceTable(); // StructuredBuffer・テクスチャSRVを連続配置

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;
    bool m_hotReload = false; // PSO生成時に.hlslを実行時コンパイルするか

    // ホットリロード用：監視する .hlsl のパスと最終更新時刻
    std::wstring m_vsPath = L"HLSL/GltfModel_VS.hlsl";
    std::wstring m_psPath = L"HLSL/GltfModel_PS.hlsl";
    long long    m_vsWriteTime = 0;
    long long    m_psWriteTime = 0;
    long long    GetFileWriteTime(const std::wstring& path) const;
};