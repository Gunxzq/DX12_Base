#include "Game.h"
#include "Common/d3dUtil.h"
#include "Core/Context/GameContext.h"
#include "Renderer/Core/D3D12DeviceContext.h"
#include "Renderer/Modules/Camera/CameraManager.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "System/ECS/Components.h"
#include "System/ECS/Registry.h"
#include "System/Event/MessageDispatcher.h"
#include "System/Framework/SystemRegistry.h"
#include "System/Resource/GpuResourceManager.h"
#include "System/Scheduler/FrameDriver.h"
#include "System/Window/Window.h"
#include <DirectXMath.h>

using namespace DX12Engine;
using namespace DX12Engine::Scheduler;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Renderer;
using namespace DirectX;

Game::Game(Core::GameContext *context) : m_context(context), m_isRunning(false), m_isInitialized(false) {}

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

    System::Resource::GpuResourceManager::GetInstance().Initialize();

    InitializePassConstantBuffers();

    InitializeGameModules();

    // 初始化 OpaqueRenderer
    m_opaqueRenderer = std::make_unique<OpaqueRenderer>();
    m_opaqueRenderer->SetDeviceContext(m_context->DeviceContext);
    m_opaqueRenderer->Initialize();

    // 创建测试立方体
    CreateTestCube();

    if (m_context->FrameDriver) {
        m_context->FrameDriver->SetTargetFPS(60); // Limit to 60 FPS
        m_context->Logging->Info("[Game] Target FPS set to 60");
    }

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
        m_context->Window->ProcessMessages();
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

    if (m_context && m_context->CommandManager) {
        m_context->CommandManager->FlushAllQueues();
    }

    ShutdownGameModules();

    if (m_context && m_context->CommandManager) {
        System::Resource::GpuResourceManager::GetInstance().Update(m_context->CommandManager->GetCompletedFenceValue());
    }

    // 5. 最后关闭 GpuResourceManager
    System::Resource::GpuResourceManager::GetInstance().Shutdown();

    m_isInitialized = false;
    m_context->Logging->Info("[Game] Game shutdown complete");
}

void Game::Update(float deltaTime) {}

void Game::InitializeGameModules() {
    m_context->Logging->Info("[Game] Initializing game modules...");

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
                uint32_t frameIndex = m_context->DeviceContext->GetCurrentBackBufferIndex();

                // 3. 构建 PassConstants
                PassConstants passData;
                const auto &camera = m_context->CameraMgr->GetMainCamera();

                XMStoreFloat4x4(&passData.View, camera.ViewMatrix);
                XMStoreFloat4x4(&passData.Proj, camera.ProjMatrix);
                XMStoreFloat4x4(&passData.ViewProj, camera.ViewProjMatrix);

                passData.CameraPos = camera.Position;
                passData.TotalTime = static_cast<float>(m_context->MainTimer->GetGameTime());

                static bool firstFrame = true;
                if (firstFrame) {
                    wchar_t buf[512];
                    swprintf_s(buf,
                               L"[DEBUG] Camera Proj Matrix:\n"
                               L"  [0]: %.4f, %.4f, %.4f, %.4f\n"
                               L"  [1]: %.4f, %.4f, %.4f, %.4f\n"
                               L"  [2]: %.4f, %.4f, %.4f, %.4f\n"
                               L"  [3]: %.4f, %.4f, %.4f, %.4f\n"
                               L"  Aspect: %.4f\n",
                               passData.Proj._11, passData.Proj._12, passData.Proj._13, passData.Proj._14,
                               passData.Proj._21, passData.Proj._22, passData.Proj._23, passData.Proj._24,
                               passData.Proj._31, passData.Proj._32, passData.Proj._33, passData.Proj._34,
                               passData.Proj._41, passData.Proj._42, passData.Proj._43, passData.Proj._44,
                               camera.AspectRatio);
                    OutputDebugStringW(buf);
                    firstFrame = false;
                }

                // 4. 上传到 GPU (Memcpy 到映射内存)
                // 由于是在 Immediate 路径且主线程串行执行，这里没有竞态条件
                memcpy(m_passCBResources[frameIndex].mappedData, &passData, sizeof(PassConstants));
            },
            "CameraUpdate");
    }

    SystemRegistry::Register(
        {.name = "CameraControlSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 if (!m_context || !m_context->CameraMgr)
                     return;

                 auto &camera = m_context->CameraMgr->GetMainCamera();
                 float deltaTime = m_context->MainTimer->GetDeltaTime();

                 // 定义旋转速度 (弧度/秒)
                 const float rotateSpeed = 1.5f;

                 // 从消息上下文中获取事件数据
                 // 注意：如果 interestedMessages 设置了，ctx 将包含该消息的数据
                 // 如果总是运行 (alwaysRun=true) 且没有特定消息，ctx 可能为空或包含最后一条消息
                 // 为了确保每帧都能响应持续按键，我们结合 GetAsyncKeyState (最稳定)
                 // 或者，如果我们要纯事件驱动，我们需要处理 WM_KEYDOWN 的重复发送。

                 // 这里我们使用 GetAsyncKeyState 以确保平滑，这是 Win32 游戏的标准做法。
                 // 事件系统可以用于其他逻辑（如 UI 交互）。

                 bool upPressed = (GetAsyncKeyState(VK_UP) & 0x8000) != 0;
                 bool downPressed = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
                 bool leftPressed = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
                 bool rightPressed = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;

                 // 调整俯仰角 (Pitch) - 绕 X 轴旋转
                 if (upPressed) {
                     camera.Rotation.x += rotateSpeed * deltaTime;
                 }
                 if (downPressed) {
                     camera.Rotation.x -= rotateSpeed * deltaTime;
                 }

                 // 调整偏航角 (Yaw) - 绕 Y 轴旋转
                 if (leftPressed) {
                     camera.Rotation.y -= rotateSpeed * deltaTime;
                 }
                 if (rightPressed) {
                     camera.Rotation.y += rotateSpeed * deltaTime;
                 }

                 // 限制俯仰角范围 (-90 到 +90 度)
                 const float maxPitch = DirectX::XM_PI / 2.0f - 0.01f;
                 if (camera.Rotation.x > maxPitch)
                     camera.Rotation.x = maxPitch;
                 if (camera.Rotation.x < -maxPitch)
                     camera.Rotation.x = -maxPitch;
             },
         .phase = TaskPhase::Update,
         .threadType = ThreadType::Main,
         .priority = TaskPriority::High,
         .dependencies = {},
         .interestedMessages = {System::Event::KeyboardInputEvent::StaticTypeHash}, // 监听键盘事件
         .alwaysRun = true}); // 即使没有新事件，也每帧运行以处理持续按键状态（如果需要结合 GetAsyncKeyState）

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
                              .interestedMessages = {System::Event::WindowResizeEvent::StaticTypeHash}});

    SystemRegistry::Register(
        {.name = "MainRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 OutputDebugStringW(L"[DEBUG] MainRenderSystem executed.\n");

                 auto &cmdMgr = m_context->CommandManager;
                 uint64_t completedFence = cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);

                 auto allocatorHandle = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 auto backBufferIndex = m_context->DeviceContext->GetCurrentBackBufferIndex();
                 auto backBuffer = m_context->DeviceContext->GetCurrentBackBuffer();

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

                 OutputDebugStringW(L"[DEBUG] Entities found: ");
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

                 uint64_t sequence = cmdMgr->GetNextSequence();
                 // 释放分配器（传入 sequence，不是立即释放）
                 cmdMgr->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
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
    auto &gpuMgr = System::Resource::GpuResourceManager::GetInstance();
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
    uint32_t frameIndex = m_context->DeviceContext->GetCurrentBackBufferIndex();
    return m_passCBResources[frameIndex].resource->GetGPUVirtualAddress();
}