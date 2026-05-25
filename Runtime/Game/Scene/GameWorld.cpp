#include "GameWorld.h"
#include "Boot/GameContext.h"
#include "Common/ThrowHelper.h"
#include "Common/d3dUtil.h"
#include "ECS/Core/Components.h"
#include "ECS/Core/Registry.h"
#include "Framework/SystemRegistry.h"
#include "Renderer/Pipeline/OpaqueRenderer.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/GpuResourceManager.h"
#include "Scheduler/FrameDriver.h"
#include <DirectXMath.h>
#include <wrl/client.h>

using namespace DirectX;
using namespace Microsoft::WRL;
using namespace DX12Engine::Boot;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::Scheduler;

GameWorld::GameWorld() = default;
GameWorld::~GameWorld() = default;

void GameWorld::Initialize(GameContext *context, OpaqueRenderer *renderer) {
    m_context = context;
    m_registry = context->Registry;
    m_renderer = renderer;

    // 注册游戏世界相关的系统
    RegisterSystems();
}

void GameWorld::RegisterSystems() {
    if (!m_context)
        return;

    // ========================================================================
    // CubeRotationSystem - 旋转立方体
    // ========================================================================
    SystemRegistry::Register(
        {.name = "CubeRotationSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 float deltaTime = m_context->MainTimer->GetDeltaTime();
                 auto view = registry.view<TransformComponent>();
                 view.each([deltaTime](TransformComponent &transform) { transform.rotation.y += deltaTime * 2.0f; });
             },
         .phase = TaskPhase::Update,
         .threadType = ThreadType::Main,
         .priority = TaskPriority::Normal,
         .alwaysRun = true});

    // ========================================================================
    // CubeRenderSystem - 渲染立方体（原 MainRenderSystem）
    // 注意：这个系统现在在 GameWorld 中注册，而不是在引擎层
    // ========================================================================
    SystemRegistry::Register(
        {.name = "CubeRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 // 获取命令列表等渲染资源
                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 auto backBufferIndex = m_context->GetBackBufferIndex();
                 auto backBuffer = m_context->GetBackBuffer();

                 // 屏障：Present -> RenderTarget
                 D3D12_RESOURCE_BARRIER beginBarrier = {};
                 beginBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 beginBarrier.Transition.pResource = backBuffer;
                 beginBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                 beginBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 beginBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &beginBarrier);

                 // 设置视口和渲染目标
                 const auto &viewport = m_context->DeviceContext->GetViewport();
                 const auto &scissorRect = m_context->DeviceContext->GetScissorRect();
                 cmdList.Get()->RSSetViewports(1, &viewport);
                 cmdList.Get()->RSSetScissorRects(1, &scissorRect);

                 auto rtvHandle = m_context->DeviceContext->GetCurrentBackBufferView();
                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 // 清除
                 const float clearColor[] = {0.0f, 0.2f, 0.4f, 1.0f};
                 cmdList.Get()->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
                 cmdList.Get()->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                                      1.0f, 0, 0, nullptr);

                 // 获取 Pass Constant Buffer 地址
                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();

                 // 开始渲染
                 m_renderer->BeginFrame(cmdList, passCBAddr);

                 // 遍历所有带 MeshComponent 和 TransformComponent 的实体并渲染
                 auto view_entities = registry.view<MeshComponent, TransformComponent>();
                 for (const auto &[entity, mesh, transform] : view_entities.each()) {
                     m_renderer->DrawMesh(cmdList, mesh, transform);
                 }
                 m_renderer->EndFrame();

                 // 屏障：RenderTarget -> Present
                 D3D12_RESOURCE_BARRIER endBarrier = {};
                 endBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 endBarrier.Transition.pResource = backBuffer;
                 endBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 endBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 endBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &endBarrier);

                 // 关闭并提交
                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Opaque, cmdListHandle);

                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Main,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Opaque,
         .alwaysRun = true});
}

void GameWorld::Clear() {
    if (!m_registry)
        return;

    // 移除测试立方体
    if (m_cubeEntity != INVALID_ENTITY) {
        m_registry->DestroyEntity(m_cubeEntity);
        m_cubeEntity = INVALID_ENTITY;
    }
}

void GameWorld::CreateTestCube() {
    if (!m_registry || !m_renderer || !m_context) {
        return;
    }

    // 1. 创建立方体几何数据
    GeometryGenerator geoGen;
    auto meshData = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 0);

    // 2. 创建顶点数据（带颜色）
    struct SimpleVertex {
        XMFLOAT3 Position;
        XMFLOAT4 Color;
    };

    std::vector<SimpleVertex> simpleVertices(meshData.Vertices.size());
    for (size_t i = 0; i < meshData.Vertices.size(); ++i) {
        simpleVertices[i].Position = meshData.Vertices[i].Position;

        // 为每个面设置不同颜色
        if (i < 4)
            simpleVertices[i].Color = XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f); // 红色
        else if (i < 8)
            simpleVertices[i].Color = XMFLOAT4(0.0f, 1.0f, 0.0f, 1.0f); // 绿色
        else if (i < 12)
            simpleVertices[i].Color = XMFLOAT4(0.0f, 0.0f, 1.0f, 1.0f); // 蓝色
        else if (i < 16)
            simpleVertices[i].Color = XMFLOAT4(1.0f, 1.0f, 0.0f, 1.0f); // 黄色
        else if (i < 20)
            simpleVertices[i].Color = XMFLOAT4(1.0f, 0.0f, 1.0f, 1.0f); // 紫色
        else
            simpleVertices[i].Color = XMFLOAT4(0.0f, 1.0f, 1.0f, 1.0f); // 青色
    }

    // 3. 创建 GPU 资源
    auto &gpuMgr = GpuResourceManager::GetInstance();
    auto device = m_context->DeviceContext->GetDevice();

    // 创建顶点缓冲区
    size_t vbSize = simpleVertices.size() * sizeof(SimpleVertex);
    auto vbHandle = gpuMgr.CreateBuffer(device, vbSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *vbResource = gpuMgr.GetResource(vbHandle);

    if (vbResource) {
        void *vbMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        vbResource->Map(0, &readRange, &vbMapped);
        memcpy(vbMapped, simpleVertices.data(), vbSize);
        vbResource->Unmap(0, nullptr);
    }

    // 创建索引缓冲区
    size_t ibSize = meshData.Indices32.size() * sizeof(uint32_t);
    auto ibHandle = gpuMgr.CreateBuffer(device, ibSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ibResource = gpuMgr.GetResource(ibHandle);

    if (ibResource) {
        void *ibMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ibResource->Map(0, &readRange, &ibMapped);
        memcpy(ibMapped, meshData.Indices32.data(), ibSize);
        ibResource->Unmap(0, nullptr);
    }

    // 4. 创建实体并添加组件
    m_cubeEntity = m_registry->CreateEntity();

    // Transform 组件
    XMFLOAT3 position(0.0f, 0.0f, 5.0f);
    XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
    XMFLOAT3 scale(1.0f, 1.0f, 1.0f);
    m_registry->AddComponent<TransformComponent>(m_cubeEntity, position, rotation, scale);

    // Mesh 组件
    MeshComponent meshComp;
    meshComp.vertexBuffer = vbHandle;
    meshComp.indexBuffer = ibHandle;
    meshComp.vertexCount = static_cast<uint32_t>(simpleVertices.size());
    meshComp.indexCount = static_cast<uint32_t>(meshData.Indices32.size());
    meshComp.vertexBufferView.BufferLocation = vbResource ? vbResource->GetGPUVirtualAddress() : 0;
    meshComp.vertexBufferView.StrideInBytes = sizeof(SimpleVertex);
    meshComp.vertexBufferView.SizeInBytes = static_cast<UINT>(vbSize);
    meshComp.indexBufferView.BufferLocation = ibResource ? ibResource->GetGPUVirtualAddress() : 0;
    meshComp.indexBufferView.Format = DXGI_FORMAT_R32_UINT;
    meshComp.indexBufferView.SizeInBytes = static_cast<UINT>(ibSize);

    m_registry->AddComponent<MeshComponent>(m_cubeEntity, std::move(meshComp));
}
