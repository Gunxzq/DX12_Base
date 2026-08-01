#include "EditorGizmoSystem.h"
#include "ECS/Core/Components.h"
#include "ECS/Core/Components/Camera.h"
#include "ECS/Core/Components/Transform.h"
#include "ECS/Core/Registry.h"
#include "ImGuizmo.h"
#include "Math/BoundingVolume.h"
#include "Renderer/Debug/WireframeManager.h"
#include "Renderer/Scene/CameraManager.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include "Scene/SceneManager.h"
#include "ThirdParty/imgui/imgui.h"
#include <DirectXMath.h>
#include <algorithm> // std::max / std::min（显示用 far/near 截断）
#include <cmath>

// ========================================================================
// EditorGizmoSystem — 编辑器视口 Gizmo 操纵器 + 选中实体可视化
//
// 职责边界（绘制/计算分离后）：
//   - ImGuizmo 操纵器：叠加绘制 + 回写 TransformComponent（保留）
//   - 选中实体可视化（AABB 线框 / 相机视锥体）：改为声明式收集——
//     AddAABB/AddFrustum 写入 WireframeManager 的 CPU 线列表，
//     实际绘制（3D 线框几何体 + GS 展开 + 深度）由 WireframeManager
//     在渲染阶段固定录制点统一完成。Gizmo 不再持有 ImDrawList 投影绘制。
// ========================================================================

/**
 * @brief 初始化 Gizmo 系统
 * @param context 游戏上下文
 * @date 2026-08-01
 */
void EditorGizmoSystem::Initialize(DX12Engine::Boot::GameContext *context) {
    if (m_initialized)
        return;
    m_context = context;
    m_initialized = true;
}

/**
 * @brief 关闭 Gizmo 系统
 * @date 2026-08-01
 */
void EditorGizmoSystem::Shutdown() {
    m_initialized = false;
    m_context = nullptr;
}

void EditorGizmoSystem::DrawGizmo(ImVec2 viewportMin, ImVec2 viewportMax) {
    if (!m_initialized || !m_context || !m_visible)
        return;

    // 获取选中实体
    DX12Engine::ECS::Entity selectedEntity = DX12Engine::ECS::INVALID_ENTITY;
    if (m_getSelectedEntity) {
        selectedEntity = m_getSelectedEntity();
    }
    if (selectedEntity == DX12Engine::ECS::INVALID_ENTITY)
        return;

    // 获取 Camera 的 View/Proj 矩阵
    auto &cameraMgr = DX12Engine::Renderer::CameraManager::GetInstance();
    const auto &camera = cameraMgr.GetMainCamera();

    // 获取选中实体的 TransformComponent
    DX12Engine::ECS::Registry *registry = nullptr;
    if (m_context && m_context->SceneMgr) {
        registry = m_context->SceneMgr->GetRegistry();
    }
    auto *tc = registry ? registry->TryGetComponent<DX12Engine::ECS::TransformComponent>(selectedEntity) : nullptr;
    if (!tc)
        return;

    // 计算变换矩阵
    DirectX::XMMATRIX worldMatrix = tc->GetMatrix();

    // ── 绘制选中实体的世界空间 AABB 线框（用于调试射线检测） ──
    auto *meshComp = registry->TryGetComponent<DX12Engine::ECS::MeshComponent>(selectedEntity);
    if (meshComp) {
        // 提取 BoundingAABB（如果是其他类型则降级）
        DX12Engine::Math::BoundingAABB localAABB;
        bool validAABB = std::visit(
            [&](const auto &bounds) -> bool {
                using T = std::decay_t<decltype(bounds)>;
                if constexpr (std::is_same_v<T, DX12Engine::Math::BoundingAABB>) {
                    localAABB = bounds;
                    return true;
                } else if constexpr (std::is_same_v<T, DX12Engine::Math::BoundingOBB>) {
                    localAABB = bounds.ToAABB();
                    return true;
                } else if constexpr (std::is_same_v<T, DX12Engine::Math::BoundingSphere>) {
                    // 球体 → 外接 AABB
                    localAABB.min = {bounds.center.x - bounds.radius, bounds.center.y - bounds.radius,
                                     bounds.center.z - bounds.radius};
                    localAABB.max = {bounds.center.x + bounds.radius, bounds.center.y + bounds.radius,
                                     bounds.center.z + bounds.radius};
                    return true;
                }
                return false;
            },
            meshComp->localBounds);

        if (validAABB) {
            // 声明式收集：写入 WireframeManager 的 CPU 线列表（渲染阶段统一绘制）
            // 黄色 AABB（对应原 IM_COL32(255,255,0,200)）
            DX12Engine::Renderer::WireframeManager::GetInstance().AddAABB(localAABB, worldMatrix,
                                                                          {1.0f, 1.0f, 0.0f, 0.78f});
        }
    }

    // ── 绘制选中实体的相机视锥体线框（如果该实体有 CameraComponent） ──
    auto *camComp = registry->TryGetComponent<DX12Engine::ECS::CameraComponent>(selectedEntity);
    if (camComp) {
        using namespace DirectX;

        float vpW = viewportMax.x - viewportMin.x;
        float vpH = viewportMax.y - viewportMin.y;
        float aspect = (vpH > 0.0f) ? (vpW / vpH) : (16.0f / 9.0f);

        // 从实体的 Transform 提取相机位置与朝向（相机局部朝向 +Z）
        XMVECTOR quat = XMLoadFloat4(&tc->rotation);
        XMFLOAT3 forward, up;
        XMStoreFloat3(&forward, XMVector3Rotate(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), quat));
        XMStoreFloat3(&up, XMVector3Rotate(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), quat));

        // 用引擎 Frustum 构建世界空间角点（透视/正交统一，计算层唯一来源）
        // 显示用裁剪面限制（行业通常，Blender/Unity 默认远裁剪面 1000）：
        //   5700 这类极端 far 会让远平面矩形投影缩成一点、线段不可见，故截断显示
        const float kDisplayFarLimit = 1000.0f;
        float displayNear = std::max(camComp->nearPlane, 0.01f);
        float displayFar = std::min(camComp->farPlane, kDisplayFarLimit);
        if (displayFar <= displayNear)
            displayFar = displayNear + 1.0f;

        DX12Engine::Renderer::Frustum frustum;
        frustum.BuildFromCamera(tc->position, forward, up, XMConvertToRadians(camComp->fov), aspect, displayNear,
                                displayFar, camComp->projection == DX12Engine::ECS::ProjectionType::Orthographic,
                                camComp->orthoSize);

        // 声明式收集：近/远裁剪面与锥角汇聚线不同色相（近亮青 / 远橙黄 / 汇聚线中性白）
        // 汇聚线从相机位置出发指向远平面 4 角点（Blender 风格，视觉上定位相机）
        DX12Engine::Renderer::WireframeManager::GetInstance().AddFrustum(
            frustum, tc->position, {0.0f, 0.9f, 1.0f, 1.0f}, {1.0f, 0.67f, 0.2f, 0.86f}, {0.75f, 0.84f, 1.0f, 0.67f});
    }

    // 获取 Gizmo 操作模式（从 EditorViewportToolbar 回调）
    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    if (m_getGizmoOp) {
        operation = static_cast<ImGuizmo::OPERATION>(m_getGizmoOp());
    }
    // 无操作模式（Cursor 等），不绘制 Gizmo
    if (operation == 0)
        return;

    // 设置 ImGuizmo 视口矩形（与视口图像区域对齐）
    float gizmoWidth = viewportMax.x - viewportMin.x;
    float gizmoHeight = viewportMax.y - viewportMin.y;
    if (gizmoWidth <= 0.0f || gizmoHeight <= 0.0f)
        return;

    ImGuizmo::SetRect(viewportMin.x, viewportMin.y, gizmoWidth, gizmoHeight);
    ImGuizmo::SetDrawlist();

    // 调用 ImGuizmo 操纵器
    DirectX::XMMATRIX deltaMatrix = DirectX::XMMatrixIdentity();
    ImGuizmo::Manipulate(&camera.ViewMatrix.r->m128_f32[0], &camera.ProjMatrix.r->m128_f32[0], operation,
                         ImGuizmo::MODE::WORLD, &worldMatrix.r->m128_f32[0], &deltaMatrix.r->m128_f32[0]);

    // 如果正在操作，更新 TransformComponent
    if (ImGuizmo::IsUsing()) {
        // 分解矩阵回 position/rotation/scale
        DirectX::XMVECTOR newPos, newScale, newRotQuat;
        DirectX::XMMatrixDecompose(&newScale, &newRotQuat, &newPos, worldMatrix);

        DirectX::XMStoreFloat3(&tc->position, newPos);
        DirectX::XMStoreFloat4(&tc->rotation, newRotQuat); // 四元数，零转换
        DirectX::XMStoreFloat3(&tc->scale, newScale);
    }
}

// ========================================================================
// Axis Gizmo — 视口右上角的 XYZ 轴向指示器
// 组合：ImGuizmo::ViewManipulate 立方体 + ImDrawList 轴标签叠加
// ========================================================================

void EditorGizmoSystem::DrawViewCube(ImVec2 viewportMin, ImVec2 viewportMax) {
    if (!m_initialized || !m_context)
        return;

    auto &cameraMgr = DX12Engine::Renderer::CameraManager::GetInstance();
    const auto &camera = cameraMgr.GetMainCamera();

    float viewWidth = viewportMax.x - viewportMin.x;
    float viewHeight = viewportMax.y - viewportMin.y;
    if (viewWidth <= 0.0f || viewHeight <= 0.0f)
        return;

    // 复制当前视图矩阵到 float[16]（ViewManipulate 会就地修改）
    DirectX::XMMATRIX viewMatrix = camera.ViewMatrix;
    float viewArr[16];
    memcpy(viewArr, &viewMatrix, sizeof(viewArr));

    // 在视口右上角定位 ViewCube
    float cubeSize = 100.0f;
    float margin = 12.0f;
    ImVec2 cubePos(viewportMax.x - cubeSize - margin, viewportMin.y + margin);
    ImVec2 cubeSizeVec(cubeSize, cubeSize);

    // ── 1. 绘制 ImGuizmo ViewCube（立方体 + 面点击） ──
    ImGuizmo::SetRect(viewportMin.x, viewportMin.y, viewWidth, viewHeight);
    ImGuizmo::SetDrawlist();
    ImGuizmo::ViewManipulate(viewArr, 8.0f, cubePos, cubeSizeVec, 0x18181818);

    // ── 2. 检测 ViewManipulate 是否修改了视图矩阵 ──
    bool viewChanged = (memcmp(viewArr, &viewMatrix, sizeof(viewArr)) != 0);

    // 从视图矩阵提取基向量（用于叠加轴线和标签）
    DirectX::XMVECTOR viewR0 = camera.ViewMatrix.r[0];
    DirectX::XMVECTOR viewR1 = camera.ViewMatrix.r[1];
    DirectX::XMVECTOR viewR2 = camera.ViewMatrix.r[2];

    float rX = viewR0.m128_f32[0], uX = viewR0.m128_f32[1], fX = viewR0.m128_f32[2];
    float rY = viewR1.m128_f32[0], uY = viewR1.m128_f32[1], fY = viewR1.m128_f32[2];
    float rZ = viewR2.m128_f32[0], uZ = viewR2.m128_f32[1], fZ = viewR2.m128_f32[2];

    // 世界轴在相机空间的方向向量（取屏幕 XY 分量 + Z 深度分量）
    struct AxisDir {
        float dx, dy, dz;
    };
    AxisDir axes[3] = {
        {rX, rY, rZ}, // X
        {uX, uY, uZ}, // Y
        {fX, fY, fZ}, // Z
    };
    ImU32 colors[3] = {IM_COL32(255, 80, 80, 240), IM_COL32(80, 220, 80, 240), IM_COL32(80, 120, 255, 240)};
    const char *labels[3] = {"X", "Y", "Z"};

    // ── 3. 叠加轴线和标签（从立方体中心向外延伸） ──
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float cx = cubePos.x + cubeSize * 0.5f;
    float cy = cubePos.y + cubeSize * 0.5f;
    float lineExtend = cubeSize * 0.55f; // 从中心伸出立方体边缘的长度

    for (int i = 0; i < 3; ++i) {
        float dx = axes[i].dx, dy = axes[i].dy, dz = axes[i].dz;
        float len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (len < 1e-6f)
            continue;

        float nx = dx / len, ny = dy / len;
        bool facing = (dz >= 0);

        // 线段端点：从立方体边缘延伸到外部
        float innerR = cubeSize * 0.45f; // 立方体内切圆半径
        ImVec2 startPt(cx + nx * innerR, cy + ny * innerR);
        ImVec2 endPt(cx + nx * lineExtend, cy + ny * lineExtend);

        ImU32 col = facing ? colors[i] : IM_COL32(120, 120, 120, 100);
        ImU32 colLabel = facing ? colors[i] : IM_COL32(160, 160, 160, 160);

        // 轴线
        dl->AddLine(startPt, endPt, col, 2.5f);

        // 端点小圆点
        dl->AddCircleFilled(endPt, 3.0f, col);

        // 标签（在端点外侧偏移一点）
        ImVec2 labelPos(endPt.x + nx * 4.0f, endPt.y + ny * 4.0f);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.9f, labelPos, colLabel, labels[i]);
    }

    // ── 4. 如果视图矩阵被 ViewCube 修改，同步回 CameraManager ──
    if (viewChanged) {
        // 从修改后的 viewArr 提取基向量（按列，与 CalculateMatrices 布局一致）
        DirectX::XMFLOAT3 newRight, newUp, newForward;
        newRight.x = viewArr[0];
        newRight.y = viewArr[4];
        newRight.z = viewArr[8];
        newUp.x = viewArr[1];
        newUp.y = viewArr[5];
        newUp.z = viewArr[9];
        newForward.x = viewArr[2];
        newForward.y = viewArr[6];
        newForward.z = viewArr[10];

        // 更新相机（非 const 引用）
        auto &cam = const_cast<Camera &>(camera);
        cam.Forward = newForward;
        cam.Up = newUp;
        cam.Right = newRight;

        cam.Rotation.x = asinf(-newForward.y);
        cam.Rotation.y = atan2f(newForward.x, newForward.z);
        cam.Rotation.z = 0.0f;

        cameraMgr.UpdateMainCamera();
    }
}