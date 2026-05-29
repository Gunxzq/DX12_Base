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
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "Renderer/Pipeline/OpaqueRenderer.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/LightManager.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/Asset/LODMesh.h"
#include "Resource/AssetLoader/AssetLoader.h"
#include "Resource/AssetLoader/Loader/DDSLoader.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Manager/MaterialManager.h"
#include "Resource/Material/MaterialResource.h"
#include "Resource/Texture/TextureManager.h"
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

    LoadTestTexture();
    CreateMaterials();
    CreateTestCube();
    CreateTerrain();
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

    // 2. 创建 GPU 资源（直接使用 GeometryGenerator::Vertex）
    auto &gpuMgr = GpuResourceManager::GetInstance();
    auto device = m_context->DeviceContext->GetDevice();

    // 创建顶点缓冲区
    size_t vbSize = meshData.Vertices.size() * sizeof(GeometryGenerator::Vertex);
    auto vbHandle = gpuMgr.CreateBuffer(device, vbSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *vbResource = gpuMgr.GetResource(vbHandle);

    if (vbResource) {
        void *vbMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        vbResource->Map(0, &readRange, &vbMapped);
        memcpy(vbMapped, meshData.Vertices.data(), vbSize);
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

    // 3. 构建 TriangleMesh
    TriangleMesh triangleMesh;
    triangleMesh.vertexBufferHandle = vbHandle;
    triangleMesh.indexBufferHandle = ibHandle;
    triangleMesh.vertexCount = static_cast<uint32_t>(meshData.Vertices.size());
    triangleMesh.indexCount = static_cast<uint32_t>(meshData.Indices32.size());
    triangleMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
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

    // Transform 组件 — 放在地形上方高处
    XMFLOAT3 position(0.0f, 15.0f, 5.0f);
    XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
    XMFLOAT3 scale(1.0f, 1.0f, 1.0f);
    m_registry->AddComponent<TransformComponent>(m_cubeEntity, position, rotation, scale);

    // 创建 LODMesh（包含 LOD 链）
    LODMesh lodMesh;
    lodMesh.lodChain = {geoHandle}; // 只有一个 LOD，后续可扩展

    LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

    // 使用预创建的材质句柄
    MaterialHandle materialHandle = m_cubeMaterialHandle;

    if (!materialHandle.IsValid()) {
        OutputDebugStringW(L"[ERROR] Cube material handle is invalid!\n");
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
    meshComp.textureHandle = m_testTextureHandle; // 使用成员变量
    m_registry->AddComponent<MeshComponent>(m_cubeEntity, std::move(meshComp));

    // 注册旋转系统
    RegisterRotationSystem();
    // 注册立方体渲染系统
    RegisterCubeRenderSystem();
}

void GameWorld::LoadTestTexture() {

    DDSTextureInfo ddsInfo;
    std::wstring texturePath = L"Content/Textures/WoodCrate01.dds";

    if (!AssetLoader::GetInstance().LoadTextureFromFile(texturePath, ddsInfo)) {
        OutputDebugStringW(L"[ERROR] Failed to load test texture\n");
        return;
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Device *device = m_context->DeviceContext->GetDevice();
    GpuResourceHandle gpuHandle = gpuMgr.CreateTexture2D(device, ddsInfo.desc, D3D12_RESOURCE_STATE_COMMON);

    if (!gpuHandle.IsValid()) {
        OutputDebugStringW(L"[ERROR] Failed to create GPU texture\n");
        return;
    }

    auto &descriptorHeaps = m_context->DescriptorHeaps;
    uint32_t srvIndex = descriptorHeaps->Allocate(DescriptorHeapType::CbvSrvUav);

    if (srvIndex == UINT32_MAX) {
        OutputDebugStringW(L"[ERROR] Failed to allocate SRV index\n");
        gpuMgr.Release(gpuHandle, 0);
        return;
    }

    // 创建 SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = ddsInfo.desc.Format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = ddsInfo.desc.MipLevels;
    srvDesc.Texture2D.MostDetailedMip = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetCpuHandle(DescriptorHeapType::CbvSrvUav, srvIndex);
    device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);

    // 注册到 TextureManager
    TextureManager *texMgr = m_context->TextureMgr;
    m_testTextureHandle = texMgr->RegisterTexture(gpuHandle, srvIndex);

    // 上传纹理数据
    uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
    auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
    auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
    auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

    std::vector<D3D12_SUBRESOURCE_DATA> subresources = ddsInfo.subresources;

    UINT64 requiredSize =
        GetRequiredIntermediateSize(gpuMgr.GetResource(gpuHandle), 0, static_cast<UINT>(subresources.size()));

    GpuResourceHandle uploadHandle =
        gpuMgr.CreateBuffer(device, requiredSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

    auto barrier1 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COMMON,
                                                         D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList.Get()->ResourceBarrier(1, &barrier1);

    UpdateSubresources(cmdList.Get(), gpuMgr.GetResource(gpuHandle), gpuMgr.GetResource(uploadHandle), 0, 0,
                       static_cast<UINT>(subresources.size()), subresources.data());

    auto barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COPY_DEST,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList.Get()->ResourceBarrier(1, &barrier2);

    cmdList.Close();

    m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList);
    m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);

    uint64_t sequence = m_context->GetNextSequence();
    gpuMgr.Release(uploadHandle, sequence);

    m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);
    m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);

    m_context->Logging->Info("[GameWorld] Test texture loaded successfully");
}

void GameWorld::CreateMaterials() {
    auto *materialMgr = m_context->MaterialMgr;

    // 立方体材质
    MaterialData cubeMaterial;
    cubeMaterial.materialId = TYPE_HASH("cube_material");
    cubeMaterial.name = "cube_material";
    cubeMaterial.baseColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    cubeMaterial.metallic = 0.0f;
    cubeMaterial.roughness = 0.5f;
    cubeMaterial.ambient = 0.5f;
    cubeMaterial.alpha = 1.0f;
    cubeMaterial.rendererTypeHash = TYPE_HASH("OpaquePBR");
    m_cubeMaterialHandle = materialMgr->RegisterMaterial(cubeMaterial);

    // 地形材质
    MaterialData terrainMaterial;
    terrainMaterial.materialId = TYPE_HASH("terrain_material");
    terrainMaterial.name = "terrain_material";
    terrainMaterial.baseColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    terrainMaterial.metallic = 0.0f;
    terrainMaterial.roughness = 0.8f;
    terrainMaterial.ambient = 0.8f;
    terrainMaterial.alpha = 1.0f;
    terrainMaterial.rendererTypeHash = TYPE_HASH("OpaquePBR");
    m_terrainMaterialHandle = materialMgr->RegisterMaterial(terrainMaterial);

    // ========================================================================
    // 创建材质数组 GPU Buffer 和 SRV
    // ========================================================================
    auto materialList = materialMgr->GetGPUMaterialList();
    if (materialList.empty()) {
        m_context->Logging->Error("[GameWorld] Material list is empty, cannot create material buffer");
        return;
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Device *device = m_context->DeviceContext->GetDevice();

    // 提取 MaterialConstants 数组
    std::vector<MaterialConstants> gpuData;
    gpuData.reserve(materialList.size());
    for (auto &[idx, constants] : materialList) {
        gpuData.push_back(constants);
    }

    size_t bufferSize = gpuData.size() * sizeof(MaterialConstants);

    // 创建默认堆缓冲区（GPU 端）
    GpuResourceHandle bufferHandle =
        gpuMgr.CreateBuffer(device, bufferSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);

    if (!bufferHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] Failed to create material GPU buffer");
        return;
    }

    // 创建上传堆缓冲区
    GpuResourceHandle uploadHandle =
        gpuMgr.CreateBuffer(device, bufferSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

    if (!uploadHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] Failed to create material upload buffer");
        gpuMgr.Release(bufferHandle, 0);
        return;
    }

    // 写入数据到上传缓冲区
    ID3D12Resource *uploadResource = gpuMgr.GetResource(uploadHandle);
    void *mappedData = nullptr;
    uploadResource->Map(0, nullptr, &mappedData);
    memcpy(mappedData, gpuData.data(), bufferSize);
    uploadResource->Unmap(0, nullptr);

    // 使用命令列表上传
    uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
    auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
    auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
    auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

    // 屏障：COMMON -> COPY_DEST
    auto barrier1 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(bufferHandle), D3D12_RESOURCE_STATE_COMMON,
                                                         D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList.Get()->ResourceBarrier(1, &barrier1);

    // 复制数据
    cmdList.Get()->CopyResource(gpuMgr.GetResource(bufferHandle), uploadResource);

    // 屏障：COPY_DEST -> NON_PIXEL_SHADER_RESOURCE
    auto barrier2 =
        CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(bufferHandle), D3D12_RESOURCE_STATE_COPY_DEST,
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList.Get()->ResourceBarrier(1, &barrier2);

    cmdList.Close();

    m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList);
    m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);

    uint64_t sequence = m_context->GetNextSequence();
    gpuMgr.Release(uploadHandle, sequence);

    m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);
    m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);

    // 分配 SRV 描述符
    auto &descriptorHeaps = m_context->DescriptorHeaps;
    uint32_t srvIndex = descriptorHeaps->Allocate(DescriptorHeapType::CbvSrvUav);
    if (srvIndex == UINT32_MAX) {
        m_context->Logging->Error("[GameWorld] Failed to allocate SRV for material buffer");
        gpuMgr.Release(bufferHandle, sequence);
        return;
    }

    // 创建 SRV（StructuredBuffer<MaterialConstants>）
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = static_cast<UINT>(gpuData.size());
    srvDesc.Buffer.StructureByteStride = sizeof(MaterialConstants);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetCpuHandle(DescriptorHeapType::CbvSrvUav, srvIndex);
    device->CreateShaderResourceView(gpuMgr.GetResource(bufferHandle), &srvDesc, cpuHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = descriptorHeaps->GetGpuHandle(DescriptorHeapType::CbvSrvUav, srvIndex);
    materialMgr->SetMaterialBufferSRV(gpuHandle);

    // 存储 bufferHandle 以便后续释放
    m_materialBufferHandle = bufferHandle;

    m_context->Logging->Info("[GameWorld] Material buffer created and SRV set");
}

void GameWorld::RegisterRotationSystem() {
    SystemRegistry::Register({.name = "CubeRotationSystem",
                              .func =
                                  [this](Registry &registry, const MessageContext &ctx) {
                                      float deltaTime = m_context->MainTimer->GetDeltaTime();
                                      // 只旋转立方体，不影响地形等其他实体
                                      if (m_cubeEntity == INVALID_ENTITY)
                                          return;
                                      auto *transform = registry.TryGetComponent<TransformComponent>(m_cubeEntity);
                                      if (transform) {
                                          transform->rotation.y += deltaTime * 2.0f;
                                      }
                                  },
                              .phase = TaskPhase::Update,
                              .threadType = ThreadType::Worker,
                              .priority = TaskPriority::Normal,
                              .alwaysRun = true});
}

void GameWorld::RegisterCubeRenderSystem() {
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
                 D3D12_GPU_VIRTUAL_ADDRESS lightCBAddr = LightManager::GetInstance().GetLightCBAddress();

                 // 获取材质数组 SRV
                 D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV = m_context->MaterialMgr->GetMaterialBufferSRV();

                 ID3D12DescriptorHeap *descriptorHeaps[] = {
                     m_context->DescriptorHeaps->GetHeap(DescriptorHeapType::CbvSrvUav)};

                 //  一个堆
                 cmdList.Get()->SetDescriptorHeaps(1, descriptorHeaps);

                 // 开始渲染
                 m_renderer->BeginFrame(cmdList, passCBAddr, lightCBAddr, materialBufferSRV);

                 // 遍历所有带 MeshComponent 和 TransformComponent 的实体并渲染
                 const auto &renderQueue = m_context->renderQueue;
                 for (const auto &item : renderQueue.GetItems()) {
                     if (!item.IsValid())
                         continue;
                     D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV = {}; // TODO: 从场景获取
                     m_renderer->DrawMesh(cmdList, item.geometryHandle, item.worldMatrix, item.objectCBAddress,
                                          item.textureSRV, envMapSRV);
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

void GameWorld::CreateTerrain() {
    if (!m_registry || !m_renderer || !m_context) {
        return;
    }

    m_context->Logging->Info("[GameWorld] Creating terrain...");

    CreateTerrainMesh();     // 第一步：从 PNG 生成网格数据
    UploadTerrainGeometry(); // 第二步：上传到 GPU
    CreateTerrainMaterial(); // 第三步：创建材质
    CreateTerrainEntity();   // 第四步：创建实体

    m_context->Logging->Info("[GameWorld] Terrain created successfully");
}

void GameWorld::CreateTerrainMesh() {
    // 地形参数
    float width = 100.0f;    // 地形宽度（X 轴）
    float depth = 100.0f;    // 地形深度（Z 轴）
    float maxHeight = 20.0f; // 最大高度
    uint32_t segments = 257; // 257x257 网格（256x256 个格子）

    // 加载高度图 PNG
    std::wstring heightmapPath = L"Content/Terrain/heightmap.png";

    if (!AssetLoader::GetInstance().LoadTerrainFromFile(heightmapPath, width, depth, maxHeight, segments,
                                                        m_terrainMeshData)) {
        m_context->Logging->Error("[GameWorld] Failed to load terrain heightmap");
        return;
    }
}

void GameWorld::UploadTerrainGeometry() {
    if (m_terrainMeshData.vertices.empty()) {
        return;
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();
    auto device = m_context->DeviceContext->GetDevice();
    auto &geoMgr = m_context->GeometryResourceManager;

    // 计算顶点和索引缓冲区大小
    size_t vbSize = m_terrainMeshData.vertices.size() * sizeof(GeometryGenerator::Vertex);
    size_t ibSize = m_terrainMeshData.indices.size() * sizeof(uint32_t);

    // 创建顶点缓冲区（UPLOAD 堆）
    auto vbHandle = gpuMgr.CreateBuffer(device, vbSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *vbResource = gpuMgr.GetResource(vbHandle);

    if (vbResource) {
        void *vbMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        vbResource->Map(0, &readRange, &vbMapped);
        memcpy(vbMapped, m_terrainMeshData.vertices.data(), vbSize);
        vbResource->Unmap(0, nullptr);
    }

    // 创建索引缓冲区（UPLOAD 堆）
    auto ibHandle = gpuMgr.CreateBuffer(device, ibSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ibResource = gpuMgr.GetResource(ibHandle);

    if (ibResource) {
        void *ibMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ibResource->Map(0, &readRange, &ibMapped);
        memcpy(ibMapped, m_terrainMeshData.indices.data(), ibSize);
        ibResource->Unmap(0, nullptr);
    }

    // 构建 TriangleMesh
    TriangleMesh triangleMesh;
    triangleMesh.vertexBufferHandle = vbHandle;
    triangleMesh.indexBufferHandle = ibHandle;
    triangleMesh.vertexCount = static_cast<uint32_t>(m_terrainMeshData.vertices.size());
    triangleMesh.indexCount = static_cast<uint32_t>(m_terrainMeshData.indices.size());
    triangleMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
    triangleMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    triangleMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    triangleMesh.isGpuReady = true;

    // 注册到 GeometryResourceManager
    m_terrainGeometryHandle = geoMgr->RegisterTriangleMesh(triangleMesh);

    if (!m_terrainGeometryHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] Failed to register terrain geometry");
    }
}

void GameWorld::CreateTerrainEntity() {
    if (!m_terrainGeometryHandle.IsValid() || !m_terrainMaterialHandle.IsValid()) {
        return;
    }

    m_terrainEntity = m_registry->CreateEntity();

    // Transform 组件（地形放在原点）
    XMFLOAT3 position(0.0f, -10.0f, 0.0f); // 将地形下移，让立方体在地形上方
    XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
    XMFLOAT3 scale(1.0f, 1.0f, 1.0f);
    m_registry->AddComponent<TransformComponent>(m_terrainEntity, position, rotation, scale);

    // 计算包围盒
    BoundingAABB bounds;
    bounds.min = XMFLOAT3(-m_terrainMeshData.width * 0.5f, 0.0f, -m_terrainMeshData.depth * 0.5f);
    bounds.max = XMFLOAT3(m_terrainMeshData.width * 0.5f, m_terrainMeshData.maxHeight, m_terrainMeshData.depth * 0.5f);

    // 创建 LODMesh
    LODMesh lodMesh;
    lodMesh.lodChain = {m_terrainGeometryHandle};

    LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

    // Mesh 组件
    MeshComponent meshComp;
    meshComp.lodMeshHandle = lodHandle;
    meshComp.localBounds = bounds;
    meshComp.materialHandle = m_terrainMaterialHandle;
    meshComp.textureHandle = m_terrainTextureHandle; // TODO: 设置纹理句柄

    m_registry->AddComponent<MeshComponent>(m_terrainEntity, std::move(meshComp));
}

void GameWorld::CreateTerrainMaterial() {
    // 加载地形漫反射贴图
    std::wstring texturePath = L"Content/Textures/heightmap.dds";

    // 复制 LoadTestTexture 的纹理加载逻辑
    DDSTextureInfo ddsInfo;
    if (!AssetLoader::GetInstance().LoadTextureFromFile(texturePath, ddsInfo)) {
        // 即使纹理加载失败，也继续创建材质（使用默认颜色）
    } else {
        auto &gpuMgr = GpuResourceManager::GetInstance();
        ID3D12Device *device = m_context->DeviceContext->GetDevice();
        GpuResourceHandle gpuHandle = gpuMgr.CreateTexture2D(device, ddsInfo.desc, D3D12_RESOURCE_STATE_COMMON);

        if (gpuHandle.IsValid()) {
            auto &descriptorHeaps = m_context->DescriptorHeaps;
            uint32_t srvIndex = descriptorHeaps->Allocate(DescriptorHeapType::CbvSrvUav);

            if (srvIndex != UINT32_MAX) {
                // 创建 SRV
                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                srvDesc.Format = ddsInfo.desc.Format;
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = ddsInfo.desc.MipLevels;
                srvDesc.Texture2D.MostDetailedMip = 0;

                D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
                    descriptorHeaps->GetCpuHandle(DescriptorHeapType::CbvSrvUav, srvIndex);
                device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);

                // 注册到 TextureManager
                TextureManager *texMgr = m_context->TextureMgr;
                m_terrainTextureHandle = texMgr->RegisterTexture(gpuHandle, srvIndex);

                // 上传纹理数据
                uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                std::vector<D3D12_SUBRESOURCE_DATA> subresources = ddsInfo.subresources;

                UINT64 requiredSize = GetRequiredIntermediateSize(gpuMgr.GetResource(gpuHandle), 0,
                                                                  static_cast<UINT>(subresources.size()));

                GpuResourceHandle uploadHandle = gpuMgr.CreateBuffer(device, requiredSize, D3D12_HEAP_TYPE_UPLOAD,
                                                                     D3D12_RESOURCE_STATE_GENERIC_READ);

                auto barrier1 = CD3DX12_RESOURCE_BARRIER::Transition(
                    gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                cmdList.Get()->ResourceBarrier(1, &barrier1);

                UpdateSubresources(cmdList.Get(), gpuMgr.GetResource(gpuHandle), gpuMgr.GetResource(uploadHandle), 0, 0,
                                   static_cast<UINT>(subresources.size()), subresources.data());

                auto barrier2 =
                    CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COPY_DEST,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                cmdList.Get()->ResourceBarrier(1, &barrier2);

                cmdList.Close();

                m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList);
                m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);

                uint64_t sequence = m_context->GetNextSequence();
                gpuMgr.Release(uploadHandle, sequence);

                m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);
                m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);

                m_context->Logging->Info("[GameWorld] Terrain texture loaded successfully");
            } else {
                m_context->Logging->Error("[GameWorld] Failed to allocate SRV for terrain texture");
                gpuMgr.Release(gpuHandle, 0);
            }
        } else {
            m_context->Logging->Error("[GameWorld] Failed to create GPU texture for terrain");
        }
    }

    // 材质已在 CreateMaterials() 中注册，这里只需要验证
    if (!m_terrainMaterialHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] Terrain material handle is invalid");
    }
}