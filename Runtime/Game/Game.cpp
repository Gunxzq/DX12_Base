#include "Game.h"
#include "./Input/GameInputActions.h"
#include "Boot/GameContext.h"
#include "Common/d3dUtil.h"
#include "ECS/Core/Components.h"
#include "ECS/Core/Registry.h"
#include "Event/MessageDispatcher.h"
#include "Framework/SystemRegistry.h"
#include "Platform/Input/InputSystem.h"
#include "Platform/Windows/Window.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/CameraManager.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/GpuResourceManager.h"
#include "Scheduler/FrameDriver.h"
#include <DirectXMath.h>

using namespace DX12Engine;
using namespace DX12Engine::DebugUI;
using namespace DX12Engine::Scheduler;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Input;
using namespace DirectX;

Game::Game(Boot::GameContext *context) : m_context(context), m_isRunning(false), m_isInitialized(false) {}

Game::~Game() {
    if (m_isRunning || m_isInitialized) {
        Shutdown();
    }
}

bool Game::Initialize() {
    if (!m_context || !m_context->IsValid()) {
        m_context->Logging->Error("[Game] Failed to initialize: %s",
                                  m_context ? m_context->GetInvalidReason() : "Context is null");
        return false;
    }

    m_context->Logging->Info("[Game] Initializing game...");

    Resource::GpuResourceManager::GetInstance().Initialize();

    InitializePassConstantBuffers();

    InitializeGameModules();

    // 初始化 OpaqueRenderer
    m_opaqueRenderer = std::make_unique<OpaqueRenderer>();
    m_opaqueRenderer->SetDeviceContext(m_context->DeviceContext);
    m_opaqueRenderer->Initialize();

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
                     dispatcher->PostEvent(Event::FullscreenToggleEvent::StaticTypeHash,
                                           0,                    // senderId
                                           fullscreen ? 1u : 0u, // payload: 1 = 全屏, 0 = 窗口
                                           Event::EventPriority::P1_High);
                 }
             }

             // 显示各种时间值
             ImGui::Text("Game Delta Time: %.3f ms", dt * 1000.0f);
             ImGui::Text("Raw Delta Time: %.3f ms", m_context->MainTimer->GetRawDeltaTime() * 1000.0f);

             // 计算理论 FPS
             float rawFPS = 1.0f / m_context->MainTimer->GetRawDeltaTime();
             ImGui::Text("Raw FPS (unlimited): %.1f", rawFPS);

             // 显示限制来源
             ImGui::Text("FrameDriver Target: %d FPS", m_context->FrameDriver->GetTargetFPS());

             // 如果原始帧率 > 60 但游戏帧率 = 60，说明是 DWM 限制
             if (rawFPS > 62.0f && dt * 1000.0f > 15.5f) {
                 ImGui::TextColored(ImVec4(1, 1, 0, 1), "⚠️ Limited by DWM (windowed mode)");
             }
         }});
    // 创建测试立方体
    CreateTestCube();

    m_isInitialized = true;
    m_context->Logging->Info("[Game] Game initialized successfully");
    return true;
}

int Game::Run() {
    if (!m_isInitialized) {
        m_context->Logging->Error("[Game] Cannot run: game is not initialized");
        return -1;
    }

    if (!m_context->FrameDriver) {
        m_context->Logging->Error("[Game] Cannot run: FrameDriver is not set");
        return -1;
    }

    m_context->Logging->Info("[Game] Starting game loop with FrameDriver...");
    m_isRunning = true;
    m_context->Window->Show();

    uint32_t frameCount = 0;
    const uint32_t maxFrames = 300;

    // 初始化 FrameDriver
    m_context->FrameDriver->Initialize();

    OutputDebugStringW(L"[DEBUG] Entering main loop\n");

    while (m_isRunning && !m_context->Window->ShouldClose()) {
        m_context->MainTimer->Tick();
        // 调用 FrameDriver::Tick() 来处理消息和执行注册的 Systems
        m_context->FrameDriver->Tick();
    }

    // 停止 FrameDriver
    m_context->FrameDriver->Stop();

    m_context->Logging->Info("[Game] Game loop ended");
    Shutdown();
    return 0;
}

void Game::Shutdown() {
    if (!m_isInitialized && !m_isRunning) {
        return;
    }

    m_isRunning = false;

    if (m_context) {
        m_context->FlushAllQueues();
    }

    ShutdownGameModules();

    if (m_context) {
        Resource::GpuResourceManager::GetInstance().Update(m_context->GetFenceValue());
    }

    // 5. 最后关闭 GpuResourceManager
    Resource::GpuResourceManager::GetInstance().Shutdown();

    m_isInitialized = false;
    m_context->Logging->Info("[Game] Game shutdown complete");
}

void Game::Update(float deltaTime) {}

void Game::InitializeGameModules() {
    m_context->Logging->Info("[Game] Initializing game modules...");

    if (!m_context->InputSys) {
        m_context->InputSys = &Input::InputSystem::Get();

        m_context->InputSys->Initialize("Config/default_input.json");
    }

    if (m_context->CameraMgr) {
        auto &mainCamera = m_context->CameraMgr->GetMainCamera();
        mainCamera.Position = DirectX::XMFLOAT3(0.0f, 0.0f, -10.0f);
        mainCamera.Rotation = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f); // 确保看向 +Z 方向

        // 强制立即更新一次矩阵，确保第一帧渲染时数据正确
        m_context->CameraMgr->UpdateMainCamera();

        m_context->Logging->Info("[Game] Main Camera set to Pos(0,0,-10) looking at +Z (Origin)");
    }

    if (m_context->FrameDriver) {
        m_context->FrameDriver->RegisterImmediateCallback(
            [this]() {
                // 1. 更新相机管理器（计算 View/Proj 矩阵）
                m_context->CameraMgr->UpdateMainCamera();

                // 2. 获取当前帧索引
                uint32_t frameIndex = m_context->GetBackBufferIndex();

                // 3. 构建 PassConstants
                PassConstants passData;
                const auto &camera = m_context->CameraMgr->GetMainCamera();

                XMStoreFloat4x4(&passData.View, camera.ViewMatrix);
                XMStoreFloat4x4(&passData.Proj, camera.ProjMatrix);
                XMStoreFloat4x4(&passData.ViewProj, camera.ViewProjMatrix);

                passData.CameraPos = camera.Position;
                passData.TotalTime = static_cast<float>(m_context->MainTimer->GetGameTime());

                // 4. 上传到 GPU (Memcpy 到映射内存)
                // 由于是在 Immediate 路径且主线程串行执行，这里没有竞态条件
                memcpy(m_passCBResources[frameIndex].mappedData, &passData, sizeof(PassConstants));
            },
            "CameraUpdate");
    }

    // WindowResizeSystem - 增加相机宽高比同步
    SystemRegistry::Register({.name = "WindowResizeSystem",
                              .func =
                                  [this](Registry &, const MessageContext &ctx) {
                                      uint32_t width = ctx.GetLow32();
                                      uint32_t height = ctx.GetHigh32();

                                      // 1. DX12 后端 resize
                                      if (m_context && m_context->DeviceContext) {
                                          m_context->DeviceContext->OnResize(width, height);
                                      }

                                      // 2. 相机管理器 resize (重新计算所有相机的投影矩阵)
                                      if (m_context && m_context->CameraMgr) {
                                          m_context->CameraMgr->OnResize(width, height);
                                      }

                                      // 3. 渲染模块 resize
                                      if (m_opaqueRenderer) {
                                          m_opaqueRenderer->OnResize(width, height);
                                      }
                                  },
                              .phase = TaskPhase::EarlyUpdate,
                              .threadType = ThreadType::Main,
                              .interestedMessages = {Event::WindowResizeEvent::StaticTypeHash}});

    SystemRegistry::Register(
        {.name = "MainRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);

                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 auto backBufferIndex = m_context->GetBackBufferIndex();
                 auto backBuffer = m_context->GetBackBuffer();

                 // 1. 屏障：Present -> RenderTarget
                 D3D12_RESOURCE_BARRIER beginBarrier = {};
                 beginBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 beginBarrier.Transition.pResource = backBuffer;
                 beginBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                 beginBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 beginBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &beginBarrier);

                 // 2. 设置视口和渲染目标
                 const auto &viewport = m_context->DeviceContext->GetViewport();
                 const auto &scissorRect = m_context->DeviceContext->GetScissorRect();
                 cmdList.Get()->RSSetViewports(1, &viewport);
                 cmdList.Get()->RSSetScissorRects(1, &scissorRect);

                 auto rtvHandle = m_context->DeviceContext->GetCurrentBackBufferView();
                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 // 3. 清除
                 const float clearColor[] = {0.0f, 0.2f, 0.4f, 1.0f};
                 cmdList.Get()->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
                 cmdList.Get()->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                                      1.0f, 0, 0, nullptr);

                 // 4. 绘制
                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = GetCurrentPassCBAddress();
                 m_opaqueRenderer->BeginFrame(cmdList, backBufferIndex, passCBAddr);

                 auto view = registry.view<MeshComponent, TransformComponent>();

                 for (const auto &[entity, mesh, transform] : view.each()) {
                     m_opaqueRenderer->DrawMesh(cmdList, mesh, transform, backBufferIndex);
                 }
                 m_opaqueRenderer->EndFrame();

                 // 5. 屏障：RenderTarget -> Present
                 D3D12_RESOURCE_BARRIER endBarrier = {};
                 endBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 endBarrier.Transition.pResource = backBuffer;
                 endBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 endBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 endBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &endBarrier);

                 // 6. 关闭并提交
                 cmdList.Close();

                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Opaque, cmdListHandle);

                 uint64_t sequence = m_context->GetNextSequence();
                 // 释放分配器（传入 sequence，不是立即释放）
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Main,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Opaque,
         .dependencies = {},
         .interestedMessages = {},
         .alwaysRun = true});

    // RotationSystem - 每帧旋转立方体 (常驻系统)
    SystemRegistry::Register({.name = "RotationSystem",
                              .func =
                                  [this](Registry &registry, const MessageContext &ctx) {
                                      // 使用 GameContext 中的定时器计算 delta time
                                      float deltaTime = m_context->MainTimer->GetDeltaTime();

                                      auto view = registry.view<TransformComponent>();
                                      int entityCount = 0;

                                      view.each([deltaTime, &entityCount](TransformComponent &transform) {
                                          entityCount++;
                                          transform.rotation.y += deltaTime * 2.0f; // 每秒旋转 2 弧度
                                      });
                                  },
                              .phase = TaskPhase::Update,
                              .threadType = ThreadType::Main,
                              .priority = TaskPriority::Normal,
                              .dependencies = {},
                              .interestedMessages = {},
                              .alwaysRun = true});

    SystemRegistry::Register({
        .name = "CameraControlSystem",
        .func = [this](Registry &registry, const MessageContext &ctx) { this->HandleCameraInput(); },
        .phase = Scheduler::TaskPhase::EarlyUpdate, // 在渲染前更新相机位置
        .threadType = Scheduler::ThreadType::Main,
        .priority = Scheduler::TaskPriority::High,
        .dependencies = {},
        .interestedMessages = {},
        .alwaysRun = true // 常驻任务
    });

    // 在 WindowResizeSystem 注册代码之后添加

    SystemRegistry::Register({
        .name = "FullscreenSystem",
        .func =
            [this](Registry &, const MessageContext &ctx) {
                bool bRequestFullscreen = ctx.GetLow32() != 0;
                // 调用 Window 的全屏切换方法（你已经实现好的）
                m_context->Window->SetFullscreen(bRequestFullscreen);
                // 切换后，窗口大小改变会触发 WM_SIZE → PostWindowResizeEvent
                // 从而自动调用 WindowResizeSystem 重建交换链和相机
            },
        .phase = TaskPhase::EarlyUpdate, // 安全时机
        .threadType = ThreadType::Main,
        .priority = TaskPriority::High,
        .interestedMessages = {Event::FullscreenToggleEvent::StaticTypeHash},
    });

    // 验证注册
    auto allSystems = SystemRegistry::GetAllSystems();
    wchar_t buf[128];
    swprintf_s(buf, L"[Game] Total systems registered: %zu", allSystems.size());
    ::OutputDebugStringW(buf);

    m_context->Logging->Info("[Game] Game modules initialized");
}

void Game::ShutdownGameModules() {
    m_context->Logging->Info("[Game] Shutting down game modules...");

    SystemRegistry::Clear();

    m_context->Logging->Info("[Game] Game modules shutdown complete");
}

void Game::CreateTestCube() {

    if (!m_context || !m_context->Registry) {
        return;
    }

    m_context->Logging->Info("[Game] Creating test cube...");

    // 1. 创建立方体几何数据
    GeometryGenerator geoGen;
    auto meshData = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 0);

    m_context->Logging->Info("[Game] Generated box geometry: {} vertices, {} indices", meshData.Vertices.size(),
                             meshData.Indices32.size());

    // 2. 创建简化的顶点缓冲区（只包含 Position 和 Color）
    struct SimpleVertex {
        DirectX::XMFLOAT3 Position;
        DirectX::XMFLOAT4 Color;
    };

    std::vector<SimpleVertex> simpleVertices(meshData.Vertices.size());
    for (size_t i = 0; i < meshData.Vertices.size(); ++i) {
        simpleVertices[i].Position = meshData.Vertices[i].Position;
        // 为每个面设置不同的颜色以便调试
        if (i < 4)
            simpleVertices[i].Color = XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f); // 红色 - 前面
        else if (i < 8)
            simpleVertices[i].Color = XMFLOAT4(0.0f, 1.0f, 0.0f, 1.0f); // 绿色 - 后面
        else if (i < 12)
            simpleVertices[i].Color = XMFLOAT4(0.0f, 0.0f, 1.0f, 1.0f); // 蓝色 - 上面
        else if (i < 16)
            simpleVertices[i].Color = XMFLOAT4(1.0f, 1.0f, 0.0f, 1.0f); // 黄色 - 下面
        else if (i < 20)
            simpleVertices[i].Color = XMFLOAT4(1.0f, 0.0f, 1.0f, 1.0f); // 紫色 - 左面
        else
            simpleVertices[i].Color = XMFLOAT4(0.0f, 1.0f, 1.0f, 1.0f); // 青色 - 右面
    }

    // 3. 创建 GPU 资源
    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
    auto device = m_context->DeviceContext->GetDevice();

    // 创建顶点缓冲
    size_t vbSize = simpleVertices.size() * sizeof(SimpleVertex);
    auto vbHandle = gpuMgr.CreateBuffer(device, vbSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *vbResource = gpuMgr.GetResource(vbHandle);

    if (!vbResource) {
        m_context->Logging->Error("[Game] Failed to create vertex buffer!");
        return;
    }

    // 映射并复制顶点数据
    void *vbMapped = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    vbResource->Map(0, &readRange, &vbMapped);
    memcpy(vbMapped, simpleVertices.data(), vbSize);
    vbResource->Unmap(0, nullptr);

    // 创建索引缓冲
    size_t ibSize = meshData.Indices32.size() * sizeof(uint32_t);
    auto ibHandle = gpuMgr.CreateBuffer(device, ibSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ibResource = gpuMgr.GetResource(ibHandle);

    if (!ibResource) {
        m_context->Logging->Error("[Game] Failed to create index buffer!");
        return;
    }

    // 映射并复制索引数据
    void *ibMapped = nullptr;
    ibResource->Map(0, &readRange, &ibMapped);
    memcpy(ibMapped, meshData.Indices32.data(), ibSize);
    ibResource->Unmap(0, nullptr);

    // 4. 创建 Entity 并添加组件
    m_cubeEntity = m_context->Registry->CreateEntity();

    // 添加 TransformComponent（放在原点前方 5 单位）
    XMFLOAT3 position(0.0f, 0.0f, 5.0f);
    XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
    XMFLOAT3 scale(1.0f, 1.0f, 1.0f);

    m_context->Registry->AddComponent<TransformComponent>(m_cubeEntity, position, rotation, scale);

    m_context->Logging->Info("[Game] Created entity at position ({}, {}, {})", position.x, position.y, position.z);

    // 添加 MeshComponent
    MeshComponent meshComp;
    meshComp.vertexBuffer = vbHandle;
    meshComp.indexBuffer = ibHandle;
    meshComp.vertexCount = static_cast<uint32_t>(simpleVertices.size());
    meshComp.indexCount = static_cast<uint32_t>(meshData.Indices32.size());

    // 设置 VBV 和 IBV
    meshComp.vertexBufferView.BufferLocation = vbResource->GetGPUVirtualAddress();
    meshComp.vertexBufferView.StrideInBytes = sizeof(SimpleVertex);
    meshComp.vertexBufferView.SizeInBytes = static_cast<UINT>(vbSize);

    meshComp.indexBufferView.BufferLocation = ibResource->GetGPUVirtualAddress();
    meshComp.indexBufferView.Format = DXGI_FORMAT_R32_UINT;
    meshComp.indexBufferView.SizeInBytes = static_cast<UINT>(ibSize);

    m_context->Registry->AddComponent<MeshComponent>(m_cubeEntity, std::move(meshComp));

    m_context->Logging->Info("[Game] Test cube created with {} vertices and {} indices", meshComp.vertexCount,
                             meshComp.indexCount);

    m_context->Logging->Info("[Game] VB Handle: {}, IB Handle: {}", vbHandle.index, ibHandle.index);
    m_context->Logging->Info("[Game] Vertex stride: {}", sizeof(SimpleVertex));
    m_context->Logging->Info("[Game] VB Valid: {}, IB Valid: {}", vbHandle.IsValid() ? "true" : "false",
                             ibHandle.IsValid() ? "true" : "false");

    // 验证 ECS 查询
    auto view = m_context->Registry->view<MeshComponent, TransformComponent>();
    bool hasEntities = false;
    view.each([&hasEntities](const MeshComponent &, const TransformComponent &) { hasEntities = true; });

    if (!hasEntities) {
        OutputDebugStringA("[WARNING] MainRenderSystem: No entities found!\n");
    } else {
        OutputDebugStringA("[INFO] MainRenderSystem: Rendering...\n");
    }
}

void Game::InitializePassConstantBuffers() {
    auto device = m_context->DeviceContext->GetDevice();
    UINT cbSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

    for (uint32_t i = 0; i < 3; ++i) {
        ThrowIfFailed(d3dUtil::CreateUploadBuffer(device, cbSize, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  &m_passCBResources[i].resource));
        m_passCBResources[i].resource->Map(0, nullptr, &m_passCBResources[i].mappedData);
    }
}

D3D12_GPU_VIRTUAL_ADDRESS Game::GetCurrentPassCBAddress() const {
    uint32_t frameIndex = m_context->GetBackBufferIndex();
    return m_passCBResources[frameIndex].resource->GetGPUVirtualAddress();
}

void Game::HandleCameraInput() {
    if (!m_context || !m_context->InputSys || !m_context->CameraMgr || !m_context->Window) {
        return;
    }

    auto &inputSys = *m_context->InputSys;
    auto &cameraMgr = *m_context->CameraMgr;
    auto &mainCamera = cameraMgr.GetMainCamera();
    auto &window = *m_context->Window;

    float deltaTime = m_context->MainTimer->GetDeltaTime();
    if (deltaTime <= 0.0f)
        return;

    // =========================================================================
    // 0. 调试：重置相机 (按 R 键)
    // =========================================================================
    if (inputSys.IsActionPressed(ActionId_ResetCamera)) {
        mainCamera.Position = DirectX::XMFLOAT3(0.0f, 2.0f, -10.0f);
        mainCamera.Rotation = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        m_context->Logging->Info("[Debug] Camera Reset!");
    }

    // =========================================================================
    // 1. 鼠标捕获控制 (Cursor Capture)
    // =========================================================================

    bool wasCaptured = window.IsCursorCaptured();

    // 策略：按 Pause (Esc) 切换捕获状态
    // 如果当前未捕获且按下了 Pause，则捕获
    // 如果当前已捕获且按下了 Pause，则释放
    if (inputSys.IsActionPressed(ActionId_Pause)) {
        bool newCaptureState = !wasCaptured;
        window.SetCursorCapture(newCaptureState);

        if (newCaptureState) {
            m_skipLookInputThisFrame = true;
            m_context->Logging->Info("[Input] Cursor Captured. Skipping first frame look input.");
        }
    }

    // 额外保护：如果窗口失去焦点，强制释放捕获（已在 Window.cpp WM_ACTIVATE 中处理，但这里双重保险）
    // 注意：IsCursorCaptured 是本地状态，如果窗口失焦，Window.cpp 会调用 SetCursorCapture(false) 同步状态

    // =========================================================================
    // 2. 相机旋转 (Look) - 仅当鼠标被捕获时生效
    // =========================================================================
    if (m_skipLookInputThisFrame) {
        m_skipLookInputThisFrame = false;
        return;
    }

    if (window.IsCursorCaptured()) {

        // 获取鼠标相对移动量 (Delta)
        // 确保 InputSystem 将 Mouse Delta 映射到了 ActionId_Look
        FVector2D lookInput = inputSys.GetActionAxis2D(ActionId_Look);

        // 灵敏度设置 (可根据需要调整或从配置读取)
        const float mouseSensitivity = 0.002f;

        // 更新偏航角 (Yaw) 和 俯仰角 (Pitch)
        // lookInput.X -> Yaw (左右旋转)
        // lookInput.Y -> Pitch (上下旋转)
        mainCamera.Rotation.y += lookInput.X * mouseSensitivity;
        mainCamera.Rotation.x += lookInput.Y * mouseSensitivity;

        // 限制俯仰角 (Pitch Clamp)，防止万向节死锁或翻转
        // 限制在 -89度 到 +89度 之间
        const float maxPitch = DirectX::XM_PI / 2.0f - 0.01f;
        mainCamera.Rotation.x = std::clamp(mainCamera.Rotation.x, -maxPitch, maxPitch);

        // 可选：归一化 Yaw 到 [0, 2PI) 防止浮点数过大
        if (mainCamera.Rotation.y > DirectX::XM_2PI)
            mainCamera.Rotation.y -= DirectX::XM_2PI;
        if (mainCamera.Rotation.y < 0.0f)
            mainCamera.Rotation.y += DirectX::XM_2PI;
    }

    // =========================================================================
    // 3. 相机移动 (Move) - WASD
    // =========================================================================

    // 获取移动输入 (-1.0 到 1.0)
    FVector2D moveInput = inputSys.GetActionAxis2D(ActionId_Move);

    // 检查是否冲刺 (Sprint)
    bool isSprinting = inputSys.IsActionHeld(ActionId_Sprint);

    // 基础移动速度
    float baseMoveSpeed = 5.0f;
    // 冲刺速度倍数
    float sprintMultiplier = 2.0f;

    float currentSpeed = baseMoveSpeed * (isSprinting ? sprintMultiplier : 1.0f);

    // 只有当有输入时才计算移动
    if (std::abs(moveInput.X) > 0.001f || std::abs(moveInput.Y) > 0.001f) {
        float yaw = mainCamera.Rotation.y;

        // 计算水平面上的 Forward 和 Right 向量
        // Forward: 指向 Yaw 方向
        DirectX::XMFLOAT3 forwardDir;
        forwardDir.x = sin(yaw);
        forwardDir.y = 0.0f;
        forwardDir.z = cos(yaw);

        // Right: 指向 Yaw + 90度 方向
        DirectX::XMFLOAT3 rightDir;
        rightDir.x = cos(yaw);
        rightDir.y = 0.0f;
        rightDir.z = -sin(yaw);

        // 应用移动
        // moveInput.Y 对应前后 (W/S), moveInput.X 对应左右 (A/D)
        DirectX::XMVECTOR pos = DirectX::XMLoadFloat3(&mainCamera.Position);
        DirectX::XMVECTOR fwd = DirectX::XMLoadFloat3(&forwardDir);
        DirectX::XMVECTOR rgt = DirectX::XMLoadFloat3(&rightDir);

        // 累加位移
        pos += fwd * (moveInput.Y * currentSpeed * deltaTime);
        pos += rgt * (moveInput.X * currentSpeed * deltaTime);

        DirectX::XMStoreFloat3(&mainCamera.Position, pos);
    }
}