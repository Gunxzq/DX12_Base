#pragma once

#include <DirectXMath.h>

namespace DX12Engine::Math {

// ============================================================================
// 屏幕投影：世界坐标 ↔ 屏幕像素坐标
// 无 UI 依赖（不包含 ImGui），Editor / Engine 通用。
// 逆方向（屏幕 → 世界射线）见 VisibleRaycaster::ScreenToRay。
// ============================================================================

// 世界坐标 → 屏幕像素坐标（视口左上角为原点，Y 轴向下）
/**
 * @brief 将世界坐标投影到屏幕像素坐标
 * @param worldPos 世界空间中的位置向量
 * @param viewProj 视图投影矩阵
 * @param vpMinX 视口左上角屏幕 X
 * @param vpMinY 视口左上角屏幕 Y
 * @param vpWidth 视口宽度（像素）
 * @param vpHeight 视口高度（像素）
 * @return DirectX::XMFLOAT2 屏幕像素坐标
 * @date 2026-08-02
 */
inline DirectX::XMFLOAT2 ProjectToScreen(DirectX::FXMVECTOR worldPos, DirectX::FXMMATRIX viewProj, float vpMinX,
                                         float vpMinY, float vpWidth, float vpHeight) {
    DirectX::XMVECTOR clipPos = DirectX::XMVector3TransformCoord(worldPos, viewProj);
    float sx = (DirectX::XMVectorGetX(clipPos) * 0.5f + 0.5f) * vpWidth + vpMinX;
    float sy = (1.0f - (DirectX::XMVectorGetY(clipPos) * 0.5f + 0.5f)) * vpHeight + vpMinY;
    return DirectX::XMFLOAT2(sx, sy);
}

} // namespace DX12Engine::Math
