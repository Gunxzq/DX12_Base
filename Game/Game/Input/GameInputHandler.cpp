#include "GameInputHandler.h"
#include "../Input/GameInputActions.h"
#include "Boot/GameContext.h"
#include "Common/ThrowHelper.h"
#include "Framework/SystemRegistry.h"
#include "Math/MathTypes.h"
#include "Platform/Input/InputContextStack.h"
#include "Platform/Input/InputManager.h"
#include "Platform/Input/InputSystem.h"
#include "Platform/Windows/Window.h"
#include "Renderer/Core/CullingSystem.h"
#include "Renderer/Core/VisibleRaycaster.h"
#include "Renderer/Scene/CameraManager.h"
#include "Scheduler/FrameDriver.h"
#include <DirectXMath.h>

using namespace DirectX;
using namespace Microsoft::WRL;
using namespace DX12Engine::Boot;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Scheduler;
using namespace DX12Engine::Input;
using namespace DX12Engine;
using DX12Engine::Input::InputSystem;

GameInputHandler::GameInputHandler() = default;
GameInputHandler::~GameInputHandler() = default;

void GameInputHandler::Initialize(GameContext *context) {
    m_context = context;

    // 注册输入处理系统
    RegisterInputSystem();

    // 注册拾取系统（EarlyUpdate）
    RegisterPickingSystems();
}

void GameInputHandler::RegisterInputSystem() {
    if (!m_context)
        return;

    // ========================================================================
    // CameraControlSystem - 每帧处理相机输入
    // ========================================================================
    SystemRegistry::Register({.name = "CameraControlSystem",
                              .func =
                                  [this](const MessageContext &ctx) {
                                      float deltaTime = m_context->MainTimer->GetDeltaTime();
                                      if (deltaTime <= 0.0f)
                                          return;

                                      HandleCursorCapture();
                                      HandleCameraInput(deltaTime);
                                  },
                              .phase = TaskPhase::EarlyUpdate, // 在渲染前更新相机位置
                              .threadType = ThreadType::Main,
                              .priority = TaskPriority::High,
                              .alwaysRun = true});
}

void GameInputHandler::RegisterPickingSystems() {
    if (!m_context || !m_visibleRaycaster)
        return;

    // ========================================================================
    // PickingProcessSystem — EarlyUpdate 阶段：输入检测 + 射线投射
    //
    // ⚠️ 不触碰 ECS 组件（读/写都不行，必须在 FrameSync 进行）
    //
    // 职责（游戏层）：
    //   1. 输入上下文 / 光标捕获检查
    //   2. Pressed: VisibleRaycaster 射线检测 → 存意图 DragIntent::Start
    //   3. Held:    纯数学射线-平面求交 → 存意图 DragIntent::Update  + 计算好的新位置
    //   4. Released: 存意图 DragIntent::End
    //   5. 将 raycastResult 存入 GameContext 供 DebugUI 消费
    // ========================================================================
    SystemRegistry::Register(
        {.name = "PickingProcessSystem",
         .func =
             [this](const MessageContext &ctx) {
                 if (!m_context || !m_visibleRaycaster || !m_context->Window)
                     return;

                 // -- 输入上下文检查 --
                 if (m_context->InputMgr) {
                     std::string topCtx = m_context->InputMgr->GetTopContext();
                     if (topCtx != "Gameplay")
                         return;
                 }

                 auto *inputSys = m_context->InputMgr ? m_context->InputMgr->GetInputSystem() : nullptr;
                 if (!inputSys)
                     return;

                 // -- 光标捕获时不拾取（自由视角模式，鼠标用于旋转）--
                 if (m_context->Window->IsCursorCaptured())
                     return;

                 // -- 获取屏幕坐标 --
                 auto &window = *m_context->Window;
                 float mouseX = static_cast<float>(window.GetMouseX());
                 float mouseY = static_cast<float>(window.GetMouseY());
                 uint32_t w = window.GetWidth();
                 uint32_t h = window.GetHeight();

                 // -- 更新相机数据 → 生成射线 --
                 m_visibleRaycaster->UpdateCameraData(m_context->predictedCameraData);
                 FRay ray = m_visibleRaycaster->ScreenToRay(mouseX, mouseY, w, h);

                 // ================================================================
                 // 状态 1: 按下 —— 射线检测 → 意图 Start
                 // ================================================================
                 if (inputSys->IsActionPressed(ActionId_Pick)) {
                     m_context->raycastResult = m_visibleRaycaster->RaycastAll(ray);
                     if (m_context->raycastResult.HasAny()) {
                         const auto &closest = m_context->raycastResult.GetClosest();
                         m_pendingHitEntity = closest.entity;
                         m_pendingHitPoint = closest.hitPoint;
                         m_pendingDragIntent = DragIntent::Start;
                     }
                 }

                 // ================================================================
                 // 状态 2: 按住 —— 纯数学射线-平面求交 → 意图 Update
                 // (不读 ECS，偏移在 FrameSync 的 ApplyDragToECS 中添加)
                 // ================================================================
                 if (inputSys->IsActionHeld(ActionId_Pick) && m_isDragging) {
                     const auto &cam = m_context->CameraMgr->GetMainCamera();

                     XMVECTOR fwd = XMLoadFloat3(&cam.Forward);
                     XMVECTOR cp = XMLoadFloat3(&cam.Position);
                     XMVECTOR planePoint = XMVectorAdd(cp, XMVectorScale(fwd, m_dragDepth));

                     XMVECTOR ro = XMVectorSet(ray.Origin.X, ray.Origin.Y, ray.Origin.Z, 0.0f);
                     XMVECTOR rd = XMVectorSet(ray.Direction.X, ray.Direction.Y, ray.Direction.Z, 0.0f);

                     float denom = XMVectorGetX(XMVector3Dot(rd, fwd));
                     if (std::abs(denom) >= 1e-6f) {
                         float t = XMVectorGetX(XMVector3Dot(XMVectorSubtract(planePoint, ro), fwd)) / denom;
                         if (t >= 0.0f) {
                             XMVECTOR hitPt = XMVectorAdd(ro, XMVectorScale(rd, t));
                             XMStoreFloat3(&m_pendingDragPosition, hitPt);
                             m_pendingDragIntent = DragIntent::Update;
                         }
                     }
                 }

                 // ================================================================
                 // 状态 3: 释放 —— 意图 End
                 // ================================================================
                 if (inputSys->IsActionReleased(ActionId_Pick) && m_isDragging) {
                     m_pendingDragIntent = DragIntent::End;
                 }
             },
         .phase = TaskPhase::EarlyUpdate,
         .threadType = ThreadType::Main,
         .priority = TaskPriority::Normal,
         .alwaysRun = true});
}

// ============================================================================
// 拖拽实现（从 FrameSync 回调调用，ECS 访问安全）
// 主线程调用
// ============================================================================

void GameInputHandler::ApplyDragToECS(Registry &registry) {
    switch (m_pendingDragIntent) {
    // ── Start: 读 TransformComponent → 计算偏移和深度 ──
    case DragIntent::Start: {
        auto *transform = registry.TryGetComponent<TransformComponent>(m_pendingHitEntity);
        if (!transform)
            break;

        m_dragEntity = m_pendingHitEntity;
        m_isDragging = true;

        // 记录偏移
        m_dragOffset =
            DirectX::XMFLOAT3(transform->position.x - m_pendingHitPoint.x, transform->position.y - m_pendingHitPoint.y,
                              transform->position.z - m_pendingHitPoint.z);

        // 计算拾取深度
        const auto &cam = m_context->CameraMgr->GetMainCamera();
        XMVECTOR cp = XMLoadFloat3(&cam.Position);
        XMVECTOR fwd = XMLoadFloat3(&cam.Forward);
        XMVECTOR hp = XMLoadFloat3(&m_pendingHitPoint);
        XMVECTOR toHit = XMVectorSubtract(hp, cp);
        m_dragDepth = XMVectorGetX(XMVector3Dot(toHit, fwd));
        break;
    }

    // ── Update: 写 TransformComponent::position ──
    case DragIntent::Update: {
        auto *transform = registry.TryGetComponent<TransformComponent>(m_dragEntity);
        if (!transform) {
            EndDragCleanup();
            break;
        }
        transform->position.x = m_pendingDragPosition.x + m_dragOffset.x;
        transform->position.y = m_pendingDragPosition.y + m_dragOffset.y;
        transform->position.z = m_pendingDragPosition.z + m_dragOffset.z;
        break;
    }

    // ── End: 清理状态 ──
    case DragIntent::End:
        EndDragCleanup();
        break;

    case DragIntent::None:
        break;
    }

    m_pendingDragIntent = DragIntent::None;
}

void GameInputHandler::EndDragCleanup() {
    m_isDragging = false;
    m_dragEntity = INVALID_ENTITY;
    m_dragDepth = 0.0f;
    m_dragOffset = {0.0f, 0.0f, 0.0f};
}

void GameInputHandler::ResetCamera() {
    if (!m_context || !m_context->CameraMgr)
        return;

    auto &mainCamera = m_context->CameraMgr->GetMainCamera();

    mainCamera.Position = DirectX::XMFLOAT3(4.0f, 34.0f, -6.0f);

    // 重置基向量（龙书默认初始值）
    mainCamera.Right = DirectX::XMFLOAT3(1.0f, 0.0f, 0.0f);
    mainCamera.Up = DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f);
    mainCamera.Forward = DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f);

    // 重置旋转角度（保留兼容）
    mainCamera.Rotation = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);

    // 重置相机参数到默认值
    m_moveSpeed = 10.0f;
    m_verticalSpeed = 6.0f;

    m_context->Logging->Info("[Input] Camera Reset! (First-person style camera)");
}

void GameInputHandler::HandleCursorCapture() {
    if (!m_context || !m_context->InputMgr || !m_context->Window)
        return;

    auto &inputSys = *m_context->InputMgr->GetInputSystem();
    auto &window = *m_context->Window;

    bool wasCaptured = window.IsCursorCaptured();

    // 按 Escape 切换捕获状态
    if (inputSys.IsActionPressed(ActionId_Pause)) {
        bool newCaptureState = !wasCaptured;
        window.SetCursorCapture(newCaptureState);

        if (newCaptureState) {
            m_skipLookInputThisFrame = true;
            m_context->Logging->Info("[Input] Cursor Captured. Skipping first frame look input.");
        }
    }
}

void GameInputHandler::HandleCameraInput(float deltaTime) {
    if (!m_context || !m_context->InputMgr || !m_context->CameraMgr || !m_context->Window) {
        return;
    }

    auto &inputSys = *m_context->InputMgr->GetInputSystem();
    auto &cameraMgr = *m_context->CameraMgr;
    auto &mainCamera = cameraMgr.GetMainCamera();
    auto &window = *m_context->Window;

    // 跳过第一帧，防止捕获瞬间的视角跳变
    if (m_skipLookInputThisFrame) {
        m_skipLookInputThisFrame = false;
        return;
    }

    // =========================================================================
    // 重置相机 (按 R 键)
    // =========================================================================
    if (inputSys.IsActionPressed(ActionId_ResetCamera)) {
        ResetCamera();
    }

    // =========================================================================
    // 相机旋转 (鼠标) - 龙书风格：Pitch + RotateY
    //   捕获模式：自由旋转
    //   非捕获模式：按住鼠标右键 (OrbitCamera) 拖拽旋转
    // =========================================================================
    bool canRotate = window.IsCursorCaptured() || inputSys.IsActionHeld(ActionId_OrbitCamera);
    if (canRotate) {
        FVector2D lookInput = inputSys.GetActionAxis2D(ActionId_Look);

        // 龙书风格：每个像素对应 0.25 度
        float dx = XMConvertToRadians(0.25f * lookInput.X);
        float dy = XMConvertToRadians(0.25f * lookInput.Y);

        Pitch(dy);
        RotateY(dx);
    }

    // =========================================================================
    // 相机移动 (WASD) - 龙书风格：Walk + Strafe
    // =========================================================================
    FVector2D moveInput = inputSys.GetActionAxis2D(ActionId_Move);
    bool isSprinting = inputSys.IsActionHeld(ActionId_Sprint);

    float currentSpeed = m_moveSpeed * (isSprinting ? m_sprintMultiplier : 1.0f);
    float d = currentSpeed * deltaTime;

    // W/S: Walk (前后移动，沿 Look 方向)
    if (moveInput.Y > 0.001f)
        Walk(d);
    else if (moveInput.Y < -0.001f)
        Walk(-d);

    // A/D: Strafe (左右移动，沿 Right 方向)
    if (moveInput.X > 0.001f)
        Strafe(d);
    else if (moveInput.X < -0.001f)
        Strafe(-d);

    // =========================================================================
    // 垂直升降 (Space/Crouch)
    // =========================================================================
    bool moveUp = inputSys.IsActionHeld(ActionId_Jump);     // Space 上升
    bool moveDown = inputSys.IsActionHeld(ActionId_Crouch); // Crouch 下降

    if (moveUp || moveDown) {
        float verticalMove = 0.0f;
        if (moveUp)
            verticalMove += 1.0f;
        if (moveDown)
            verticalMove -= 1.0f;

        XMVECTOR pos = XMLoadFloat3(&mainCamera.Position);
        pos += XMVectorSet(0.0f, verticalMove * m_verticalSpeed * deltaTime, 0.0f, 0.0f);
        XMStoreFloat3(&mainCamera.Position, pos);
    }
}

// =========================================================================
// 龙书风格相机操作
// =========================================================================

void GameInputHandler::Pitch(float angle) {
    auto &camera = m_context->CameraMgr->GetMainCamera();

    // 绕 Right 轴旋转 Up 和 Forward
    XMMATRIX R = XMMatrixRotationAxis(XMLoadFloat3(&camera.Right), angle);

    XMStoreFloat3(&camera.Up, XMVector3TransformNormal(XMLoadFloat3(&camera.Up), R));
    XMStoreFloat3(&camera.Forward, XMVector3TransformNormal(XMLoadFloat3(&camera.Forward), R));
}

void GameInputHandler::RotateY(float angle) {
    auto &camera = m_context->CameraMgr->GetMainCamera();

    // 绕世界 Y 轴旋转所有基向量
    XMMATRIX R = XMMatrixRotationY(angle);

    XMStoreFloat3(&camera.Right, XMVector3TransformNormal(XMLoadFloat3(&camera.Right), R));
    XMStoreFloat3(&camera.Up, XMVector3TransformNormal(XMLoadFloat3(&camera.Up), R));
    XMStoreFloat3(&camera.Forward, XMVector3TransformNormal(XMLoadFloat3(&camera.Forward), R));
}

void GameInputHandler::Strafe(float d) {
    auto &camera = m_context->CameraMgr->GetMainCamera();

    // position += d * right
    XMVECTOR s = XMVectorReplicate(d);
    XMVECTOR r = XMLoadFloat3(&camera.Right);
    XMVECTOR p = XMLoadFloat3(&camera.Position);
    XMStoreFloat3(&camera.Position, XMVectorMultiplyAdd(s, r, p));
}

void GameInputHandler::Walk(float d) {
    auto &camera = m_context->CameraMgr->GetMainCamera();

    // position += d * forward
    XMVECTOR s = XMVectorReplicate(d);
    XMVECTOR l = XMLoadFloat3(&camera.Forward);
    XMVECTOR p = XMLoadFloat3(&camera.Position);
    XMStoreFloat3(&camera.Position, XMVectorMultiplyAdd(s, l, p));
}