#include "Editor.h"
#include "Asset/IO/Loader/SceneDescription.h"
#include "Asset/IO/Loader/SceneLoader.h"
#include "DebugUI/DebugUIManager.h"
#include "EditorLayout.h"
#include "EditorStrings.h"
#include "EditorViewport.h"
#include "EditorViewportInputActions.h"
#include "Event/MessageDispatcher.h"
#include "Framework/SystemBuilder.h"
#include "Framework/SystemRegistry.h"
#include "ImGuizmo.h"
#include "Platform/Input/InputContextStack.h"
#include "Platform/Input/InputManager.h"
#include "Platform/Input/InputSystem.h"
#include "Platform/Windows/Window.h"
#include "Properties/ComponentEditorRegistrations.h"
#include "Renderer/Effects/AO/AmbientOcclusionManager.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/Material/MaterialManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/CameraManager.h"
#include "Renderer/Scene/GridManager.h"
#include "Renderer/Scene/LightManager/LightManager.h"
#include "Renderer/Scene/SkyboxManager.h"
#include "Renderer/Scene/WaterManager.h"
#include "Resource/AssetManager/AssetManager.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Pool/DepthStencilPool.h"
#include "Resource/Pool/RenderTargetPool.h"
#include "Scene/EditorSceneManager.h"
#include "Scene/SceneConstructor.h"
#include "Scheduler/FrameDriver.h"
#include "ThirdParty/imgui/imgui.h"
#include "Viewport/EditorViewportToolbar.h"
#include "Viewport/Systems/EditorCameraSystem.h"

#include <DirectXMath.h>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace DX12Engine;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::Scheduler;
using namespace DX12Engine::ECS;

Editor::Editor(Boot::GameContext *context) : m_context(context) {}

Editor::~Editor() { Shutdown(); }

bool Editor::Initialize() {
    if (!m_context || !m_context->IsValid()) {
        return false;
    }

    m_context->Logging->Info("[Editor] Initializing editor...");

    // 注册组件编辑器（属性卡系统）
    RegisterTransformEditor();
    RegisterLightEditor();
    m_context->Logging->Info("[Editor] Component editors registered");

    // 初始化相机配置（裁剪面、远平面），不设位置，后续由场景加载时恢复
    m_editorSceneMgr.InitCameraConfig(m_context);

    // 初始化环境光遮蔽管理器（SSAO，各端自行管理，尺寸随视口调整）
    {
        auto &aoMgr = DX12Engine::Renderer::AmbientOcclusionManager::GetInstance();
        aoMgr.SetDeviceContext(m_context->DeviceContext);
        aoMgr.Initialize(m_context->DeviceContext->GetDevice(), m_context->DescriptorHeaps,
                         m_viewport ? m_viewport->GetWidth() : 1280, m_viewport ? m_viewport->GetHeight() : 720);
        // 更新 RenderScene 中的 AO 引用（Bootstrap 已设置单例指针，但 Initialize 延迟至此）
        if (auto *rs = m_context->SceneMgr->GetRenderScene())
            rs->SetAmbientOcclusionManager(&aoMgr);
        m_context->Logging->Info("[Editor] AmbientOcclusionManager initialized");
    }

    // 初始化 LightManager（测试光源）
    LightManager::GetInstance().Initialize(m_context->DeviceContext->GetDevice(), m_context->DescriptorHeaps);
    LightManager::GetInstance().CreateTestLights();

    // 初始化 SkyboxManager
    SkyboxManager::GetInstance().Initialize(m_context->DeviceContext->GetDevice(), m_context->DescriptorHeaps,
                                            Resource::HeapTag::EditorViewport);
    m_context->Logging->Info("[Editor] SkyboxManager initialized");

    // 初始化编辑器场景管理器（组合包装 Bootstrap 的 SceneManager）
    m_editorSceneMgr.Initialize(m_context->SceneMgr, m_context);

    // 注册场景构造完成回调 System
    m_editorSceneMgr.RegisterSceneConstructSystem();

    // 传场景管理器引用给 OutlinerPanel
    m_outlinerPanel.SetEditorSceneManager(&m_editorSceneMgr);

    // 初始化 WaterManager
    WaterManager::GetInstance().Initialize(m_context->DeviceContext->GetDevice());
    m_context->Logging->Info("[Editor] WaterManager initialized");

    // 初始化 GridManager（全局网格单例）
    GridManager::GetInstance().Initialize(m_context->DeviceContext->GetDevice());
    m_context->Logging->Info("[Editor] GridManager initialized");

    // 初始化帧临时上传分配器（64KB 单块，共 2 块）
    m_scratchAllocator.Initialize(m_context->DeviceContext->GetDevice(), 64 * 1024, L"EditorScratchAlloc");
    m_context->Logging->Info("[Editor] FrameScratchAllocator initialized");

    // 初始化资产预览管理器
    m_previewManager.Initialize(m_context->DeviceContext->GetDevice(), m_context->DeviceContext,
                                &RenderTargetPool::GetInstance());
    m_context->Logging->Info("[Editor] PreviewManager initialized");

    // 初始化缩略图纹理数组
    if (m_thumbnailArray.Initialize(m_context->DeviceContext->GetDevice(), m_context->DescriptorHeaps)) {
        m_context->Logging->Info("[Editor] ThumbnailArray initialized (capacity=%u)", m_thumbnailArray.GetCapacity());
    } else {
        m_context->Logging->Warn("[Editor] ThumbnailArray initialization failed, thumbnails disabled");
    }

    // 初始化预览缓存管理器（缩略图 DDS 磁盘缓存）
    {
        std::string cacheDir =
            (std::filesystem::path(m_context->GetProjectConfig().Root) / "Content/Cache/Thumbnails/").string();
        m_previewCache.SetCacheDirectory(cacheDir);
        m_context->Logging->Info("[Editor] PreviewCache initialized: {}", cacheDir);
    }

    // 创建预览 PBR 渲染器
    m_previewRenderer.SetDeviceContext(m_context->DeviceContext);
    m_previewRenderer.Initialize();
    m_context->Logging->Info("[Editor] PreviewPBRRenderer initialized");

    // ── 初始化场景渲染管线（Opaque + Lighting） ──
    m_opaqueRenderer = std::make_unique<Renderer::OpaqueRenderer>();
    m_opaqueRenderer->SetDeviceContext(m_context->DeviceContext);
    m_opaqueRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_opaqueRenderer->SetMaterialManager(m_context->MaterialMgr);
    m_opaqueRenderer->Initialize();
    m_context->Logging->Info("[Editor] OpaqueRenderer initialized");

    m_lightingRenderer = std::make_unique<Renderer::LightingRenderer>();
    m_lightingRenderer->SetDeviceContext(m_context->DeviceContext);
    m_lightingRenderer->Initialize();
    m_context->Logging->Info("[Editor] LightingRenderer initialized");

    m_opaqueBuilder = std::make_unique<Renderer::OpaqueRenderItemBuilder>(
        m_context->FrameResourceManager, m_context->MaterialMgr, m_context->TextureMgr);
    m_context->Logging->Info("[Editor] OpaqueRenderItemBuilder initialized");

    // 注册编辑器渲染系统（构建器 + 实体渲染器）
    RegisterEditorRenderSystems();

    // 创建内置球体 mesh（纹理/材质预览用）
    {
        auto sphere = m_previewRenderer.CreatePreviewSphere(Resource::GpuResourceManager::GetInstance(),
                                                            m_context->GeometryResourceManager);
        if (sphere) {
            m_context->Logging->Info("[Editor] Preview sphere created: {} verts, {} indices", sphere->vertexCount,
                                     sphere->indexCount);
        } else {
            m_context->Logging->Warn("[Editor] Failed to create preview sphere (will fall back to loaded meshes)");
        }
    }

    // 注册引擎级系统
    RegisterEngineSystems();

    // 初始化编辑器布局
    m_layout = std::make_unique<EditorLayout>(m_context);
    if (!m_layout->Initialize()) {
        m_context->Logging->Error("[Editor] Failed to initialize editor layout");
        return false;
    }

    // ── 初始化并注册面板 ──

    // 初始化控制台面板（创建 spdlog sink 并注册）
    m_consolePanel.Initialize();

    // 初始化文件浏览器（设置 Content 根目录）
    if (m_context->ProjectConfig) {
        m_assetBrowser.SetContentRoot(m_context->ProjectConfig->ContentRoot);
        m_assetManager.SetContentRoot(m_context->ProjectConfig->ContentRoot);
        if (m_context->DeviceContext) {
            m_assetManager.SetDevice(m_context->DeviceContext->GetDevice());
            m_assetManager.SetCommandManager(&m_context->DeviceContext->GetCommandManager());
        }
    }

    // 设置预览渲染上下文（AssetBrowser 直接持有，不再通过 Layout 转发）
    m_assetManager.SetPreviewContext(&m_previewManager, &m_previewRenderer, &m_thumbnailArray, &m_scratchAllocator,
                                     m_context);

    // 设置缩略图数组引用
    m_assetManager.SetThumbnailArray(&m_thumbnailArray);

    // 加载默认场景（标准天空盒 + 空世界，此时 m_gameCtx 已就绪）
    m_assetManager.LoadSceneDescription(m_editorSceneMgr.GetDefaultSceneDescription());

    // 加载磁盘缓存的缩略图
    m_assetManager.LoadThumbnailPack(m_context->DeviceContext);

    // 设置资产浏览器布局代理（预览 ID 同步、面板显示等）
    // 这些回调通过 Layout 的预览状态和 Properties 面板交互
    m_assetManager.SetLayoutProxy(
        [this](PreviewId id) {
            // 通过 Layout 设置预览 ID（Properties 面板读取）
            // 当前 Layout 的 DrawProperties 直接读取 m_previewManager 和 m_previewId
            // 后期当 Properties 独立为 Panel 后，改为直接通知 PropertiesPanel
            m_layout->SetPreviewId(id);
            m_layout->SetPreviewManager(&m_previewManager);
        },
        [this]() { m_layout->ShowPreviewPanel(); });

    // 设置场景切换回调（AssetBrowser 加载新场景前释放旧场景资源）
    m_assetManager.SetSceneSwitcher(
        [this](const std::string &sceneName, const std::filesystem::path &sceneFilePath) -> bool {
            return m_editorSceneMgr.SwitchScene(sceneName.empty() ? sceneFilePath.stem().string() : sceneName,
                                                sceneFilePath);
        });

    // 注册面板到 Layout
    m_layout->RegisterPanel(&m_assetManager);
    m_layout->RegisterPanel(&m_assetBrowser);
    m_layout->RegisterPanel(&m_consolePanel);
    m_layout->RegisterPanel(&m_outlinerPanel);

    // 初始化编辑器视口预览
    m_viewport = std::make_unique<EditorViewport>(m_context);
    if (!m_viewport->Initialize()) {
        m_context->Logging->Error("[Editor] Failed to initialize editor viewport");
        return false;
    }

    // 将视口 SRV 传递给布局
    m_layout->SetViewportSRV(m_viewport->GetOutputSRV());
    m_layout->SetViewportSize(m_viewport->GetWidth(), m_viewport->GetHeight());

    // 初始化视口输入处理（旧系统，已注释）
    // m_viewportInput = std::make_unique<EditorViewportInput>();
    // m_viewportInput->Initialize(m_context);
    // m_viewportInput->SetEditorSceneManager(m_editorSceneMgr.GetSceneManager());
    // m_viewportInput->SetViewportSize(m_viewport->GetWidth(), m_viewport->GetHeight());

    // 初始化声明式输入驱动的相机系统
    m_cameraSystem = std::make_unique<EditorCameraSystem>();
    m_cameraSystem->Initialize(m_context);
    m_cameraSystem->SetEditorSceneManager(m_editorSceneMgr.GetSceneManager());
    m_cameraSystem->SetViewportSize(m_viewport->GetWidth(), m_viewport->GetHeight());
    m_cameraSystem->SetGetCurrentToolCallback(
        [this]() -> ViewportTool { return m_toolbar ? m_toolbar->GetCurrentTool() : ViewportTool::Cursor; });
    m_cameraSystem->SetGetCursorModeCallback(
        [this]() -> CursorMode { return m_toolbar ? m_toolbar->GetCursorMode() : CursorMode::View; });

    // 初始化视口工具栏
    m_toolbar = std::make_unique<EditorViewportToolbar>();
    m_toolbar->Initialize(m_context);

    // 注册视口工具栏回调（在 EditorLayout::DrawViewport 中叠加）
    if (m_layout) {
        m_layout->SetViewportToolbarCallback([this](ImVec2 viewportPos, ImVec2 viewportSize) {
            // 无 Tab 时不显示工具栏
            if (m_toolbar && !m_editorSceneMgr.GetOpenTabs().empty()) {
                m_toolbar->DrawToolbar(viewportPos, viewportSize);
            }
        });

        // 注册 Gizmo 操作类型回调（将工具栏工具模式映射到 ImGuizmo 操作）
        m_layout->SetGetGizmoOpCallback(
            [this]() -> int { return m_toolbar ? m_toolbar->GetCurrentGizmoOp() : ImGuizmo::TRANSLATE; });
    }

    // 注册相机更新回调（Immediate 路径）
    if (m_context->FrameDriver) {
        m_context->FrameDriver->RegisterImmediateCallback(
            [this]() {
                // ── 视口输入处理（旧系统，已注释，由 EditorCameraSystem 的输入回调替代） ──
                // if (m_viewportInput) {
                //    m_viewportInput->SetViewportHovered(m_layout->IsViewportHovered());
                //    m_viewportInput->Update(m_context->MainTimer->GetDeltaTime());
                //
                //    // ── F 键：聚焦到选中实体 ──
                //    if (m_context->InputMgr) {
                //        auto *inputSys = m_context->InputMgr->GetInputSystem();
                //        if (inputSys && inputSys->IsActionPressed(DX12Engine::Input::ActionId_FocusSelection)) {
                //            DX12Engine::ECS::Entity selected = m_outlinerPanel.GetSelectedEntity();
                //            if (selected != DX12Engine::ECS::INVALID_ENTITY && m_editorSceneMgr.GetSceneManager()) {
                //                m_viewportInput->FocusOnEntity(selected, 10.0f);
                //            }
                //        }
                //    }
                //}

                // 新系统：更新相机悬停状态
                if (m_cameraSystem) {
                    m_cameraSystem->SetViewportHovered(m_layout->IsViewportHovered());
                }

                // 更新 PassConstants（由 EditorCameraSystem 统一处理）
                if (m_cameraSystem) {
                    m_cameraSystem->UpdatePassConstants();
                } else {
                    // 回退：旧路径
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

                // 获取当前相机引用（用于后续 LightManager/WaterManager/GridManager 更新）
                const auto &camera = m_context->CameraMgr->GetMainCamera();

                auto *renderScene = m_context->SceneMgr->GetRenderScene();
                if (renderScene && renderScene->GetLightManager())
                    renderScene->GetLightManager()->UpdateAndUpload(m_context->GetNextFence(), camera);
                WaterManager::GetInstance().UpdateAndUpload(m_context->GetNextFence());
                GridManager::GetInstance().UpdateAndUpload(m_context->GetNextFence(), camera.ViewProjMatrix,
                                                           camera.Position);

                m_context->FrameResourceManager->UpdatePassConstants();

                // ── 视口悬停检测与输入上下文切换 ──
                {
                    auto *inputMgr = m_context->InputMgr;
                    bool viewportHovered = m_layout->IsViewportHovered();
                    static bool wasViewportHovered = false;

                    if (viewportHovered && !wasViewportHovered) {
                        inputMgr->PushContext(EditorStrings::Get("viewport", "Viewport"));
                    } else if (!viewportHovered && wasViewportHovered) {
                        inputMgr->PopContext();
                    }
                    wasViewportHovered = viewportHovered;
                }

                // ── Outliner 焦点检测与输入上下文切换 ──
                {
                    auto *inputMgr = m_context->InputMgr;
                    bool outlinerFocused = m_outlinerPanel.IsOutlinerFocused();
                    static bool wasOutlinerFocused = false;

                    if (outlinerFocused && !wasOutlinerFocused) {
                        inputMgr->PushContext("Outliner");
                    } else if (!outlinerFocused && wasOutlinerFocused) {
                        inputMgr->PopContext();
                    }
                    wasOutlinerFocused = outlinerFocused;
                }

                // 渲染编辑器视口 — 尺寸检查
                if (m_viewport) {
                    uint32_t vpW = m_layout->GetViewportWidth();
                    uint32_t vpH = m_layout->GetViewportHeight();
                    if (vpW > 0 && vpH > 0 && (vpW != m_viewport->GetWidth() || vpH != m_viewport->GetHeight())) {
                        m_viewport->OnResize(vpW, vpH);
                        m_layout->SetViewportSRV(m_viewport->GetOutputSRV());
                        m_layout->SetViewportSize(vpW, vpH);
                        // if (m_viewportInput)
                        //     m_viewportInput->SetViewportSize(vpW, vpH);
                        if (m_cameraSystem)
                            m_cameraSystem->SetViewportSize(vpW, vpH);
                    }
                }
            },
            "EditorCameraUpdate");
    }

    // 所有模块初始化完成后再注册绘制回调，确保第一帧时所有资源已就绪
    m_layout->Register();

    // 注册场景 Tab 栏绘制回调（在视口窗口顶部，内容区域之前渲染，传入 SRV）
    m_layout->SetViewportTabBarCallback([this](ImTextureID srv, ImVec2 *outImageMin, ImVec2 *outImageMax) {
        m_editorSceneMgr.DrawTabBar(srv, outImageMin, outImageMax);
    });

    // 注册场景加载回调（Tab 切换时触发，通过 SceneConstructor 异步加载场景）
    m_editorSceneMgr.SetOnLoadSceneCallback(
        [this](const std::string &sceneName, const std::filesystem::path &sceneFilePath) {
            m_context->Logging->Info("[Editor] Tab switch triggered loading: '{}' (path: {})", sceneName,
                                     sceneFilePath.string());
            // 直接通过 AssetManager 加载场景文件，不触发 SwitchScene 回调
            // （SwitchScene 已在 ProcessPendingTabSwitch 中完成）
            m_assetManager.LoadSceneFromFile(sceneFilePath);
        });

    m_isInitialized = true;
    m_context->Logging->Info("[Editor] Editor initialized successfully");
    return true;
}

void Editor::RegisterEngineSystems() {
    // WindowResizeSystem
    SystemRegistry::Register({.name = "WindowResizeSystem",
                              .func =
                                  [this](const MessageContext &ctx) {
                                      uint32_t width = ctx.GetLow32();
                                      uint32_t height = ctx.GetHigh32();
                                      if (m_context && m_context->DeviceContext) {
                                          m_context->DeviceContext->OnResize(width, height);
                                      }
                                      if (m_context && m_context->CameraMgr) {
                                          m_context->CameraMgr->OnResize(width, height);
                                      }
                                      // SSAO RT 尺寸跟随窗口缩放（OnResize 内部有 m_initialized 守卫）
                                      auto &aoMgr = DX12Engine::Renderer::AmbientOcclusionManager::GetInstance();
                                      if (aoMgr.IsInitialized())
                                          aoMgr.OnResize(width, height);
                                  },
                              .phase = TaskPhase::EarlyUpdate,
                              .threadType = ThreadType::Main,
                              .interestedMessages = {Event::WindowResizeEvent::StaticTypeHash}});

    // FullscreenSystem
    SystemRegistry::Register({.name = "FullscreenSystem",
                              .func =
                                  [this](const MessageContext &ctx) {
                                      bool bRequestFullscreen = ctx.GetLow32() != 0;
                                      m_context->Window->SetFullscreen(bRequestFullscreen);
                                  },
                              .phase = TaskPhase::EarlyUpdate,
                              .threadType = ThreadType::Main,
                              .priority = TaskPriority::High,
                              .interestedMessages = {Event::FullscreenToggleEvent::StaticTypeHash}});

    // ── EditorMainClearSystem：清除交换链 backbuffer + depth ──
    SystemRegistry::Register(
        {.name = "EditorMainClearSystem",
         .func =
             [this](const MessageContext &) {
                 if (!m_context || !m_context->DeviceContext)
                     return;

                 auto *deviceCtx = m_context->DeviceContext;
                 auto &cmdMgr = deviceCtx->GetCommandManager();

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocHandle = cmdMgr.AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto *alloc = cmdMgr.GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle);
                 auto cmdHandle = cmdMgr.AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmdList = cmdMgr.GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdHandle);

                 auto backBuffer = m_context->GetBackBuffer();
                 auto rtvHandle = deviceCtx->GetCurrentBackBufferView();
                 auto dsvHandle = deviceCtx->GetDepthStencilView();

                 // Barrier: PRESENT → RENDER_TARGET
                 D3D12_RESOURCE_BARRIER barrier = {};
                 barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 barrier.Transition.pResource = backBuffer;
                 barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                 barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 // 深度缓冲从 DEPTH_WRITE → DEPTH_WRITE（保持状态，仅为一致性）
                 // 注：深度缓冲初始状态为 DEPTH_WRITE，无需转换

                 // Clear backbuffer + depth
                 const float clearColor[] = {0.0f, 0.0f, 0.0f, 0.0f};
                 cmdList.Get()->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
                 cmdList.Get()->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                                      1.0f, 0, 0, nullptr);

                 // 设置视口 + 裁剪矩形
                 const auto &viewport = deviceCtx->GetViewport();
                 const auto &scissorRect = deviceCtx->GetScissorRect();
                 cmdList.Get()->RSSetViewports(1, &viewport);
                 cmdList.Get()->RSSetScissorRects(1, &scissorRect);
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 // Barrier 回退: RENDER_TARGET → PRESENT
                 barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::PrePass, cmdHandle);

                 uint64_t seq = m_context->GetNextSequence();
                 cmdMgr.ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::PrePass,
         .alwaysRun = true});

    // ── CullingCameraUpdate：更新剔除视锥（与 Game 端一致，LateUpdate 阶段） ──
    SystemRegistry::Register({.name = "EditorCullingCameraUpdate",
                              .func =
                                  [this](const MessageContext &) {
                                      float dt = m_context->MainTimer->GetDeltaTime();
                                      float predictionFactor = 0.5f;
                                      auto &camMgr = DX12Engine::Renderer::CameraManager::GetInstance();
                                      m_context->predictedCameraData =
                                          camMgr.GetPredictedCameraData(dt, predictionFactor);
                                      m_context->CullingSystem->SetCamera(m_context->predictedCameraData);
                                  },
                              .phase = TaskPhase::LateUpdate,
                              .threadType = ThreadType::Worker,
                              .alwaysRun = true});

    // ── PreviewRenderSystem：遍历预览上下文，直接调用渲染回调 ──
    SystemRegistry::Register({.name = "PreviewRenderSystem",
                              .func =
                                  [this](const MessageContext &) {
                                      if (!m_isInitialized)
                                          return;
                                      m_previewManager.RenderPreviews();
                                  },
                              .phase = TaskPhase::Render,
                              .threadType = ThreadType::Render,
                              .priority = TaskPriority::Low,
                              .renderPhase = RenderPhase::PostProcess,
                              .alwaysRun = true});
}

// ========================================================================
// 注册编辑器渲染系统（构建器 + 实体渲染器）
// 阶段必须与 Game 端一致，否则会产生异常
// ========================================================================

void Editor::RegisterEditorRenderSystems() {
    using namespace DX12Engine::Renderer;

    // ── BuilderUpload：设置相机/视锥到构建器（PreRender 阶段） ──
    REGISTER_SYSTEM(EditorBuilderUpload, PreRender, Worker)
        .Func([this](const MessageContext &) {
            if (!m_context->CullingSystem || !m_context->CameraMgr)
                return;
            const Frustum &frustum = m_context->CullingSystem->GetFrustum();
            auto camPos = m_context->CameraMgr->GetMainCamera().Position;
            m_opaqueBuilder->SetFrustum(&frustum);
            m_opaqueBuilder->SetCameraPos(camPos);
            m_opaqueBuilder->SetLODSystem(m_context->LODSystem);

            // 设置实体过滤器（按 SceneTagComponent 过滤，只处理当前活跃场景的实体）
            uint64_t activeSceneId = m_editorSceneMgr.GetActiveSceneId();
            auto *registry = m_editorSceneMgr.GetRegistry();
            m_opaqueBuilder->SetEntityFilter([activeSceneId, registry](DX12Engine::ECS::Entity entity) -> bool {
                if (!registry)
                    return true;
                auto *tag = registry->TryGetComponent<SceneTagComponent>(entity);
                return tag && tag->sceneId == activeSceneId;
            });
        })
        .AlwaysRun()
        .Build();

    // ── BuildOpaque：从 ECS 收集不透明渲染项（PreRender 阶段，依赖 BuilderUpload） ──
    REGISTER_SYSTEM(EditorBuildOpaque, PreRender, Worker)
        .Func([this](const MessageContext &) {
            m_opaqueBuilder->BuildTyped(*m_editorSceneMgr.GetRegistry(), m_opaqueQueue);
        })
        .AlwaysRun()
        .DependsOn("EditorBuilderUpload")
        .Build();

    // FrameSync 回调：上传实例数据到 GPU RingBuffer
    if (m_context->FrameDriver) {
        m_context->FrameDriver->RegisterFrameSyncCallback(
            [this]() {
                auto *frameRes = m_context->FrameResourceManager;
                if (!frameRes)
                    return;
                auto &batches = m_opaqueBuilder->GetPendingBatches();
                auto &queue = m_opaqueQueue;
                for (auto &batch : batches) {
                    if (batch.queueIndex >= queue.Size() || batch.instances.empty())
                        continue;
                    D3D12_GPU_VIRTUAL_ADDRESS addr =
                        frameRes->Allocate("Instance", batch.instances.data(),
                                           static_cast<uint32_t>(batch.instances.size() * sizeof(InstanceData)));
                    queue[batch.queueIndex].instanceBuffer = addr;
                }
                batches.clear();
            },
            "FrameSync_EditorUploadInstanceData");
    }

    // ── EditorOpaqueRenderSystem：G-buffer 通道（RenderPhase::Opaque） ──
    // 使用 EditorViewport 的 ApplicationRenderTargets 作为 G-buffer
    SystemRegistry::Register(
        {.name = "EditorOpaqueRenderSystem",
         .func =
             [this](const MessageContext &) {
                 if (!m_opaqueRenderer || m_opaqueQueue.Empty())
                     return;
                 auto *vpRTs = m_viewport ? m_viewport->GetAppRTs() : nullptr;
                 if (!vpRTs || !vpRTs->IsInitialized())
                     return;

                 auto &rtPool = RenderTargetPool::GetInstance();

                 uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                 auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                 auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);

                 D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {
                     rtPool.GetRtvHandle(vpRTs->GetGBufferAlbedo()),
                     rtPool.GetRtvHandle(vpRTs->GetGBufferNormal()),
                     rtPool.GetRtvHandle(vpRTs->GetGBufferMaterial()),
                     rtPool.GetRtvHandle(vpRTs->GetGBufferWorldPos()),
                 };
                 if (rtvs[0].ptr == 0 || rtvs[1].ptr == 0 || rtvs[2].ptr == 0 || rtvs[3].ptr == 0)
                     return;

                 // 资源屏障：COMMON → RENDER_TARGET
                 auto barrierRT = [&](ID3D12Resource *res) {
                     if (!res)
                         return;
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_COMMON,
                                                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                     cmd.Get()->ResourceBarrier(1, &b);
                 };
                 barrierRT(vpRTs->GetGBufferAlbedoResource());
                 barrierRT(vpRTs->GetGBufferNormalResource());
                 barrierRT(vpRTs->GetGBufferMaterialResource());
                 barrierRT(vpRTs->GetGBufferWorldPosResource());

                 // 设置视口和裁剪矩形（使用 EditorViewport 尺寸，与 G-buffer RT 匹配）
                 D3D12_VIEWPORT vp = {0, 0, (float)m_viewport->GetWidth(), (float)m_viewport->GetHeight(), 0, 1};
                 D3D12_RECT sr = {0, 0, (LONG)m_viewport->GetWidth(), (LONG)m_viewport->GetHeight()};
                 cmd.Get()->RSSetViewports(1, &vp);
                 cmd.Get()->RSSetScissorRects(1, &sr);

                 ID3D12DescriptorHeap *heaps[] = {m_context->DescriptorHeaps->GetHeap(
                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, HeapTag::EditorViewport)};
                 cmd.Get()->SetDescriptorHeaps(1, heaps);

                 auto dsvHandle = m_viewport ? m_viewport->GetDSVHandle() : D3D12_CPU_DESCRIPTOR_HANDLE{};
                 if (dsvHandle.ptr == 0)
                     return;

                 // 深度缓冲屏障：COMMON → DEPTH_WRITE
                 auto &dsPool = DepthStencilPool::GetInstance();
                 ID3D12Resource *depthRes = dsPool.GetResource(m_viewport->GetDepthHandle());
                 if (depthRes) {
                     auto depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(depthRes, D3D12_RESOURCE_STATE_COMMON,
                                                                              D3D12_RESOURCE_STATE_DEPTH_WRITE);
                     cmd.Get()->ResourceBarrier(1, &depthBarrier);
                 }

                 cmd.Get()->OMSetRenderTargets(4, rtvs, FALSE, &dsvHandle);

                 const float clearColor[4] = {0, 0, 0, 0};
                 cmd.Get()->ClearRenderTargetView(rtvs[0], clearColor, 0, nullptr);
                 cmd.Get()->ClearRenderTargetView(rtvs[1], clearColor, 0, nullptr);
                 cmd.Get()->ClearRenderTargetView(rtvs[2], clearColor, 0, nullptr);
                 cmd.Get()->ClearRenderTargetView(rtvs[3], clearColor, 0, nullptr);

                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 D3D12_GPU_DESCRIPTOR_HANDLE matSRV = m_context->MaterialMgr->GetMaterialBufferSRV();
                 D3D12_GPU_DESCRIPTOR_HANDLE texHeapStart = m_context->DescriptorHeaps->GetPartitionGpuHandle(
                     PartitionType::Texture, 0, HeapTag::EditorViewport);

                 m_opaqueRenderer->BeginFrameGBuffer(cmd, passCBAddr, matSRV, texHeapStart);

                 for (const auto &item : m_opaqueQueue) {
                     if (!item.IsValid())
                         continue;
                     m_opaqueRenderer->DrawInstancedGBuffer(cmd, item.geometryHandle, item.instanceBuffer,
                                                            item.instanceCount, item.startIndex, item.startVertex,
                                                            item.indexCount);
                 }

                 m_opaqueRenderer->EndFrameGBuffer();

                 // 屏障：RENDER_TARGET → COMMON
                 auto barrierBack = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                   D3D12_RESOURCE_STATE_COMMON);
                     cmd.Get()->ResourceBarrier(1, &b);
                 };
                 barrierBack(vpRTs->GetGBufferAlbedoResource());
                 barrierBack(vpRTs->GetGBufferNormalResource());
                 barrierBack(vpRTs->GetGBufferMaterialResource());
                 barrierBack(vpRTs->GetGBufferWorldPosResource());

                 // 深度缓冲不移回 COMMON，由后续的 PostProcess（Grid）统一回退

                 cmd.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Opaque, cmdH);
                 uint64_t seq = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Opaque,
         .alwaysRun = true});

    // ── EditorLightingRenderSystem：光照 Pass（RenderPhase::Lighting） ──
    SystemRegistry::Register(
        {.name = "EditorLightingRenderSystem",
         .func =
             [this](const MessageContext &) {
                 if (!m_lightingRenderer)
                     return;
                 auto *vpRTs = m_viewport ? m_viewport->GetAppRTs() : nullptr;
                 if (!vpRTs || !vpRTs->IsInitialized())
                     return;

                 uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                 auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                 auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);

                 // 屏障：G-buffer SRV 状态
                 auto barrierToSRV = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_COMMON,
                                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                     cmd.Get()->ResourceBarrier(1, &b);
                 };
                 barrierToSRV(vpRTs->GetGBufferAlbedoResource());
                 barrierToSRV(vpRTs->GetGBufferNormalResource());
                 barrierToSRV(vpRTs->GetGBufferMaterialResource());
                 barrierToSRV(vpRTs->GetGBufferWorldPosResource());

                 // 屏障：场景颜色 RT COMMON → RENDER_TARGET
                 auto &rtPool = RenderTargetPool::GetInstance();
                 ID3D12Resource *sceneColorRes = vpRTs->GetSceneColorResource();
                 if (!sceneColorRes)
                     return;
                 {
                     auto sceneBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
                         sceneColorRes, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
                     cmd.Get()->ResourceBarrier(1, &sceneBarrier);
                 }

                 // 设置描述符堆（BeginFrame 内部会设置根描述符表）
                 ID3D12DescriptorHeap *heaps[] = {m_context->DescriptorHeaps->GetHeap(
                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, HeapTag::EditorViewport)};
                 cmd.Get()->SetDescriptorHeaps(1, heaps);

                 // 绑定场景颜色 RT 为渲染目标
                 D3D12_CPU_DESCRIPTOR_HANDLE sceneRTV = rtPool.GetRtvHandle(vpRTs->GetSceneColor());
                 if (sceneRTV.ptr == 0)
                     return;
                 cmd.Get()->OMSetRenderTargets(1, &sceneRTV, FALSE, nullptr);

                 // 设置视口和裁剪矩形（使用 EditorViewport 尺寸，与 RT 匹配）
                 D3D12_VIEWPORT vp = {0, 0, (float)m_viewport->GetWidth(), (float)m_viewport->GetHeight(), 0, 1};
                 D3D12_RECT sr = {0, 0, (LONG)m_viewport->GetWidth(), (LONG)m_viewport->GetHeight()};
                 cmd.Get()->RSSetViewports(1, &vp);
                 cmd.Get()->RSSetScissorRects(1, &sr);

                 m_lightingRenderer->BeginFrame(
                     cmd, m_context->FrameResourceManager->GetPassCBAddress(),
                     m_context->SceneMgr->GetRenderScene()->GetLightManager()->GetLightCBAddress(),
                     vpRTs->GetGBufferAlbedoSRV(), vpRTs->GetGBufferNormalSRV(), vpRTs->GetGBufferMaterialSRV(),
                     vpRTs->GetGBufferWorldPosSRV(), {} /* ssaoSrv */, {} /* envMapSrv */, {} /* cubemapArraySrv */,
                     {} /* shadowDataSRV */, {} /* shadowMapSRV */);
                 m_lightingRenderer->Draw(cmd);
                 m_lightingRenderer->EndFrame();

                 // 屏障回退：G-buffer SRV → COMMON
                 auto barrierToCommon = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                                   D3D12_RESOURCE_STATE_COMMON);
                     cmd.Get()->ResourceBarrier(1, &b);
                 };
                 barrierToCommon(vpRTs->GetGBufferAlbedoResource());
                 barrierToCommon(vpRTs->GetGBufferNormalResource());
                 barrierToCommon(vpRTs->GetGBufferMaterialResource());
                 barrierToCommon(vpRTs->GetGBufferWorldPosResource());

                 // 屏障：场景颜色 RT RENDER_TARGET → COMMON
                 if (sceneColorRes) {
                     auto sceneBack = CD3DX12_RESOURCE_BARRIER::Transition(
                         sceneColorRes, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
                     cmd.Get()->ResourceBarrier(1, &sceneBack);
                 }

                 cmd.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Lighting, cmdH);
                 uint64_t seq = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Lighting,
         .alwaysRun = true});
}

void Editor::FinalizePreviewMesh() {
    // 轮询等待 GeometryResourceManager 中的待处理加载完成
    // 用于预览 mesh 的异步加载完成后的最终确认
    // 当前实现：检查 AssetBrowser 的预览状态，如果正在加载则等待
    // 具体逻辑在 AssetBrowser 的 OnFileDoubleClick 回调中处理
}

void Editor::CachePreviewThumbnail(PreviewId id) {
    if (!m_thumbnailArray.IsInitialized() || !m_previewCache.IsInitialized())
        return;

    auto *ctx = m_previewManager.GetContext(id);
    if (!ctx || ctx->arraySlice == UINT32_MAX)
        return;

    // 回读预览渲染结果
    auto result = m_thumbnailArray.ReadbackSlice(ctx->arraySlice, m_context->DeviceContext);
    if (!result.pixels)
        return;

    // 生成缓存键
    std::string cacheKey = m_previewCache.MakeCacheKey(m_assetManager.GetPreviewFilePath());

    // 写入 DDS 磁盘缓存
    m_previewCache.WriteDDS(cacheKey, result.width, result.height, result.pixels);

    // 更新缩略图映射
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = {};
    // 在 ImGui 堆中分配 SRV 描述符
    auto &debugUI = DebugUI::DebugUIManager::Get();
    debugUI.AllocateSrvDescriptor(&srvCpu, &srvGpu);
    m_thumbnailArray.CreateSliceSRV(ctx->arraySlice, srvCpu);
    m_assetManager.RegisterThumbnail(m_assetManager.GetPreviewFilePath(), srvGpu, ctx->arraySlice);

    PreviewCacheManager::FreeDDSData(result.pixels);
}

int Editor::Run() {
    if (!m_isInitialized) {
        m_context->Logging->Error("[Editor] Run() called before Initialize()");
        return 1;
    }

    m_isRunning = true;
    m_context->Logging->Info("[Editor] Editor main loop started");

    while (m_isRunning && !m_context->Window->ShouldClose()) {

        // 1. 更新计时器
        m_context->MainTimer->Tick();

        // 1b. 同步选中实体到 Layout（供 Properties 面板使用）
        m_layout->SetSelectedEntity(m_outlinerPanel.GetSelectedEntity());

        // 2. 推进异步加载
        m_context->BackgroundExecutor->Tick();

        // 3. 驱动帧循环
        m_context->FrameDriver->Tick();

        // 处理待处理的 Tab 切换（ImGui 渲染完成后执行，避免在渲染中切换场景）
        m_editorSceneMgr.ProcessPendingTabSwitch();

        // 4. 检查是否需要缓存缩略图
        if (m_assetManager.NeedsThumbnailCache()) {
            CachePreviewThumbnail(m_assetManager.GetDetailPreviewId());
            m_assetManager.ClearThumbnailCacheFlag();
        }

        // ── 统一回收所有延迟释放的 GPU 资源（主线程，每帧） ──
        {
            auto &cmdMgr = m_context->DeviceContext->GetCommandManager();
            auto &fenceMgr = cmdMgr.GetFenceManager();
            auto *directFence = fenceMgr.GetFence(D3D12_COMMAND_LIST_TYPE_DIRECT);
            uint64_t completedFence = directFence ? directFence->Get()->GetCompletedValue() : 0;

            if (completedFence > 0) {
                Resource::GpuResourceManager::GetInstance().Update(completedFence);
                Resource::DepthStencilPool::GetInstance().Reclaim(completedFence);
                Resource::RenderTargetPool::GetInstance().Reclaim(completedFence);

                if (m_context->TextureMgr)
                    m_context->TextureMgr->Reclaim(completedFence);
                if (m_context->GeometryResourceManager)
                    m_context->GeometryResourceManager->Reclaim(completedFence);
                if (m_context->SkeletonMgr)
                    m_context->SkeletonMgr->Reclaim(completedFence);

                // 每 60 帧清理池中长时间未使用的 GPU 资源
                if (m_context->FrameDriver && m_context->FrameDriver->GetFrameStats().frameNumber % 60 == 0) {
                    Resource::RenderTargetPool::GetInstance().PurgeUnused(
                        m_context->FrameDriver->GetFrameStats().frameNumber, 120);
                    Resource::DepthStencilPool::GetInstance().PurgeUnused(
                        m_context->FrameDriver->GetFrameStats().frameNumber, 120);
                }
            }
        }

        // 5. 检查窗口关闭标志
        if (m_context->Window->ShouldClose()) {
            m_isRunning = false;
        }
    }

    m_context->Logging->Info("[Editor] Editor main loop ended");
    return 0;
}

void Editor::Shutdown() {
    if (!m_isInitialized && !m_isRunning)
        return;

    m_isRunning = false;

    if (m_context) {
        m_context->FlushAllQueues();
    }

    // 关闭视口预览
    if (m_viewport) {
        m_viewport->Shutdown();
        m_viewport.reset();
    }

    // 关闭视口输入（旧系统，已注释）
    // if (m_viewportInput) {
    //    m_viewportInput->Shutdown();
    //    m_viewportInput.reset();
    //}

    // ── 将缩略图打包写入磁盘缓存（正常退出时保存，需在 layout 销毁前） ──
    if (m_thumbnailArray.IsInitialized() && m_previewCache.IsInitialized()) {
        const auto &thumbMap = m_assetManager.GetThumbnailMap();
        m_context->Logging->Info("[Editor] Saving {} thumbnails to disk cache...", thumbMap.size());

        // 收集所有有效缩略图数据
        struct PackedEntry {
            std::string filePath;
            std::vector<uint8_t> ddsData;
        };
        std::vector<PackedEntry> entries;
        for (const auto &[filePath, entry] : thumbMap) {
            if (entry.slice == UINT32_MAX || entry.gpuHandle.ptr == 0)
                continue;
            auto result = m_thumbnailArray.ReadbackSlice(entry.slice, m_context->DeviceContext);
            if (!result.pixels)
                continue;
            PackedEntry pe;
            pe.filePath = filePath;
            auto writeU32 = [](std::vector<uint8_t> &v, uint32_t val) {
                v.push_back(val & 0xFF);
                v.push_back((val >> 8) & 0xFF);
                v.push_back((val >> 16) & 0xFF);
                v.push_back((val >> 24) & 0xFF);
            };
            writeU32(pe.ddsData, 0x20534444); // "DDS "
            struct DDSHeader {
                uint32_t size = 124;
                uint32_t flags = 0x100F;
                uint32_t height;
                uint32_t width;
                uint32_t pitchOrLinearSize;
                uint32_t depth = 0;
                uint32_t mipMapCount = 1;
                uint8_t reserved1[44] = {};
                uint32_t caps = 0x1000;
                uint8_t reserved2[16] = {};
            };
            DDSHeader hdr;
            hdr.width = result.width;
            hdr.height = result.height;
            hdr.pitchOrLinearSize = result.width * 4;
            auto hdrBytes = reinterpret_cast<const uint8_t *>(&hdr);
            pe.ddsData.insert(pe.ddsData.end(), hdrBytes, hdrBytes + sizeof(hdr));
            auto pixelBytes = static_cast<const uint8_t *>(result.pixels);
            pe.ddsData.insert(pe.ddsData.end(), pixelBytes, pixelBytes + result.width * result.height * 4);
            PreviewCacheManager::FreeDDSData(result.pixels);
            entries.push_back(std::move(pe));
        }

        if (!entries.empty()) {
            std::string packPath = (std::filesystem::path(m_context->GetProjectConfig().Root) /
                                    "Content/Cache/Thumbnails/thumbnails.thumb")
                                       .string();
            std::filesystem::create_directories(std::filesystem::path(packPath).parent_path());
            std::ofstream file(packPath, std::ios::binary);
            if (file.is_open()) {
                uint32_t count = static_cast<uint32_t>(entries.size());
                file.write(reinterpret_cast<const char *>(&count), sizeof(count));
                for (auto &pe : entries) {
                    uint32_t keyLen = static_cast<uint32_t>(pe.filePath.size());
                    uint32_t ddsLen = static_cast<uint32_t>(pe.ddsData.size());
                    file.write(reinterpret_cast<const char *>(&keyLen), sizeof(keyLen));
                    file.write(pe.filePath.data(), keyLen);
                    file.write(reinterpret_cast<const char *>(&ddsLen), sizeof(ddsLen));
                    file.write(reinterpret_cast<const char *>(pe.ddsData.data()), ddsLen);
                }
                file.close();
                m_context->Logging->Info("[Editor] Thumbnail pack saved: {} ({} thumbnails)", packPath, count);
            }
        }
    }

    // 关闭面板（在 Layout 销毁前，确保 sink 等资源被清理）
    m_consolePanel.Shutdown();

    // 关闭编辑器布局
    if (m_layout) {
        m_layout->Shutdown();
        m_layout.reset();
    }

    SystemRegistry::Clear();

    // 关闭资产预览系统
    m_thumbnailArray.Shutdown();
    m_previewManager.Shutdown();
    m_scratchAllocator.Shutdown();

    // 关闭全局单例
    GridManager::GetInstance().Shutdown();

    // 保存当前场景的快照到磁盘
    m_editorSceneMgr.SaveCurrentSnapshotToDisk();

    m_isInitialized = false;
    m_context->Logging->Info("[Editor] Editor shutdown complete");
}