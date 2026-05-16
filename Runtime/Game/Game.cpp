#include "Game.h"
#include "Common/d3dUtil.h"
#include "Core/Context/GameContext.h"
#include "Renderer/Core/D3D12DeviceContext.h"
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

    InitializeGameModules();

    // 初始化 OpaqueRenderer
    m_opaqueRenderer = std::make_unique<OpaqueRenderer>();
    m_opaqueRenderer->SetDeviceContext(m_context->DeviceContext);
    m_opaqueRenderer->Initialize();

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
    m_context->MainTimer->Reset();
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

void Game::Update(float deltaTime) {
    // 此方法保留给纯逻辑更新（如物理、动画等）
    if (m_context->Registry && entt::null != m_cubeEntity) {
        auto *transform = m_context->Registry->TryGetComponent<TransformComponent>(m_cubeEntity);
        if (transform) {
            transform->rotation.y += deltaTime * 0.5f; // 每秒旋转 0.5 弧度
        }
    }
}

void Game::InitializeGameModules() {
    m_context->Logging->Info("[Game] Initializing game modules...");

    // ─────────────────────────────────────────────────
    // L4: 注册消息驱动的 Systems（利用调度层能力）
    // ─────────────────────────────────────────────────

    // WindowResizeSystem - 处理窗口大小变化 (主线程执行)
    SystemRegistry::Register(
        {.name = "WindowResizeSystem",
         .func =
             [this](Registry &, const MessageContext &ctx) {
                 // 首先输出到调试器
                 char dbgBuf[256];
                 sprintf_s(dbgBuf, "[WindowResizeSystem] Executed! Width=%u Height=%u Payload=0x%llX\n", ctx.GetLow32(),
                           ctx.GetHigh32(), (unsigned long long)ctx.payload);
                 ::OutputDebugStringA(dbgBuf);

                 // 尝试 spdlog 输出
                 if (m_context && m_context->Logging) {
                     m_context->Logging->Info("[WindowResizeSystem] spdlog: {}x{}", ctx.GetLow32(), ctx.GetHigh32());
                 } else {
                     ::OutputDebugStringA("[WindowResizeSystem] WARNING: m_context or Logging is null!\n");
                 }

                 uint32_t width = ctx.GetLow32();
                 uint32_t height = ctx.GetHigh32();

                 // DX12 resize
                 if (m_context && m_context->DeviceContext) {
                     m_context->DeviceContext->OnResize(ctx.GetLow32(), ctx.GetHigh32());
                 }

                 if (m_opaqueRenderer) {
                     m_opaqueRenderer->OnResize(width, height);

                     sprintf_s(dbgBuf, "[WindowResizeSystem] OpaqueRenderer projection matrix updated\n");
                     ::OutputDebugStringA(dbgBuf);
                 }
             },
         .phase = TaskPhase::EarlyUpdate,
         .threadType = ThreadType::Main, // 主线程执行
         .interestedMessages = {System::Event::WindowResizeEvent::StaticTypeHash}});

    SystemRegistry::Register(
        {.name = "MainRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
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
                 m_opaqueRenderer->BeginFrame(cmdList, backBufferIndex);
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
    SystemRegistry::Register(
        {.name = "RotationSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 // 使用 GameContext 中的定时器计算 delta time
                 float deltaTime = m_context->MainTimer->DeltaTime();

                 // 限制最大 delta time 避免跳帧时旋转过快
                 if (deltaTime > 0.1f) {
                     deltaTime = 0.016f; // 假设 60 FPS
                 }

                 // 调试输出：检查是否有实体
                 static int frameCount = 0;
                 frameCount++;

                 auto view = registry.view<TransformComponent>();
                 int entityCount = 0;

                 view.each([deltaTime, &entityCount](TransformComponent &transform) {
                     entityCount++;
                     transform.rotation.y += deltaTime * 2.0f; // 每秒旋转 2 弧度

                     // 每30帧输出一次调试信息
                     if (frameCount % 30 == 0 && entityCount == 1) {
                         char dbgBuf[256];
                         sprintf_s(dbgBuf, "[RotationSystem] Frame=%d, DeltaTime=%.4f, Rotation=(%.2f, %.2f, %.2f)\n",
                                   frameCount, deltaTime, transform.rotation.x, transform.rotation.y,
                                   transform.rotation.z);
                         ::OutputDebugStringA(dbgBuf);
                     }
                 });

                 // 如果没有找到实体，输出警告
                 if (entityCount == 0 && frameCount <= 10) {
                     OutputDebugStringA("[WARNING] RotationSystem: No entities with TransformComponent found!\n");
                 }
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
