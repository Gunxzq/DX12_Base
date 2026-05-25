#include "Frustum.h"
#include <cmath>

namespace DX12Engine::Renderer {

namespace {
constexpr int PLANE_LEFT = 0;   // 左平面
constexpr int PLANE_RIGHT = 1;  // 右平面
constexpr int PLANE_BOTTOM = 2; // 下平面
constexpr int PLANE_TOP = 3;    // 上平面
constexpr int PLANE_NEAR = 4;   // 近平面
constexpr int PLANE_FAR = 5;    // 远平面

// 角点索引
constexpr int CORNER_NEAR_BL = 0; // 近平面左下
constexpr int CORNER_NEAR_BR = 1; // 近平面右下
constexpr int CORNER_NEAR_TL = 2; // 近平面左上
constexpr int CORNER_NEAR_TR = 3; // 近平面右上
constexpr int CORNER_FAR_BL = 4;  // 远平面左下
constexpr int CORNER_FAR_BR = 5;  // 远平面右下
constexpr int CORNER_FAR_TL = 6;  // 远平面左上
constexpr int CORNER_FAR_TR = 7;  // 远平面右上
} // namespace

void Frustum::BuildFromCamera(const DirectX::XMFLOAT3 &position, const DirectX::XMFLOAT3 &forward,
                              const DirectX::XMFLOAT3 &up, float fovY, float aspectRatio, float nearZ, float farZ) {
    // 存储参数

#ifdef _DEBUG
    m_params.position = position;
    m_params.forward = forward;
    m_params.up = up;
    m_params.fovY = fovY;
    m_params.aspectRatio = aspectRatio;
    m_params.nearZ = nearZ;
    m_params.farZ = farZ;
    m_params.isValid = true;
#endif

    // 计算相机坐标系轴
    DirectX::XMVECTOR pos = DirectX::XMLoadFloat3(&position);
    DirectX::XMVECTOR fwd = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&forward));
    DirectX::XMVECTOR upVec = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&up));
    DirectX::XMVECTOR right = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(fwd, upVec));
    upVec = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(right, fwd));

    // 计算视野参数
    float tanHalfFov = tanf(fovY * 0.5f);
    float nearH = tanHalfFov * nearZ;
    float nearW = nearH * aspectRatio;
    float farH = tanHalfFov * farZ;
    float farW = farH * aspectRatio;

    // 近平面中心
    DirectX::XMVECTOR nearCenter = DirectX::XMVectorAdd(pos, DirectX::XMVectorScale(fwd, nearZ));
    // 远平面中心
    DirectX::XMVECTOR farCenter = DirectX::XMVectorAdd(pos, DirectX::XMVectorScale(fwd, farZ));

    // 存储中心点
    DirectX::XMStoreFloat3(&m_nearCenter, nearCenter);
    DirectX::XMStoreFloat3(&m_farCenter, farCenter);

    // 计算近平面四个角
    DirectX::XMVECTOR nearBL = DirectX::XMVectorAdd(
        nearCenter, DirectX::XMVectorAdd(DirectX::XMVectorScale(right, -nearW), DirectX::XMVectorScale(upVec, -nearH)));
    DirectX::XMVECTOR nearBR = DirectX::XMVectorAdd(
        nearCenter, DirectX::XMVectorAdd(DirectX::XMVectorScale(right, nearW), DirectX::XMVectorScale(upVec, -nearH)));
    DirectX::XMVECTOR nearTL = DirectX::XMVectorAdd(
        nearCenter, DirectX::XMVectorAdd(DirectX::XMVectorScale(right, -nearW), DirectX::XMVectorScale(upVec, nearH)));
    DirectX::XMVECTOR nearTR = DirectX::XMVectorAdd(
        nearCenter, DirectX::XMVectorAdd(DirectX::XMVectorScale(right, nearW), DirectX::XMVectorScale(upVec, nearH)));

    // 计算远平面四个角
    DirectX::XMVECTOR farBL = DirectX::XMVectorAdd(
        farCenter, DirectX::XMVectorAdd(DirectX::XMVectorScale(right, -farW), DirectX::XMVectorScale(upVec, -farH)));
    DirectX::XMVECTOR farBR = DirectX::XMVectorAdd(
        farCenter, DirectX::XMVectorAdd(DirectX::XMVectorScale(right, farW), DirectX::XMVectorScale(upVec, -farH)));
    DirectX::XMVECTOR farTL = DirectX::XMVectorAdd(
        farCenter, DirectX::XMVectorAdd(DirectX::XMVectorScale(right, -farW), DirectX::XMVectorScale(upVec, farH)));
    DirectX::XMVECTOR farTR = DirectX::XMVectorAdd(
        farCenter, DirectX::XMVectorAdd(DirectX::XMVectorScale(right, farW), DirectX::XMVectorScale(upVec, farH)));

    // 存储角点
    DirectX::XMStoreFloat3(&m_corners[CORNER_NEAR_BL], nearBL);
    DirectX::XMStoreFloat3(&m_corners[CORNER_NEAR_BR], nearBR);
    DirectX::XMStoreFloat3(&m_corners[CORNER_NEAR_TL], nearTL);
    DirectX::XMStoreFloat3(&m_corners[CORNER_NEAR_TR], nearTR);
    DirectX::XMStoreFloat3(&m_corners[CORNER_FAR_BL], farBL);
    DirectX::XMStoreFloat3(&m_corners[CORNER_FAR_BR], farBR);
    DirectX::XMStoreFloat3(&m_corners[CORNER_FAR_TL], farTL);
    DirectX::XMStoreFloat3(&m_corners[CORNER_FAR_TR], farTR);

    // 从角点计算平面
    ComputePlanesFromCorners();
}

void Frustum::BuildFromMatrix(const DirectX::XMMATRIX &viewProj) {
    // Gribb/Hartmann 方法从 View-Projection 矩阵提取平面
    // 注意：此方法假设 DX 风格的投影矩阵（Z 范围 [0,1]）

    // 提取矩阵行
    DirectX::XMVECTOR row0 = viewProj.r[0];
    DirectX::XMVECTOR row1 = viewProj.r[1];
    DirectX::XMVECTOR row2 = viewProj.r[2];
    DirectX::XMVECTOR row3 = viewProj.r[3];

    // 左平面: row3 + row0
    m_planes[PLANE_LEFT] = DirectX::XMVectorAdd(row3, row0);
    // 右平面: row3 - row0
    m_planes[PLANE_RIGHT] = DirectX::XMVectorSubtract(row3, row0);
    // 下平面: row3 + row1
    m_planes[PLANE_BOTTOM] = DirectX::XMVectorAdd(row3, row1);
    // 上平面: row3 - row1
    m_planes[PLANE_TOP] = DirectX::XMVectorSubtract(row3, row1);
    // 近平面: row2 (因为 DX 投影映射 Z 到 [0,1])
    m_planes[PLANE_NEAR] = row2;
    // 远平面: row3 - row2
    m_planes[PLANE_FAR] = DirectX::XMVectorSubtract(row3, row2);

    // 归一化所有平面
    NormalizePlanes();

#ifdef _DEBUG
    m_params.isValid = false;
#endif
}

void Frustum::ComputePlanesFromCorners() {
    using namespace DirectX;

    // 左平面: 由 nearBL, farBL, nearTL 确定（法线指向视锥体外）
    XMVECTOR normalLeft =
        XMPlaneFromPoints(XMLoadFloat3(&m_corners[CORNER_NEAR_BL]), XMLoadFloat3(&m_corners[CORNER_FAR_BL]),
                          XMLoadFloat3(&m_corners[CORNER_NEAR_TL]));
    m_planes[PLANE_LEFT] = normalLeft;

    // 右平面: nearBR, nearTR, farBR
    XMVECTOR normalRight =
        XMPlaneFromPoints(XMLoadFloat3(&m_corners[CORNER_NEAR_BR]), XMLoadFloat3(&m_corners[CORNER_NEAR_TR]),
                          XMLoadFloat3(&m_corners[CORNER_FAR_BR]));
    m_planes[PLANE_RIGHT] = normalRight;

    // 下平面: nearBL, nearBR, farBL
    XMVECTOR normalBottom =
        XMPlaneFromPoints(XMLoadFloat3(&m_corners[CORNER_NEAR_BL]), XMLoadFloat3(&m_corners[CORNER_NEAR_BR]),
                          XMLoadFloat3(&m_corners[CORNER_FAR_BL]));
    m_planes[PLANE_BOTTOM] = normalBottom;

    // 上平面: nearTL, farTL, nearTR
    XMVECTOR normalTop =
        XMPlaneFromPoints(XMLoadFloat3(&m_corners[CORNER_NEAR_TL]), XMLoadFloat3(&m_corners[CORNER_FAR_TL]),
                          XMLoadFloat3(&m_corners[CORNER_NEAR_TR]));
    m_planes[PLANE_TOP] = normalTop;

    // 近平面: nearTL, nearBL, nearTR
    XMVECTOR normalNear =
        XMPlaneFromPoints(XMLoadFloat3(&m_corners[CORNER_NEAR_TL]), XMLoadFloat3(&m_corners[CORNER_NEAR_BL]),
                          XMLoadFloat3(&m_corners[CORNER_NEAR_TR]));
    m_planes[PLANE_NEAR] = normalNear;

    // 远平面: farTR, farBR, farTL
    XMVECTOR normalFar =
        XMPlaneFromPoints(XMLoadFloat3(&m_corners[CORNER_FAR_TR]), XMLoadFloat3(&m_corners[CORNER_FAR_BR]),
                          XMLoadFloat3(&m_corners[CORNER_FAR_TL]));
    m_planes[PLANE_FAR] = normalFar;

    NormalizePlanes();
}

void Frustum::NormalizePlanes() {
    for (auto &plane : m_planes) {
        plane = DirectX::XMPlaneNormalize(plane);
    }
}

DirectX::XMFLOAT3 Frustum::GetCenterLine() const {
    DirectX::XMFLOAT3 result;
    result.x = m_farCenter.x - m_nearCenter.x;
    result.y = m_farCenter.y - m_nearCenter.y;
    result.z = m_farCenter.z - m_nearCenter.z;
    return result;
}

void Frustum::GetSectionSize(float distanceFromCamera, float &outWidth, float &outHeight) const {
#ifdef _DEBUG
    if (!m_params.isValid || distanceFromCamera <= 0.0f) {
        outWidth = 0.0f;
        outHeight = 0.0f;
        return;
    }

    float tanHalfFov = tanf(m_params.fovY * 0.5f);
    float heightAtDist = distanceFromCamera * tanHalfFov * 2.0f;
    float widthAtDist = heightAtDist * m_params.aspectRatio;
#else
    if (distanceFromCamera <= 0.0f) {
        outWidth = 0.0f;
        outHeight = 0.0f;
        return;
    }
    outWidth = widthAtDist;
    outHeight = heightAtDist;
#endif

    // 非调试模式下无法获取参数，返回默认值
    outWidth = 0.0f;
    outHeight = 0.0f;
}

} // namespace DX12Engine::Renderer