#include "Camera.h"
#include "RHI/DeviceManager.h"

using namespace DirectX;

//-----------------------------------------------------------------------------
// Initialize  ―  シーン定数バッファ(b1)を作る
//-----------------------------------------------------------------------------
bool Camera::Initialize()
{
    return m_sceneCB.Initialize(sizeof(SceneConstants));
}

//-----------------------------------------------------------------------------
// Update  ―  View・Projection行列を計算して定数バッファに書き込む
//-----------------------------------------------------------------------------
void Camera::Update(float aspectRatio)
{
    // Projection（遠近投影）
    XMMATRIX P = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(fovDegree), aspectRatio, nearZ, farZ);

    // View（カメラの視点）
    XMVECTOR e = XMVectorSet(eye.x, eye.y, eye.z, 1.0f);
    XMVECTOR f = XMVectorSet(focus.x, focus.y, focus.z, 1.0f);
    XMVECTOR u = XMVectorSet(up.x, up.y, up.z, 0.0f);
    XMMATRIX V = XMMatrixLookAtLH(e, f, u);

    // 定数バッファへ書き込み（View × Projection を合成）
    SceneConstants data = {};
    XMStoreFloat4x4(&data.view_projection, V * P);
    data.camera_position = { eye.x, eye.y, eye.z, 1.0f };
    data.light_direction = lightDir;
    data.light_color = lightColor;
    data.ambient_color = ambient;

    m_sceneCB.Update(data);
}