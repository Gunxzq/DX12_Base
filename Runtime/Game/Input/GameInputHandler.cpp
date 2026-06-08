#include "GameInputHandler.h"
#include "../Input/GameInputActions.h"
#include "Boot/GameContext.h"
#include "Common/ThrowHelper.h"
#include "Framework/SystemRegistry.h"
#include "Math/MathTypes.h"
#include "Platform/Input/InputManager.h"
#include "Platform/Input/InputSystem.h"
#include "Platform/Windows/Window.h"
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
}

void GameInputHandler::RegisterInputSystem() {
    if (!m_context)
        return;

    // ========================================================================
    // CameraControlSystem - 每帧处理相机输入
    // ========================================================================
    SystemRegistry::Register({.name = "CameraControlSystem",
                              .func =
                                  [this](Registry &registry, const MessageContext &ctx) {
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

void GameInputHandler::ResetCamera() {
    if (!m_context || !m_context->CameraMgr)
        return;

    auto &mainCamera = m_context->CameraMgr->GetMainCamera();

    // 重置位置
    mainCamera.Position = DirectX::XMFLOAT3(0.0f, 2.0f, -15.0f);

    // 重置基向量（龙书默认初始值）
    mainCamera.Right   = DirectX::XMFLOAT3(1.0f, 0.0f, 0.0f);
    mainCamera.Up      = DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f);
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
    // 相机旋转 (鼠标) - 龙书风格：Pitch + RotateY，仅当鼠标被捕获时生效
    // =========================================================================
    if (window.IsCursorCaptured()) {
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