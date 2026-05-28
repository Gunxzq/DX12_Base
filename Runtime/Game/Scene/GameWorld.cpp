#include "GameWorld.h"
#include "Boot/GameContext.h"
#include "Common/ThrowHelper.h"
#include "Common/d3dUtil.h"
#include "ECS/Core/Components.h"
#include "ECS/Core/Registry.h"
#include "Framework/SystemRegistry.h"
#include "Math/BoundingVolume.h"
#include "Math/HashTypes.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/Core/RendererRegistry.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/Pipeline/OpaqueRenderer.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/Asset/LODMesh.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Manager/MaterialManager.h"
#include "Resource/Material/MaterialResource.h"
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
using namespace DX12Engine::Math;

using RendererGroup = std::unordered_map<uint64_t, std::vector<const RenderItem *>>;
RendererGroup groups;

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
         .threadType = ThreadType::Worker,
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
                 if (m_context->renderQueue.Empty()) {
                     ;
                     return;
                 }

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
                 D3D12_GPU_VIRTUAL_ADDRESS lightCBAddr = m_context->lightCBAddress;

                 // 开始渲染
                 m_renderer->BeginFrame(cmdList, passCBAddr, lightCBAddr);

                 ID3D12DescriptorHeap *descriptorHeaps[] = {
                     m_context->DescriptorHeaps->GetHeap(DescriptorHeapType::CbvSrvUav)};

                 //  一个堆
                 cmdList.Get()->SetDescriptorHeaps(1, descriptorHeaps);

                 // 遍历所有带 MeshComponent 和 TransformComponent 的实体并渲染
                 const auto &renderQueue = m_context->renderQueue;
                 for (const auto &item : renderQueue.GetItems()) {
                     if (!item.IsValid())
                         continue;
                     m_renderer->DrawMesh(cmdList, item.geometryHandle, item.worldMatrix, item.objectCBAddress,
                                          item.materialCBAddress, m_context->testTextureSRVHandle);
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
        XMFLOAT3 Normal;
        XMFLOAT2 TexCoord;
    };

    std::vector<SimpleVertex> simpleVertices(meshData.Vertices.size());
    for (size_t i = 0; i < meshData.Vertices.size(); ++i) {
        simpleVertices[i].Position = meshData.Vertices[i].Position;
        simpleVertices[i].Normal = meshData.Vertices[i].Normal;
        simpleVertices[i].Color = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        simpleVertices[i].TexCoord = meshData.Vertices[i].TexC;
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

    // 4. 构建 TriangleMesh
    TriangleMesh triangleMesh;
    triangleMesh.vertexBufferHandle = vbHandle;
    triangleMesh.indexBufferHandle = ibHandle;
    triangleMesh.vertexCount = static_cast<uint32_t>(simpleVertices.size());
    triangleMesh.indexCount = static_cast<uint32_t>(meshData.Indices32.size());
    triangleMesh.vertexStride = sizeof(SimpleVertex);
    triangleMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    triangleMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    triangleMesh.isGpuReady = true;

    // 计算包围盒
    BoundingAABB bounds;
    bounds.min = XMFLOAT3(-0.5f, -0.5f, -0.5f);
    bounds.max = XMFLOAT3(0.5f, 0.5f, 0.5f);

    // 5. 注册到 GeometryResourceManager
    auto &geoMgr = m_context->GeometryResourceManager;
    GeometryHandle geoHandle = geoMgr->RegisterTriangleMesh(triangleMesh);

    if (!geoHandle.IsValid()) {
        OutputDebugStringW(L"[ERROR] RegisterTriangleMesh failed!\n");
        return;
    }

    const TriangleMesh *testMesh = geoMgr->GetTriangleMesh(geoHandle);
    if (!testMesh) {
        OutputDebugStringW(L"[ERROR] GetTriangleMesh returned null!\n");
        return;
    }

    // 4. 创建实体并添加组件
    m_cubeEntity = m_registry->CreateEntity();

    // Transform 组件
    XMFLOAT3 position(0.0f, 0.0f, 5.0f);
    XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
    XMFLOAT3 scale(1.0f, 1.0f, 1.0f);
    m_registry->AddComponent<TransformComponent>(m_cubeEntity, position, rotation, scale);

    // 创建 LODMesh（包含 LOD 链）
    LODMesh lodMesh;
    lodMesh.lodChain = {geoHandle}; // 只有一个 LOD，后续可扩展

    LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

    // 创建材质
    MaterialData material;
    material.baseColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    material.metallic = 0.0f;
    material.roughness = 0.2f;
    material.ambient = 0.0f;
    material.alphaCutoff = 0.0f;
    material.alpha = 1.0f;
    material.emissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    material.normalIntensity = 1.0f;

    material.rendererTypeHash = TYPE_HASH("OpaquePBR"); // 需要定义

    MaterialHandle materialHandle = m_context->MaterialMgr->RegisterMaterial(material);

    if (!materialHandle.IsValid()) {
        OutputDebugStringW(L"[ERROR] RegisterMaterial failed!\n");
    }

    // 验证能否取回
    const MaterialData *testMaterial = m_context->MaterialMgr->GetMaterial(materialHandle);
    if (!testMaterial) {
        OutputDebugStringW(L"[ERROR] GetMaterial returned null!\n");
    }

    // Mesh 组件
    MeshComponent meshComp;
    meshComp.lodMeshHandle = lodHandle;
    meshComp.localBounds = bounds;
    meshComp.materialHandle = materialHandle;
    m_registry->AddComponent<MeshComponent>(m_cubeEntity, std::move(meshComp));
}
