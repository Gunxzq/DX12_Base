#include "Game.h"
#include "DebugUI/DebugUIManager.h"
#include "Event/MessageDispatcher.h"
#include "Framework/SystemRegistry.h"
#include "Platform/Input/InputSystem.h"
#include "Platform/Windows/Window.h"
#include "Renderer/Effects/AO/AmbientOcclusionManager.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/CameraManager.h"
#include "Renderer/Scene/ReflectionProbeManager/ReflectionProbeManager.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include "Resource/AssetLoader/AssetLoader.h"
#include "Resource/AssetLoader/Loader/DDSLoader.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Texture/TextureManager.h"
#include "Scheduler/FrameDriver.h"
#include "ThirdParty/imgui/imgui.h"

using namespace DX12Engine;
using namespace DX12Engine::DebugUI;
using namespace DX12Engine::Scheduler;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Input;
using namespace DX12Engine::Renderer;
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

    // 4. 初始化相机
    // 场景物体在 y=30 平面，x∈[-100,100], z∈[-100,100]
    // 相机放在场景前方，平视平面以看到 soldier 角色
    if (m_context->CameraMgr) {
        auto &mainCamera = m_context->CameraMgr->GetMainCamera();
        mainCamera.Position = DirectX::XMFLOAT3(0.0f, 25.0f, -30.0f);
        mainCamera.Rotation = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        m_context->CameraMgr->UpdateMainCamera();
    }

    // 初始化 LightManager
    LightManager::GetInstance().Initialize(m_context->DeviceContext->GetDevice(), m_context->DescriptorHeaps);
    LightManager::GetInstance().CreateTestLights(); // 创建测试光源

    // 初始化环境光遮蔽管理器（SSAO 资源分配 + SsaoRenderer）
    {
        auto &aoMgr = AmbientOcclusionManager::GetInstance();
        aoMgr.SetDeviceContext(m_context->DeviceContext);
        const auto &vp = m_context->DeviceContext->GetViewport();
        aoMgr.Initialize(m_context->DeviceContext->GetDevice(), m_context->DescriptorHeaps,
                         static_cast<uint32_t>(vp.Width), static_cast<uint32_t>(vp.Height));
        m_context->Logging->Info("[Game] AmbientOcclusionManager initialized");
        // SSAO 已添加 OnResize 支持，窗口缩放不再导致 TDR
        aoMgr.SetEnabled(true);
    }

    // 为主方向光预创建阴影贴图（2048x2048）
    LightManager::GetInstance().CreateShadowMapForDirectionalLight(0, 2048, m_context->GetNextFence());

    // 为第一个点光源预创建阴影贴图（1024x1024）
    LightManager::GetInstance().CreateShadowMapForPointLight(0, 1024, m_context->GetNextFence());

    // 反射探针管理器 — 已在 Bootstrap::CreateContext 中初始化，直接使用
    {
        auto &probeMgr = *m_context->ReflectionProbeMgr;

        // 创建测试探针（256x256, 普通优先级），位置与反射测试立方体重合
        probeMgr.AddProbe({10.0f, 32.0f, 3.0f}, 50.0f, 256, 1);
        m_context->Logging->Info("[Probe] Created test probe at (10, 32, 3), 256x256");
    }

    // 5. 注册引擎级系统（窗口大小变化、全屏切换等）
    RegisterEngineSystems();

    // 6. 初始化游戏模块（它们会自己注册游戏逻辑系统）
    m_world.Initialize(m_context, m_opaqueRenderer.get());

    // 上传 SSAO 随机向量纹理（命令管理器就绪后，与其他纹理上传在同一阶段）
    AmbientOcclusionManager::GetInstance().BuildRandomVectorTexture();

    // (InitializeResourceStates 已废弃——SSAO System 外层屏障 COMMON → RT 自动处理状态)

    // 连接 CullingSystem/VisibleRaycaster 引用，必须在 Initialize 之前完成
    // 因为 Initialize → RegisterPickingSystems() 需要这些指针非空
    m_inputHandler.SetVisibleRaycaster(m_context->VisibleRaycaster);

    m_inputHandler.Initialize(m_context);

    // 7. 注册帧同步回调：消费 raycastResult + 应用拖拽到 ECS
    if (m_context->FrameDriver) {
        m_context->FrameDriver->RegisterFrameSyncCallback(
            [this]() {
                // ── 缓存拾取结果（DebugUI 等消费）──
                const auto &result = m_context->raycastResult;
                if (result.HasAny()) {
                    const auto &closest = result.GetClosest();
                    m_pickedEntity = closest.entity;
                    m_pickedHitPoint = closest.hitPoint;
                    m_pickedDistance = closest.distance;
                    m_hasPickedResult = true;
                } else {
                    m_hasPickedResult = false;
                }

                // ── 安全地将拖拽意图写入 ECS 组件 ──
                m_inputHandler.ApplyDragToECS(*m_world.GetRegistry());
            },
            "PickingResultSync");
    }

    // 8. 注册相机更新回调（Immediate 路径）
    if (m_context->FrameDriver) {
        m_context->FrameDriver->RegisterImmediateCallback(
            [this]() {
                m_context->CameraMgr->UpdateMainCamera();

                const auto &camera = m_context->CameraMgr->GetMainCamera();
                PassConstants &passConstants = m_context->FrameResourceManager->GetPassConstants();

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

                // ========================================================================
                // 上传光源数据到 GPU（使用 UpdateAndUpload，内部脏标记自动判断是否更新）
                // 传入相机位置用于方向光阴影矩阵计算
                // ========================================================================
                LightManager::GetInstance().UpdateAndUpload(m_context->GetNextFence(), camera);

                m_context->FrameResourceManager->UpdatePassConstants();
            },
            "CameraUpdate");
    }

    // 9. 注册调试 UI 面板
    // ── Performance 面板 ──
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

    // ── Reflection Probe 面板 ──
    DebugUIManager::Get().RegisterPanel(
        {.name = "ReflectionProbes", .group = "Debug", .drawFunc = [this](float dt, uint32_t frame) {
             auto &probeMgr = *m_context->ReflectionProbeMgr;
             ImGui::Text("Active probes: %u", probeMgr.GetActiveProbeCount());
             ImGui::Separator();
             ImGui::Text("Probe array SRV: 0x%llx", probeMgr.GetProbeCubemapArraySRV().ptr);
             ImGui::Text("(Resource allocation verified at init)");
         }});

    // ── Picking 面板 ──
    DebugUIManager::Get().RegisterPanel(
        {.name = "Picking", .group = "Debug", .drawFunc = [this](float dt, uint32_t frame) {
             ImGui::Text("Frame: %u", frame);
             ImGui::Separator();

             if (m_hasPickedResult) {
                 ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Hit!");
                 ImGui::Text("  Entity:  %u", static_cast<uint32_t>(m_pickedEntity));
                 ImGui::Text("  Distance: %.2f", m_pickedDistance);
                 ImGui::Text("  Hit Point: (%.2f, %.2f, %.2f)", m_pickedHitPoint.x, m_pickedHitPoint.y,
                             m_pickedHitPoint.z);

                 // 显示本帧所有命中
                 const auto &allHits = m_context->raycastResult.hits;
                 if (allHits.size() > 1) {
                     ImGui::Separator();
                     ImGui::Text("All hits (%zu):", allHits.size());
                     for (size_t i = 0; i < allHits.size(); ++i) {
                         const auto &h = allHits[i];
                         ImGui::Text("  [%zu] Entity=%u  Dist=%.2f  Pos=(%.1f,%.1f,%.1f)", i,
                                     static_cast<uint32_t>(h.entity), h.distance, h.hitPoint.x, h.hitPoint.y,
                                     h.hitPoint.z);
                     }
                 }
             } else {
                 ImGui::TextDisabled("No hit this frame");
                 ImGui::TextDisabled("Ctrl+Click to pick entities");
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
                                      // G-buffer RT 尺寸跟随窗口缩放
                                      m_world.OnResize(width, height);
                                      // SSAO RT 尺寸跟随窗口缩放
                                      auto &aoMgr = DX12Engine::Renderer::AmbientOcclusionManager::GetInstance();
                                      if (aoMgr.IsInitialized()) {
                                          aoMgr.OnResize(width, height);
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

    SystemRegistry::Register({.name = "CullingCameraUpdate",
                              .func =
                                  [this](Registry &, const MessageContext &) {
                                      float dt = m_context->MainTimer->GetDeltaTime();
                                      float predictionFactor = 0.5f;
                                      auto &camMgr = CameraManager::GetInstance();
                                      m_context->predictedCameraData =
                                          camMgr.GetPredictedCameraData(dt, predictionFactor);
                                      m_context->CullingSystem->SetCamera(m_context->predictedCameraData);
                                  },
                              .phase = TaskPhase::LateUpdate,
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
        m_world.Update(); // 清理 BackgroundExecutor 已完成任务
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

    // 清理反射探针资源
    m_context->ReflectionProbeMgr->Shutdown();

    m_isInitialized = false;
    m_context->Logging->Info("[Game] Game shutdown complete");
}
