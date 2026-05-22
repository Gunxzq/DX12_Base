#include "GameInputHandler.h"
#include "../Input/GameInputActions.h"
#include "Boot/GameContext.h"
#include "Common/ThrowHelper.h"
#include "Framework/SystemRegistry.h"
#include "Math/MathTypes.h"
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
using namespace DX12Engine;

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
    mainCamera.Position = DirectX::XMFLOAT3(0.0f, 2.0f, -10.0f);
    mainCamera.Rotation = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);

    m_context->Logging->Info("[Input] Camera Reset!");
}

void GameInputHandler::HandleCursorCapture() {
    if (!m_context || !m_context->InputSys || !m_context->Window)
        return;

    auto &inputSys = *m_context->InputSys;
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
    if (!m_context || !m_context->InputSys || !m_context->CameraMgr || !m_context->Window) {
        return;
    }

    auto &inputSys = *m_context->InputSys;
    auto &cameraMgr = *m_context->CameraMgr;
    auto &mainCamera = cameraMgr.GetMainCamera();
    auto &window = *m_context->Window;

    // 跳过第一帧，防止捕获瞬间的视角跳变
    if (m_skipLookInputThisFrame) {
        m_skipLookInputThisFrame = false;
        return;
    }

    // =========================================================================
    // 调试：重置相机 (按 R 键)
    // =========================================================================
    if (inputSys.IsActionPressed(ActionId_ResetCamera)) {
        ResetCamera();
    }

    // =========================================================================
    // 相机旋转 (Look) - 仅当鼠标被捕获时生效
    // =========================================================================
    if (window.IsCursorCaptured()) {
        FVector2D lookInput = inputSys.GetActionAxis2D(ActionId_Look);
        const float mouseSensitivity = 0.002f;

        mainCamera.Rotation.y += lookInput.X * mouseSensitivity; // Yaw
        mainCamera.Rotation.x += lookInput.Y * mouseSensitivity; // Pitch

        // 限制俯仰角，防止万向节死锁
        const float maxPitch = XM_PI / 2.0f - 0.01f;
        mainCamera.Rotation.x = std::clamp(mainCamera.Rotation.x, -maxPitch, maxPitch);

        // 归一化 Yaw 到 [0, 2PI)
        if (mainCamera.Rotation.y > XM_2PI)
            mainCamera.Rotation.y -= XM_2PI;
        if (mainCamera.Rotation.y < 0.0f)
            mainCamera.Rotation.y += XM_2PI;
    }

    // =========================================================================
    // 相机移动 (Move) - WASD
    // =========================================================================
    FVector2D moveInput = inputSys.GetActionAxis2D(ActionId_Move);
    bool isSprinting = inputSys.IsActionHeld(ActionId_Sprint);

    float currentSpeed = 5.0f * (isSprinting ? 2.0f : 1.0f);

    if (std::abs(moveInput.X) > 0.001f || std::abs(moveInput.Y) > 0.001f) {
        float yaw = mainCamera.Rotation.y;

        // 计算水平方向的前向和右向向量
        XMFLOAT3 forwardDir;
        forwardDir.x = sin(yaw);
        forwardDir.y = 0.0f;
        forwardDir.z = cos(yaw);

        XMFLOAT3 rightDir;
        rightDir.x = cos(yaw);
        rightDir.y = 0.0f;
        rightDir.z = -sin(yaw);

        XMVECTOR pos = XMLoadFloat3(&mainCamera.Position);
        XMVECTOR fwd = XMLoadFloat3(&forwardDir);
        XMVECTOR rgt = XMLoadFloat3(&rightDir);

        pos += fwd * (moveInput.Y * currentSpeed * deltaTime);
        pos += rgt * (moveInput.X * currentSpeed * deltaTime);

        XMStoreFloat3(&mainCamera.Position, pos);
    }
}
