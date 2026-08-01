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
                              const DirectX::XMFLOAT3 &up, float fovY, float aspectRatio, float nearZ, float farZ,
                              bool isOrtho, float orthoSize) {
#ifdef _DEBUG
    m_params.position = position;
    m_params.forward = forward;
    m_params.up = up;
    m_params.fovY = fovY;
    m_params.aspectRatio = aspectRatio;
    m_params.nearZ = nearZ;
    m_params.farZ = farZ;
    m_params.isOrtho = isOrtho;
    m_params.orthoSize = orthoSize;
    m_params.isValid = true;
#endif

    // 计算相机坐标系轴
    // 左手系：右向量 = up × forward（+Y × +Z = +X），用标准叉积计算
    DirectX::XMVECTOR pos = DirectX::XMLoadFloat3(&position);
    DirectX::XMVECTOR fwd = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&forward));
    DirectX::XMVECTOR upVec = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&up));
    DirectX::XMVECTOR right = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(upVec, fwd));
    // 重新正交化 up：forward × right = up（标准叉积下 Z × X = +Y）
    // 注意不能用 right × forward（X × Z = -Y，会导致角点 Y 翻转、滚转丢失）
    upVec = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(fwd, right));

    // 计算视野参数
    // 透视：近/远平面尺寸随距离发散；正交：近/远平面尺寸恒定 = orthoSize
    float nearH, nearW, farH, farW;
    if (isOrtho) {
        nearH = orthoSize * 0.5f;
        nearW = nearH * aspectRatio;
        farH = nearH;
        farW = nearW;
    } else {
        float tanHalfFov = tanf(fovY * 0.5f);
        nearH = tanHalfFov * nearZ;
        nearW = nearH * aspectRatio;
        farH = tanHalfFov * farZ;
        farW = farH * aspectRatio;
    }

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

    // 从角点直接构建平面，使用左手系叉积确保法线方向正确
    ComputePlanesFromCorners();
}

void Frustum::BuildFromMatrix(const DirectX::XMMATRIX &viewProj) {
    // Gribb/Hartmann 方法从 View-Projection 矩阵提取平面
    //
    // 标准 Gribb/Hartmann 公式为 OpenGL 右手系设计：
    //   左 = row3+row0, 右 = row3-row0, 下 = row3+row1, 上 = row3-row1,
    //   近 = row3+row2, 远 = row3-row2
    //
    // 对于 DX 左手系 XMMatrixPerspectiveFovLH：
    //   - 近平面直接用 row2（因为 DX 映射 Z 到 [0,1]）
    //   - 左平面符号需要取反：-(row3+row0)
    //   - 其他平面保持与标准公式一致
    //
    // 验证方法：使用 DX 内置 BoundingFrustum::CreateFromMatrix 进行交叉验证。
    //
    // 参考：https://www8.cs.umu.se/kurser/5DV051/HT12/lab/plane_extraction.pdf
    //       Lengyel, "Oblique View Frustums for Shadow Maps and Other Tricks"

    // 提取矩阵行
    DirectX::XMVECTOR row0 = viewProj.r[0];
    DirectX::XMVECTOR row1 = viewProj.r[1];
    DirectX::XMVECTOR row2 = viewProj.r[2];
    DirectX::XMVECTOR row3 = viewProj.r[3];

    // 左平面: -(row3 + row0) — DX 左手系修正
    m_planes[PLANE_LEFT] = DirectX::XMVectorNegate(DirectX::XMVectorAdd(row3, row0));
    // 右平面: row3 - row0
    m_planes[PLANE_RIGHT] = DirectX::XMVectorSubtract(row3, row0);
    // 下平面: row3 + row1
    m_planes[PLANE_BOTTOM] = DirectX::XMVectorAdd(row3, row1);
    // 上平面: row3 - row1
    m_planes[PLANE_TOP] = DirectX::XMVectorSubtract(row3, row1);
    // 近平面: row2 (DX 投影映射 Z 到 [0,1])
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

    // 从角点构建平面，法线方向指向视锥体内部。
    //
    // 使用 XMVector3Cross 计算法线，然后自动校正方向：
    // 取视锥体中心点（near/far 中心的均值），若法线指向远离中心点则取反。
    //
    // 平面方程: N·P + d = 0，正半空间（N·P + d > 0）为内部。

    auto load = [](const XMFLOAT3 &f) { return XMLoadFloat3(&f); };

    // 视锥体中心点（用于判断法线方向）
    XMVECTOR nearC = load(m_nearCenter);
    XMVECTOR farC = load(m_farCenter);
    XMVECTOR frustumCenter = XMVectorScale(XMVectorAdd(nearC, farC), 0.5f);

    auto buildPlane = [&](XMVECTOR A, XMVECTOR B, XMVECTOR C) -> XMVECTOR {
        XMVECTOR edge1 = XMVectorSubtract(B, A);
        XMVECTOR edge2 = XMVectorSubtract(C, A);
        XMVECTOR normal = XMVector3Cross(edge1, edge2);
        // 确保法线指向视锥体内部：从 A 指向 frustumCenter 应与法线同向
        XMVECTOR toCenter = XMVectorSubtract(frustumCenter, A);
        XMVECTOR dotVal = XMVector3Dot(normal, toCenter);
        if (XMVectorGetX(dotVal) < 0.0f) {
            normal = XMVectorNegate(normal);
        }
        return XMPlaneFromPointNormal(A, normal);
    };

    // 左平面
    m_planes[PLANE_LEFT] =
        buildPlane(load(m_corners[CORNER_NEAR_BL]), load(m_corners[CORNER_FAR_BL]), load(m_corners[CORNER_NEAR_TL]));

    // 右平面
    m_planes[PLANE_RIGHT] =
        buildPlane(load(m_corners[CORNER_NEAR_BR]), load(m_corners[CORNER_NEAR_TR]), load(m_corners[CORNER_FAR_BR]));

    // 下平面
    m_planes[PLANE_BOTTOM] =
        buildPlane(load(m_corners[CORNER_NEAR_BL]), load(m_corners[CORNER_NEAR_BR]), load(m_corners[CORNER_FAR_BL]));

    // 上平面
    m_planes[PLANE_TOP] =
        buildPlane(load(m_corners[CORNER_NEAR_TL]), load(m_corners[CORNER_FAR_TL]), load(m_corners[CORNER_NEAR_TR]));

    // 近平面
    m_planes[PLANE_NEAR] =
        buildPlane(load(m_corners[CORNER_NEAR_BL]), load(m_corners[CORNER_NEAR_BR]), load(m_corners[CORNER_NEAR_TL]));

    // 远平面
    m_planes[PLANE_FAR] =
        buildPlane(load(m_corners[CORNER_FAR_BL]), load(m_corners[CORNER_FAR_TL]), load(m_corners[CORNER_FAR_BR]));

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

// ========================================================================
// AABB 相交测试
// ========================================================================

bool Frustum::Intersects(const Math::BoundingAABB &aabb) const {
    using namespace DirectX;

    // 提取 AABB 的 8 个角点
    XMFLOAT3 corners[8];
    corners[0] = {aabb.min.x, aabb.min.y, aabb.min.z};
    corners[1] = {aabb.min.x, aabb.min.y, aabb.max.z};
    corners[2] = {aabb.min.x, aabb.max.y, aabb.min.z};
    corners[3] = {aabb.min.x, aabb.max.y, aabb.max.z};
    corners[4] = {aabb.max.x, aabb.min.y, aabb.min.z};
    corners[5] = {aabb.max.x, aabb.min.y, aabb.max.z};
    corners[6] = {aabb.max.x, aabb.max.y, aabb.min.z};
    corners[7] = {aabb.max.x, aabb.max.y, aabb.max.z};

    for (const auto &plane : m_planes) {
        XMVECTOR planeNormal = XMVectorSet(XMVectorGetX(plane), XMVectorGetY(plane), XMVectorGetZ(plane), 0.0f);
        float planeD = XMVectorGetW(plane);

        // p-vertex: AABB 在平面法线方向上最远的角点
        int px = (XMVectorGetX(planeNormal) >= 0.0f) ? 1 : 0;
        int py = (XMVectorGetY(planeNormal) >= 0.0f) ? 1 : 0;
        int pz = (XMVectorGetZ(planeNormal) >= 0.0f) ? 1 : 0;
        int pIdx = px * 4 + py * 2 + pz;

        XMVECTOR pVertex = XMLoadFloat3(&corners[pIdx]);
        float dot = XMVectorGetX(XMVector3Dot(pVertex, planeNormal));
        if (dot + planeD < 0.0f)
            return false;
    }
    return true;
}

} // namespace DX12Engine::Renderer