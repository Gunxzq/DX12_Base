#include "EditorViewportInput.h"
#include "EditorViewportInputActions.h"
#include "ECS/Core/Components/Transform.h"
#include "Platform/Input/InputManager.h"
#include "Platform/Input/InputSystem.h"
#include "Renderer/Scene/CameraManager.h"
#include "Scene/SceneManager.h"
#include "ThirdParty/imgui/imgui.h"

using namespace DX12Engine;
using namespace DX12Engine::Input;
using namespace DirectX;

// ========================================================================
// 构造/析构
// ========================================================================

EditorViewportInput::EditorViewportInput() = default;
EditorViewportInput::~EditorViewportInput() { Shutdown(); }

// ========================================================================
// 初始化
// ========================================================================

void EditorViewportInput::Initialize(Boot::GameContext *context) {
    if (m_initialized || !context)
        return;

    m_context = context;
    m_context->Logging->Info("[EditorViewportInput] Initializing...");

    m_initialized = true;
    m_context->Logging->Info("[EditorViewportInput] Initialized");
}

void EditorViewportInput::Shutdown() {
    if (!m_initialized)
        return;
    m_initialized = false;
}

// ========================================================================
// 每帧更新
// ========================================================================

void EditorViewportInput::Update(float deltaTime) {
    if (!m_initialized || !m_context)
        return;

    HandleCameraInput(deltaTime);
}

void EditorViewportInput::SetViewportSize(uint32_t width, uint32_t height) {
    m_viewportWidth = width;
    m_viewportHeight = height;
}

// ========================================================================
// 相机控制（复制自 GameInputHandler 的龙书风格第一人称相机）
// ========================================================================

void EditorViewportInput::HandleCameraInput(float deltaTime) {
    // 视口未悬停（用户在 UI 面板上操作），跳过所有相机控制
    if (!m_viewportHovered)
        return;

    auto *inputSys = m_context->InputMgr ? m_context->InputMgr->GetInputSystem() : nullptr;
    if (!inputSys) {
        static bool once = true;
        if (once) {
            m_context->Logging->Warn("[EditorViewportInput] No InputSystem available");
            once = false;
        }
        return;
    }

    if (!m_context->CameraMgr) {
        static bool once = true;
        if (once) {
            m_context->Logging->Warn("[EditorViewportInput] No CameraMgr");
            once = false;
        }
        return;
    }

    auto &camera = m_context->CameraMgr->GetMainCamera();

    // =========================================================================
    // 相机旋转（鼠标右键拖拽） - 龙书风格：Pitch + RotateY
    // =========================================================================
    bool canRotate = inputSys->IsActionHeld(ActionId_OrbitCamera);
    if (canRotate) {
        FVector2D lookInput = inputSys->GetActionAxis2D(ActionId_Look);

        // 龙书风格：每个像素对应 0.25 度
        float dx = XMConvertToRadians(0.25f * lookInput.X);
        float dy = XMConvertToRadians(0.25f * lookInput.Y);

        Pitch(dy);
        RotateY(dx);
    }

    // =========================================================================
    // 相机移动 (WASD) - 龙书风格：Walk + Strafe
    // =========================================================================
    FVector2D moveInput = inputSys->GetActionAxis2D(ActionId_Move);
    // 鼠标滚轮调整移动速度
    FVector2D zoomInput = inputSys->GetActionAxis2D(ActionId_Zoom);
    if (zoomInput.Y != 0.0f) {
        m_moveSpeed *= (1.0f + zoomInput.Y * 0.1f);
        m_moveSpeed = std::clamp(m_moveSpeed, 1.0f, 5000.0f);
    }

    float currentSpeed = m_moveSpeed * deltaTime;

    // W/S: Walk（前后移动，沿 Forward 方向）
    if (moveInput.Y > 0.001f)
        Walk(currentSpeed);
    else if (moveInput.Y < -0.001f)
        Walk(-currentSpeed);

    // A/D: Strafe（左右移动，沿 Right 方向）
    if (moveInput.X > 0.001f)
        Strafe(currentSpeed);
    else if (moveInput.X < -0.001f)
        Strafe(-currentSpeed);

    // =========================================================================
    // 垂直升降 (Space / LeftCtrl)
    // =========================================================================
    bool moveUp = inputSys->IsActionHeld(ActionId_MoveUp);
    bool moveDown = inputSys->IsActionHeld(ActionId_MoveDown);

    if (moveUp || moveDown) {
        float verticalMove = 0.0f;
        if (moveUp)
            verticalMove += 1.0f;
        if (moveDown)
            verticalMove -= 1.0f;

        XMVECTOR pos = XMLoadFloat3(&camera.Position);
        pos += XMVectorSet(0.0f, verticalMove * m_verticalSpeed * deltaTime, 0.0f, 0.0f);
        XMStoreFloat3(&camera.Position, pos);
    }

    // =========================================================================
    // 更新相机 AspectRatio（视口尺寸变化时）
    // =========================================================================
    if (m_viewportWidth > 0 && m_viewportHeight > 0) {
        camera.AspectRatio = static_cast<float>(m_viewportWidth) / static_cast<float>(m_viewportHeight);
    }

    m_context->CameraMgr->UpdateMainCamera();
}

// ========================================================================
// 聚焦到选中实体（保持当前视角方向，仅调整位置）
// ========================================================================

void EditorViewportInput::FocusOnEntity(ECS::Entity entity, float defaultDistance) {
    if (!m_context || !m_context->CameraMgr || !m_editorSceneMgr)
        return;

    auto &camera = m_context->CameraMgr->GetMainCamera();
    auto handle = static_cast<uint64_t>(entity);

    // 获取实体位置
    auto *tc = m_editorSceneMgr->GetComponent<ECS::TransformComponent>(handle);
    if (!tc)
        return;

    XMVECTOR targetPos = XMLoadFloat3(&tc->position);
    XMVECTOR forward = XMLoadFloat3(&camera.Forward);

    // 保持当前 Forward/Right/Up 不变，只调整 Position
    // 新位置 = 目标位置 - Forward * 距离
    XMVECTOR newPos = targetPos - forward * defaultDistance;
    XMStoreFloat3(&camera.Position, newPos);

    m_context->CameraMgr->UpdateMainCamera();
}

// ========================================================================
// 龙书风格相机操作（复制自 GameInputHandler）
// ========================================================================

void EditorViewportInput::Pitch(float angle) {
    auto &camera = m_context->CameraMgr->GetMainCamera();

    // 绕 Right 轴旋转 Up 和 Forward
    XMMATRIX R = XMMatrixRotationAxis(XMLoadFloat3(&camera.Right), angle);

    XMStoreFloat3(&camera.Up, XMVector3TransformNormal(XMLoadFloat3(&camera.Up), R));
    XMStoreFloat3(&camera.Forward, XMVector3TransformNormal(XMLoadFloat3(&camera.Forward), R));
}

void EditorViewportInput::RotateY(float angle) {
    auto &camera = m_context->CameraMgr->GetMainCamera();

    // 绕世界 Y 轴旋转所有基向量
    XMMATRIX R = XMMatrixRotationY(angle);

    XMStoreFloat3(&camera.Right, XMVector3TransformNormal(XMLoadFloat3(&camera.Right), R));
    XMStoreFloat3(&camera.Up, XMVector3TransformNormal(XMLoadFloat3(&camera.Up), R));
    XMStoreFloat3(&camera.Forward, XMVector3TransformNormal(XMLoadFloat3(&camera.Forward), R));
}

void EditorViewportInput::Strafe(float d) {
    auto &camera = m_context->CameraMgr->GetMainCamera();

    // position += d * right
    XMVECTOR s = XMVectorReplicate(d);
    XMVECTOR r = XMLoadFloat3(&camera.Right);
    XMVECTOR p = XMLoadFloat3(&camera.Position);
    XMStoreFloat3(&camera.Position, XMVectorMultiplyAdd(s, r, p));
}

void EditorViewportInput::Walk(float d) {
    auto &camera = m_context->CameraMgr->GetMainCamera();

    // position += d * forward
    XMVECTOR s = XMVectorReplicate(d);
    XMVECTOR l = XMLoadFloat3(&camera.Forward);
    XMVECTOR p = XMLoadFloat3(&camera.Position);
    XMStoreFloat3(&camera.Position, XMVectorMultiplyAdd(s, l, p));
}