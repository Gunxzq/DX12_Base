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
    mainCamera.Position = DirectX::XMFLOAT3(0.0f, 5.0f, -15.0f);

    // 重置旋转角度
    mainCamera.Rotation = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);

    // 重置相机参数到默认值
    m_moveSpeed = 8.0f;
    m_verticalSpeed = 6.0f;

    m_context->Logging->Info("[Input] Camera Reset! (Free camera)");
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
    // 相机旋转 (鼠标) - 仅当鼠标被捕获时生效
    // =========================================================================
    if (window.IsCursorCaptured()) {
        FVector2D lookInput = inputSys.GetActionAxis2D(ActionId_Look);

        // Yaw (左右旋转)
        mainCamera.Rotation.y += lookInput.X * m_mouseSensitivity;
        // Pitch (上下旋转)
        mainCamera.Rotation.x += lookInput.Y * m_mouseSensitivity;

        // 限制俯仰角，防止万向节死锁
        const float maxPitch = XM_PIDIV2 - 0.01f;
        mainCamera.Rotation.x = std::clamp(mainCamera.Rotation.x, -maxPitch, maxPitch);

        // 归一化 Yaw 到 [0, 2PI)
        if (mainCamera.Rotation.y > XM_2PI)
            mainCamera.Rotation.y -= XM_2PI;
        if (mainCamera.Rotation.y < 0.0f)
            mainCamera.Rotation.y += XM_2PI;
    }

    // =========================================================================
    // 相机移动 (WASD + Q/E)
    // =========================================================================

    // 获取移动输入
    FVector2D moveInput = inputSys.GetActionAxis2D(ActionId_Move);
    bool isSprinting = inputSys.IsActionHeld(ActionId_Sprint);

    // 计算当前移动速度
    float currentSpeed = m_moveSpeed * (isSprinting ? m_sprintMultiplier : 1.0f);

    // 只有有输入时才计算移动
    if (std::abs(moveInput.X) > 0.001f || std::abs(moveInput.Y) > 0.001f) {
        // 直接使用 CameraManager 每帧计算好的前向和上向量
        XMVECTOR fwdVec = XMLoadFloat3(&mainCamera.Forward);
        XMVECTOR upVec = XMLoadFloat3(&mainCamera.Up);

        // 右向 = 前向 x 上向（叉积，左手坐标系中为左手法则）
        XMVECTOR rgtVec = XMVector3Cross(upVec, fwdVec);
        rgtVec = XMVector3Normalize(rgtVec);

        // 移除垂直分量用于水平移动计算
        XMVECTOR horizontalFwd = XMVectorSet(XMVectorGetX(fwdVec), 0.0f, XMVectorGetZ(fwdVec), 0.0f);
        horizontalFwd = XMVector3Normalize(horizontalFwd);

        XMVECTOR pos = XMLoadFloat3(&mainCamera.Position);

        // 前后移动 (Y 轴输入)
        if (std::abs(moveInput.Y) > 0.001f) {
            pos += horizontalFwd * (moveInput.Y * currentSpeed * deltaTime);
        }

        // 左右移动 (X 轴输入)
        if (std::abs(moveInput.X) > 0.001f) {
            pos += rgtVec * (moveInput.X * currentSpeed * deltaTime);
        }

        XMStoreFloat3(&mainCamera.Position, pos);
    }

    // =========================================================================
    // 垂直升降 (Q 键上升，E 键下降)
    // =========================================================================
    // 注意：需要先在 JSON 配置中为 Q/E 添加动作绑定
    // 暂时使用备用方案：LeftControl 下降，Space 上升（如果没有独立 Q/E）
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