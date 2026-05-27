#include "Game.h"
#include "DebugUI/DebugUIManager.h"
#include "Event/MessageDispatcher.h"
#include "Framework/SystemRegistry.h"
#include "Platform/Input/InputSystem.h"
#include "Platform/Windows/Window.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/CameraManager.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Scheduler/FrameDriver.h"

using namespace DX12Engine;
using namespace DX12Engine::DebugUI;
using namespace DX12Engine::Scheduler;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Input;
using namespace DX12Engine::Math;
using namespace DX12Engine::ECS;

Game::Game(Boot::GameContext *context) : m_context(context) {}

Game::~Game() {
    if (m_isRunning || m_isInitialized) {
        Shutdown();
    }
}

bool Game::Initialize() {
    if (!m_context || !m_context->IsValid()) {
        return false;
    }

    m_context->Logging->Info("[Game] Initializing game...");

    // 1. 初始化 GPU 资源管理器
    Resource::GpuResourceManager::GetInstance().Initialize();

    // 2. 创建渲染器
    m_opaqueRenderer = std::make_unique<OpaqueRenderer>();
    m_opaqueRenderer->SetDeviceContext(m_context->DeviceContext);
    m_opaqueRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_opaqueRenderer->SetMaterialManager(m_context->MaterialMgr);
    m_opaqueRenderer->Initialize();

    m_context->CullingSystem = &m_cullingSystem;
    m_context->LODSystem = &m_lodSystem;
    m_context->RenderItemBuilder = &m_renderItemBuilder;

    // 设置帧驱动器引用
    m_context->RenderItemBuilder->SetFrameResourceManager(m_context->FrameResourceManager);
    m_context->RenderItemBuilder->SetMaterialManager(m_context->MaterialMgr);

    // 4. 初始化相机
    if (m_context->CameraMgr) {
        auto &mainCamera = m_context->CameraMgr->GetMainCamera();
        mainCamera.Position = DirectX::XMFLOAT3(0.0f, 0.0f, -10.0f);
        mainCamera.Rotation = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        m_context->CameraMgr->UpdateMainCamera();
    }

    // 配置 LODSystem
    m_lodSystem.SetLODConfig(LODConfig::GetDefault());
    m_lodSystem.SetCameraManager(m_context->CameraMgr);
    m_lodSystem.SetGeometryManager(m_context->GeometryResourceManager);

    // 配置 RenderItemBuilder
    m_renderItemBuilder.SetCameraManager(m_context->CameraMgr);

    // 5. 注册引擎级系统（窗口大小变化、全屏切换等）
    RegisterEngineSystems();

    // 6. 初始化游戏模块（它们会自己注册游戏逻辑系统）
    m_world.Initialize(m_context, m_opaqueRenderer.get());
    m_world.CreateTestCube();

    m_inputHandler.Initialize(m_context);

    // 7. 注册相机更新回调（Immediate 路径）
    if (m_context->FrameDriver) {
        m_context->FrameDriver->RegisterImmediateCallback(
            [this]() {
                m_context->CameraMgr->UpdateMainCamera();

                const auto &camera = m_context->CameraMgr->GetMainCamera();
                auto &passConstants = m_context->FrameResourceManager->GetPassConstants();

                // 存储矩阵
                XMStoreFloat4x4(&passConstants.View, camera.ViewMatrix);
                XMStoreFloat4x4(&passConstants.Proj, camera.ProjMatrix);
                XMStoreFloat4x4(&passConstants.ViewProj, camera.ViewProjMatrix);
                XMStoreFloat4x4(&passConstants.InvViewProj, camera.InverseViewProj);

                // 存储其他数据
                passConstants.CameraPos = camera.Position;
                passConstants.TotalTime = static_cast<float>(m_context->MainTimer->GetGameTime());
                passConstants.DeltaTime = m_context->MainTimer->GetDeltaTime();
                passConstants.NearPlane = camera.NearPlane;
                passConstants.FarPlane = camera.FarPlane;
                passConstants.AspectRatio = camera.AspectRatio;
                passConstants.FrameCount = m_context->FrameDriver->GetFrameStats().frameNumber;
                passConstants.AmbientLight = {0.5f, 0.5f, 0.5f, 0.8f}; // 提高环境光强度

                // 在相机更新回调中
                static float time = 0.0f;
                time += m_context->MainTimer->GetDeltaTime();
                float angle = time * 0.5f;
                float x = cosf(angle);
                float z = sinf(angle);

                LightConstants lightConstants = {};
                lightConstants.NumDirLights = 1;   // 本例不使用方向光
                lightConstants.NumPointLights = 3; // 使用3个点光源
                lightConstants.NumSpotLights = 0;  // 不使用聚光灯

                // ========================================================================
                // 点光源 0：暖色光源（红色/橙色），位于场景右前方
                // ========================================================================
                lightConstants.Lights[0].Strength = DirectX::XMFLOAT4(1.2f, 0.6f, 0.2f, 0.0f); // 橙红色光
                lightConstants.Lights[0].Direction = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f); // 点光源不使用方向
                lightConstants.Lights[0].Position = DirectX::XMFLOAT4(5.0f, 2.0f, 5.0f, 0.0f);
                lightConstants.Lights[0].FalloffStart = 1.0f; // 1米开始衰减
                lightConstants.Lights[0].FalloffEnd = 15.0f;  // 15米处完全衰减
                lightConstants.Lights[0].SpotPower = 0.0f;    // 点光源不使用聚光灯指数
                lightConstants.Lights[0].Range = 15.0f;
                lightConstants.Lights[0].CastShadow = 1;
                lightConstants.Lights[0].ShadowBias = 0.001f;
                lightConstants.Lights[0].ShadowMapIndex = 0;

                // ========================================================================
                // 点光源 1：冷色光源（蓝色/青色），位于场景左前方
                // ========================================================================
                lightConstants.Lights[1].Strength = DirectX::XMFLOAT4(0.2f, 0.5f, 1.2f, 0.0f); // 蓝色光
                lightConstants.Lights[1].Direction = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
                lightConstants.Lights[1].Position = DirectX::XMFLOAT4(-5.0f, 2.0f, 5.0f, 0.0f);
                lightConstants.Lights[1].FalloffStart = 1.0f;
                lightConstants.Lights[1].FalloffEnd = 15.0f;
                lightConstants.Lights[1].SpotPower = 0.0f;
                lightConstants.Lights[1].Range = 15.0f;
                lightConstants.Lights[1].CastShadow = 0; // 不投射阴影
                lightConstants.Lights[1].ShadowBias = 0.001f;
                lightConstants.Lights[1].ShadowMapIndex = 1;

                // ========================================================================
                // 点光源 2：跟随相机/玩家的光源（手电筒效果）
                // ========================================================================
                lightConstants.Lights[2].Strength = DirectX::XMFLOAT4(0.8f, 0.8f, 1.0f, 0.0f); // 冷白光
                lightConstants.Lights[2].Direction = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
                // Position 需要在每帧更新为相机位置
                lightConstants.Lights[2].Position = DirectX::XMFLOAT4(0.0f, 3.0f, 0.0f, 0.0f);
                lightConstants.Lights[2].FalloffStart = 0.5f;
                lightConstants.Lights[2].FalloffEnd = 8.0f;
                lightConstants.Lights[2].SpotPower = 0.0f;
                lightConstants.Lights[2].Range = 8.0f;
                lightConstants.Lights[2].CastShadow = 0;
                lightConstants.Lights[2].ShadowBias = 0.0005f;
                lightConstants.Lights[2].ShadowMapIndex = 2;

                lightConstants.Lights[3].Position = DirectX::XMFLOAT4(0.0f, 0.0f, 5.0f, 0.0f);
                lightConstants.Lights[3].Strength = DirectX::XMFLOAT4(0.5f, 0.5f, 0.5f, 0.0f);
                lightConstants.Lights[3].FalloffStart = 0.1f;
                lightConstants.Lights[3].FalloffEnd = 2.0f;
                lightConstants.NumPointLights = 4;

                // 上传到 GPU
                D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress =
                    m_context->FrameResourceManager->AllocateLight(&lightConstants, sizeof(LightConstants));

                // 存储到 GameContext 或 PassConstants 中，供 BeginFrame 使用
                m_context->lightCBAddress = lightCBAddress;

                m_context->FrameResourceManager->UpdatePassConstants();
            },
            "CameraUpdate");
    }

    // 8. 注册调试 UI 面板
    DebugUIManager::Get().RegisterPanel(
        {.name = "Performance", .group = "Performance", .drawFunc = [this](float dt, uint32_t frame) {
             static int targetFPS = 60;
             if (ImGui::SliderInt("Target FPS", &targetFPS, 0, 240)) {
                 m_context->FrameDriver->SetTargetFPS(targetFPS);
             }

             static bool fullscreen = false;
             if (ImGui::Checkbox("Fullscreen", &fullscreen)) {
                 auto *dispatcher = Event::MessageDispatcher::GetInstance();
                 if (dispatcher) {
                     dispatcher->PostEvent(Event::FullscreenToggleEvent::StaticTypeHash, 0, fullscreen ? 1u : 0u,
                                           Event::EventPriority::P1_High);
                 }
             }

             ImGui::Text("Game Delta Time: %.3f ms", dt * 1000.0f);
             ImGui::Text("Raw Delta Time: %.3f ms", m_context->MainTimer->GetRawDeltaTime() * 1000.0f);
             ImGui::Text("Raw FPS: %.1f", 1.0f / m_context->MainTimer->GetRawDeltaTime());
             ImGui::Text("FrameDriver Target: %d FPS", m_context->FrameDriver->GetTargetFPS());

             float rawFPS = 1.0f / m_context->MainTimer->GetRawDeltaTime();
             if (rawFPS > 62.0f && dt * 1000.0f > 15.5f) {
                 ImGui::TextColored(ImVec4(1, 1, 0, 1), "⚠️ Limited by DWM (windowed mode)");
             }
         }});

    m_isInitialized = true;
    m_context->Logging->Info("[Game] Game initialized successfully");
    return true;
}

void Game::RegisterEngineSystems() {
    // WindowResizeSystem
    SystemRegistry::Register({.name = "WindowResizeSystem",
                              .func =
                                  [this](Registry &, const MessageContext &ctx) {
                                      uint32_t width = ctx.GetLow32();
                                      uint32_t height = ctx.GetHigh32();
                                      if (m_context && m_context->DeviceContext) {
                                          m_context->DeviceContext->OnResize(width, height);
                                      }
                                      if (m_context && m_context->CameraMgr) {
                                          m_context->CameraMgr->OnResize(width, height);
                                      }
                                      if (m_opaqueRenderer) {
                                          m_opaqueRenderer->OnResize(width, height);
                                      }
                                  },
                              .phase = TaskPhase::EarlyUpdate,
                              .threadType = ThreadType::Main,
                              .interestedMessages = {Event::WindowResizeEvent::StaticTypeHash}});

    // FullscreenSystem
    SystemRegistry::Register({.name = "FullscreenSystem",
                              .func =
                                  [this](Registry &, const MessageContext &ctx) {
                                      bool bRequestFullscreen = ctx.GetLow32() != 0;
                                      m_context->Window->SetFullscreen(bRequestFullscreen);
                                  },
                              .phase = TaskPhase::EarlyUpdate,
                              .threadType = ThreadType::Main,
                              .priority = TaskPriority::High,
                              .interestedMessages = {Event::FullscreenToggleEvent::StaticTypeHash}});

    SystemRegistry::Register({.name = "PreRenderPipeline",
                              .func =
                                  [this](Registry &reg, const MessageContext &ctx) {
                                      // 串行执行三个步骤
                                      m_context->CullingSystem->Execute(reg, m_context->cullingResult);
                                      m_context->LODSystem->Execute(reg, m_context->lodResult);
                                      m_context->RenderItemBuilder->Execute(
                                          reg, m_context->cullingResult, m_context->lodResult, m_context->renderQueue);

                                      m_context->renderQueue.Sort();
                                  },
                              .phase = TaskPhase::PreRender,
                              .threadType = ThreadType::Worker,
                              .alwaysRun = true});
}

int Game::Run() {
    if (!m_isInitialized || !m_context->FrameDriver) {
        return -1;
    }

    m_context->Logging->Info("[Game] Starting game loop...");
    m_isRunning = true;
    m_context->Window->Show();
    m_context->FrameDriver->Initialize();

    while (m_isRunning && !m_context->Window->ShouldClose()) {
        m_context->MainTimer->Tick();
        m_context->FrameDriver->Tick();
    }

    m_context->FrameDriver->Stop();
    Shutdown();
    return 0;
}

void Game::Shutdown() {
    if (!m_isInitialized && !m_isRunning)
        return;

    m_isRunning = false;

    if (m_context) {
        m_context->FlushAllQueues();
    }

    m_world.Clear();
    SystemRegistry::Clear();

    if (m_context) {
        Resource::GpuResourceManager::GetInstance().Update(m_context->GetFenceValue());
    }

    Resource::GpuResourceManager::GetInstance().Shutdown();

    m_isInitialized = false;
    m_context->Logging->Info("[Game] Game shutdown complete");
}