#pragma once

#include "Common/d3dUtil.h"
#include "Math/BoundingVolume.h"
#include <array>

namespace DX12Engine::Renderer {

// 纯粹的视锥体几何定义，不包含任何剔除逻辑
class Frustum {
public:
    Frustum() = default;

    void BuildFromCamera(const DirectX::XMFLOAT3 &position, const DirectX::XMFLOAT3 &forward,
                         const DirectX::XMFLOAT3 &up,
                         float fovY,        // 垂直视野角（弧度）
                         float aspectRatio, // 宽高比 = 宽度/高度
                         float nearZ,       // 近平面距离（正值）
                         float farZ         // 远平面距离（正值）
    );

    void BuildFromMatrix(const DirectX::XMMATRIX &viewProj);

    // ===== 几何查询 =====

    const std::array<DirectX::XMVECTOR, 6> &GetPlanes() const { return m_planes; }
    const std::array<DirectX::XMFLOAT3, 8> &GetCorners() const { return m_corners; }

    DirectX::XMFLOAT3 GetNearCenter() const { return m_nearCenter; }
    DirectX::XMFLOAT3 GetFarCenter() const { return m_farCenter; }
    DirectX::XMFLOAT3 GetCenterLine() const;

    void GetSectionSize(float distanceFromCamera, float &outWidth, float &outHeight) const;

    /// 测试 AABB 是否与视锥体相交（视锥体包含或部分包含 AABB）
    bool Intersects(const Math::BoundingAABB &aabb) const;

    // ===== 调试/序列化 =====

#ifdef _DEBUG
    struct Parameters {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 forward;
        DirectX::XMFLOAT3 up;
        float fovY;
        float aspectRatio;
        float nearZ;
        float farZ;
        bool isValid;
    };
    Parameters GetParameters() const { return m_params; }

#endif

private:
    std::array<DirectX::XMVECTOR, 6> m_planes;
    std::array<DirectX::XMFLOAT3, 8> m_corners;

    DirectX::XMFLOAT3 m_nearCenter;
    DirectX::XMFLOAT3 m_farCenter;

#ifdef _DEBUG
    Parameters m_params = {};
#endif

    void ComputePlanesFromCorners();
    void NormalizePlanes();
};

} // namespace DX12Engine::Renderer