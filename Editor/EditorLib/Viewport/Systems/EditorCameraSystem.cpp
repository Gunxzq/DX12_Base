#include "EditorCameraSystem.h"
#include "EditorViewportInputActions.h"
#include "ECS/Core/Components/Transform.h"
#include "Platform/Input/InputManager.h"
#include "Platform/Input/InputSystem.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/Scene/CameraManager.h"
#include "Scene/SceneManager.h"
#include "Scheduler/FrameDriver.h"
#include "ThirdParty/imgui/imgui.h"

using namespace DX12Engine;
using namespace DX12Engine::Input;
using namespace DirectX;

// ========================================================================
// 初始化/销毁
// ========================================================================

void EditorCameraSystem::Initialize(Boot::GameContext *context) {
    if (m_initialized || !context)
        return;

    m_context = context;
    m_context->Logging->Info("[EditorCameraSystem] Initializing...");

    RegisterInputCallbacks();

    m_initialized = true;
    m_context->Logging->Info("[EditorCameraSystem] Initialized");
}

void EditorCameraSystem::Shutdown() {
    if (!m_initialized)
        return;
    UnregisterInputCallbacks();
    m_initialized = false;
}

// ========================================================================
// 输入回调注册
// ========================================================================

void EditorCameraSystem::RegisterInputCallbacks() {
    m_inputSystem = m_context->InputMgr ? m_context->InputMgr->GetInputSystem() : nullptr;
    if (!m_inputSystem) {
        m_context->Logging->Warn("[EditorCameraSystem] No InputSystem available for callback registration");
        return;
    }

    // Move: WASD 移动相机，WhileHeld 持续触发
    m_callbackIds[0] = m_inputSystem->BindCallback(
        ActionId_Move,
        [this](const InputActionState &state) {
            if (!m_viewportHovered)
                return;

            float deltaTime = m_context->MainTimer->GetDeltaTime();
            float x, y;
            state.GetAxis2D(x, y);

            float currentSpeed = m_moveSpeed * deltaTime;

            // W/S: Walk（前后移动，沿 Forward 方向）
            if (y > 0.001f)
                Walk(currentSpeed);
            else if (y < -0.001f)
                Walk(-currentSpeed);

            // A/D: Strafe（左右移动，沿 Right 方向）
            if (x > 0.001f)
                Strafe(currentSpeed);
            else if (x < -0.001f)
                Strafe(-currentSpeed);
        },
        TriggerBehavior::WhileHeld);

    // Look: 鼠标旋转相机（仅在 OrbitCamera 按住时生效）
    m_callbackIds[1] = m_inputSystem->BindCallback(
        ActionId_Look,
        [this](const InputActionState &state) {
            if (!m_viewportHovered)
                return;

            // 只有 OrbitCamera 按住时，Look 才触发旋转
            bool canRotate = m_inputSystem->IsActionHeld(ActionId_OrbitCamera);
            if (!canRotate)
                return;

            float x, y;
            state.GetAxis2D(x, y);

            // 龙书风格：每个像素对应 0.25 度
            float dx = XMConvertToRadians(0.25f * x);
            float dy = XMConvertToRadians(0.25f * y);

            Pitch(dy);
            RotateY(dx);
        },
        TriggerBehavior::Axis2D);

    // OrbitCamera: 右键拖拽，标记启用轨道旋转（Look 回调中判断）
    m_callbackIds[2] = m_inputSystem->BindCallback(
        ActionId_OrbitCamera,
        [this](const InputActionState & /*state*/) {
            // OrbitCamera 的 Held 状态由 Look 回调读取
            // 不需要额外操作
        },
        TriggerBehavior::WhileHeld);

    // Zoom: 鼠标滚轮调整移动速度
    m_callbackIds[3] = m_inputSystem->BindCallback(
        ActionId_Zoom,
        [this](const InputActionState &state) {
            if (!m_viewportHovered)
                return;

            float x, y;
            state.GetAxis2D(x, y);
            if (y != 0.0f) {
                m_moveSpeed *= (1.0f + y * 0.1f);
                m_moveSpeed = std::clamp(m_moveSpeed, 1.0f, 5000.0f);
            }
        },
        TriggerBehavior::Axis2D);

    // Pan: 左键拖拽平移相机（仅在 Cursor+View 模式下生效）
    // 注意：Pan 绑定为数字键（Mouse_Left），位移数据从 Look 轴读取
    m_callbackIds[4] = m_inputSystem->BindCallback(
        ActionId_Pan,
        [this](const InputActionState & /*state*/) {
            if (!m_viewportHovered)
                return;

            // 仅 Cursor+View 模式下 Pan 生效
            if (m_getCurrentTool && m_getCurrentTool() != ViewportTool::Cursor)
                return;
            if (m_getCursorMode && m_getCursorMode() != CursorMode::View)
                return;

            // 从 Look 动作读取鼠标位移（Pan 本身是数字键，无轴数据）
            FVector2D lookInput = m_inputSystem->GetActionAxis2D(ActionId_Look);

            // 应用灵敏度缩放（原始鼠标 delta 过大，需要大幅衰减）
            float panSpeed = m_panSpeed * m_context->MainTimer->GetDeltaTime() * 0.1f;
            Strafe(-lookInput.X * panSpeed);          // 水平：左右 → X 轴
            Walk(lookInput.Y * panSpeed);              // 垂直：上下 → Z 轴（左手坐标系，保持 XZ 水平面）
        },
        TriggerBehavior::WhileHeld);

    // Select: 左键点击选中实体（仅在 Cursor+Select 模式下生效）
    m_callbackIds[5] = m_inputSystem->BindCallback(
        ActionId_Select,
        [this](const InputActionState &state) {
            if (!m_viewportHovered)
                return;

            // 仅 Cursor+Select 模式下 Select 生效
            if (m_getCurrentTool && m_getCurrentTool() != ViewportTool::Cursor)
                return;
            if (m_getCursorMode && m_getCursorMode() != CursorMode::Select)
                return;

            // TODO: 射线检测选中实体（当前射线检测系统在场景迁移时被注释，待恢复）
            // 点击时触发：存储拾取请求 → Worker 线程精确检测 → 设置选中实体
            (void)state;
        },
        TriggerBehavior::OnPressed);

    // FocusSelection: F 键聚焦到选中实体
    m_callbackIds[6] = m_inputSystem->BindCallback(
        ActionId_FocusSelection,
        [this](const InputActionState &state) {
            if (!m_viewportHovered)
                return;

            // 通过 m_context->GetActiveEntity() 或 Editor 持有的选中实体来聚焦
            // 当前由 Editor 的 ImmediateCallback 处理，此处保留回调占位
            (void)state;
        },
        TriggerBehavior::OnPressed);

    // MoveUp: Space 垂直上升
    m_callbackIds[7] = m_inputSystem->BindCallback(
        ActionId_MoveUp,
        [this](const InputActionState &) {
            if (!m_viewportHovered)
                return;

            float deltaTime = m_context->MainTimer->GetDeltaTime();
            XMVECTOR pos = XMLoadFloat3(&m_context->CameraMgr->GetMainCamera().Position);
            pos += XMVectorSet(0.0f, m_verticalSpeed * deltaTime, 0.0f, 0.0f);
            XMStoreFloat3(&m_context->CameraMgr->GetMainCamera().Position, pos);
        },
        TriggerBehavior::WhileHeld);

    // MoveDown: LeftCtrl 垂直下降
    m_callbackIds[7] = m_inputSystem->BindCallback(
        ActionId_MoveDown,
        [this](const InputActionState &) {
            if (!m_viewportHovered)
                return;

            float deltaTime = m_context->MainTimer->GetDeltaTime();
            XMVECTOR pos = XMLoadFloat3(&m_context->CameraMgr->GetMainCamera().Position);
            pos -= XMVectorSet(0.0f, m_verticalSpeed * deltaTime, 0.0f, 0.0f);
            XMStoreFloat3(&m_context->CameraMgr->GetMainCamera().Position, pos);
        },
        TriggerBehavior::WhileHeld);
}

void EditorCameraSystem::UnregisterInputCallbacks() {
    if (!m_inputSystem)
        return;

    for (auto id : m_callbackIds) {
        if (id != 0)
            m_inputSystem->UnbindCallback(id);
    }
    std::memset(m_callbackIds, 0, sizeof(m_callbackIds));
}

// ========================================================================
// PassConstants 更新（每帧由 Immediate 回调调用）
// ========================================================================

void EditorCameraSystem::UpdatePassConstants() {
    if (!m_context || !m_context->CameraMgr || !m_context->FrameResourceManager)
        return;

    // 更新相机 AspectRatio（视口尺寸变化时）
    if (m_viewportWidth > 0 && m_viewportHeight > 0) {
        auto &camera = m_context->CameraMgr->GetMainCamera();
        camera.AspectRatio = static_cast<float>(m_viewportWidth) / static_cast<float>(m_viewportHeight);
    }

    m_context->CameraMgr->UpdateMainCamera();

    const auto &camera = m_context->CameraMgr->GetMainCamera();
    auto &passConstants = m_context->FrameResourceManager->GetPassConstants();

    XMStoreFloat4x4(&passConstants.View, camera.ViewMatrix);
    XMStoreFloat4x4(&passConstants.Proj, camera.ProjMatrix);
    XMStoreFloat4x4(&passConstants.ViewProj, camera.ViewProjMatrix);
    XMStoreFloat4x4(&passConstants.InvView, camera.InverseView);
    XMStoreFloat4x4(&passConstants.InvProj, camera.InverseProj);
    XMStoreFloat4x4(&passConstants.InvViewProj, camera.InverseViewProj);
    XMStoreFloat4x4(&passConstants.PrevViewProj, camera.PrevViewProjMatrix);

    passConstants.CameraPos = camera.Position;
    passConstants.TotalTime = static_cast<float>(m_context->MainTimer->GetGameTime());
    passConstants.DeltaTime = m_context->MainTimer->GetDeltaTime();
    passConstants.NearPlane = camera.NearPlane;
    passConstants.FarPlane = camera.FarPlane;
    passConstants.AspectRatio = camera.AspectRatio;
    passConstants.FrameCount = m_context->FrameDriver->GetFrameStats().frameNumber;
}

// ========================================================================
// 聚焦到选中实体
// ========================================================================

void EditorCameraSystem::FocusOnEntity(ECS::Entity entity, float defaultDistance) {
    if (!m_context || !m_context->CameraMgr || !m_editorSceneMgr)
        return;

    auto &camera = m_context->CameraMgr->GetMainCamera();
    auto handle = static_cast<uint64_t>(entity);

    auto *tc = m_editorSceneMgr->GetComponent<ECS::TransformComponent>(handle);
    if (!tc)
        return;

    XMVECTOR targetPos = XMLoadFloat3(&tc->position);
    XMVECTOR forward = XMLoadFloat3(&camera.Forward);

    XMVECTOR newPos = targetPos - forward * defaultDistance;
    XMStoreFloat3(&camera.Position, newPos);

    m_context->CameraMgr->UpdateMainCamera();
}

// ========================================================================
// 龙书风格相机操作
// ========================================================================

void EditorCameraSystem::Pitch(float angle) {
    auto &camera = m_context->CameraMgr->GetMainCamera();

    XMMATRIX R = XMMatrixRotationAxis(XMLoadFloat3(&camera.Right), angle);

    XMStoreFloat3(&camera.Up, XMVector3TransformNormal(XMLoadFloat3(&camera.Up), R));
    XMStoreFloat3(&camera.Forward, XMVector3TransformNormal(XMLoadFloat3(&camera.Forward), R));
}

void EditorCameraSystem::RotateY(float angle) {
    auto &camera = m_context->CameraMgr->GetMainCamera();

    XMMATRIX R = XMMatrixRotationY(angle);

    XMStoreFloat3(&camera.Right, XMVector3TransformNormal(XMLoadFloat3(&camera.Right), R));
    XMStoreFloat3(&camera.Up, XMVector3TransformNormal(XMLoadFloat3(&camera.Up), R));
    XMStoreFloat3(&camera.Forward, XMVector3TransformNormal(XMLoadFloat3(&camera.Forward), R));
}

void EditorCameraSystem::Strafe(float d) {
    auto &camera = m_context->CameraMgr->GetMainCamera();

    XMVECTOR s = XMVectorReplicate(d);
    XMVECTOR r = XMLoadFloat3(&camera.Right);
    XMVECTOR p = XMLoadFloat3(&camera.Position);
    XMStoreFloat3(&camera.Position, XMVectorMultiplyAdd(s, r, p));
}

void EditorCameraSystem::Walk(float d) {
    auto &camera = m_context->CameraMgr->GetMainCamera();

    XMVECTOR s = XMVectorReplicate(d);
    XMVECTOR l = XMLoadFloat3(&camera.Forward);
    XMVECTOR p = XMLoadFloat3(&camera.Position);
    XMStoreFloat3(&camera.Position, XMVectorMultiplyAdd(s, l, p));
}