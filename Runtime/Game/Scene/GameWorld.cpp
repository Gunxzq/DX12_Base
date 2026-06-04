#include "GameWorld.h"
#include "Boot/GameContext.h"
#include "Common/ThrowHelper.h"
#include "Common/d3dUtil.h"
#include "ECS/Core/Components.h"
#include "ECS/Core/Registry.h"
#include "Event/EventRegistry.h"
#include "Event/EventTypes.h"
#include "Event/MessageDispatcher.h"
#include "Framework/SystemRegistry.h"
#include "Math/BoundingVolume.h"
#include "Math/HashTypes.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/Core/RendererRegistry.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"

#include "Renderer/Pipeline/OpaqueRenderer.h"
#include "Renderer/Pipeline/ShadowRenderer.h"
#include "Renderer/Pipeline/SkyRenderer.h"
#include "Renderer/Pipeline/WaterRenderer.h"

#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/LightManager/LightManager.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/Asset/LODMesh.h"
#include "Resource/AssetDataManager.h"
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

// 异步加载任务
#include "Async/BackgroundExecutor.h"
#include "Async/ResourceTransitionTask.h"
#include "Async/TerrainLoadTask.h"

#include <DirectXMath.h>
#include <wrl/client.h>

using namespace DirectX;
using namespace Microsoft::WRL;
using namespace DX12Engine;
using namespace DX12Engine::Async;
using namespace DX12Engine::Boot;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::Scheduler;
using namespace DX12Engine::Math;

GameWorld::GameWorld() = default;
GameWorld::~GameWorld() = default;

void GameWorld::Initialize(GameContext *context, OpaqueRenderer *renderer) {
    m_context = context;
    m_registry = context->Registry;
    m_renderer = renderer;

    // 初始化新的 OpaqueRenderItemBuilder
    m_opaqueBuilder = std::make_unique<OpaqueRenderItemBuilder>(m_context->FrameResourceManager, m_context->MaterialMgr,
                                                                m_context->TextureMgr);

    // 初始化新的 TransparentRenderItemBuilder（需要相机用于远到近排序）
    m_transparentBuilder = std::make_unique<TransparentRenderItemBuilder>(
        m_context->FrameResourceManager, m_context->MaterialMgr, m_context->TextureMgr, m_context->CameraMgr);

    // 初始化水渲染器
    m_waterRenderer = std::make_unique<WaterRenderer>();
    m_waterRenderer->SetDeviceContext(m_context->DeviceContext);
    m_waterRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_waterRenderer->SetMaterialManager(m_context->MaterialMgr);
    m_waterRenderer->Initialize();

    // // 初始化阴影渲染器
    m_shadowRenderer = std::make_unique<ShadowRenderer>();
    m_shadowRenderer->SetDeviceContext(m_context->DeviceContext);
    m_shadowRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_shadowRenderer->Initialize();

    LoadTestTexture();
    LoadWaterTexture();

    CreateMaterials();
    CreateSkybox();
    CreateGroundPlane();
    CreateTestCube();

    // 创建后台异步执行器（类比帧驱动器，2 个工作线程）
    // 必须在 LoadTerrainAsync() 之前创建！
    m_backgroundExecutor = std::make_unique<BackgroundExecutor>(2);
    m_backgroundExecutor->SetCommandManager(&m_context->DeviceContext->GetCommandManager());

    // 注册地形异步加载响应 System（消息驱动：BackgroundExecutor 提交完成 → PostEvent → System 执行）
    RegisterTerrainSystems();

    LoadTerrainAsync();

    CreateWater();

    // 注册水常量立即回调（每帧上传水波动画数据）
    RegisterWaterConstantsCallback();

    // 注册阴影渲染系统
    RegisterShadowRenderSystem();
}

void GameWorld::Clear() {
    if (!m_registry)
        return;

    // 移除所有测试立方体
    for (auto entity : m_cubeEntities) {
        if (entity != INVALID_ENTITY) {
            m_registry->DestroyEntity(entity);
        }
    }
    m_cubeEntities.clear();
    m_cubeEntity = INVALID_ENTITY;

    // 移除地面平面
    if (m_groundPlaneEntity != INVALID_ENTITY) {
        m_registry->DestroyEntity(m_groundPlaneEntity);
        m_groundPlaneEntity = INVALID_ENTITY;
    }
}

void GameWorld::BuildRenderQueue() {
    if (!m_registry)
        return;

    // 设置每帧数据
    m_opaqueBuilder->SetCullingResult(&m_context->cullingResult);
    m_opaqueBuilder->SetLODResult(&m_context->lodResult);

    m_transparentBuilder->SetCullingResult(&m_context->cullingResult);
    m_transparentBuilder->SetLODResult(&m_context->lodResult);

    // 构建不透明渲染双队列（Standard / Instanced 分离，避免渲染时 PSO 切换）
    m_opaqueBuilder->BuildDualQueue(*m_registry, m_opaqueQueueStandard, m_opaqueQueueInstanced);

    // 构建透明渲染队列（从远到近排序）
    m_transparentBuilder->BuildTyped(*m_registry, m_transparentQueue);
}

void GameWorld::RegisterWaterConstantsCallback() {
    if (!m_context || !m_context->FrameDriver)
        return;

    m_context->FrameDriver->RegisterImmediateCallback([this]() {
        const auto &passConstants = m_context->FrameResourceManager->GetPassConstants();

        WaterConstants waterCB;
        waterCB.Time = passConstants.TotalTime;
        waterCB.WaveAmplitude = 0.5f + sin(passConstants.TotalTime * 0.5f) * 0.2f;
        waterCB.WaveSpeed = 1.5f;
        waterCB.WaveFrequency = 2.0f;
        waterCB.RefractionStrength = 0.3f;
        waterCB.FresnelPower = 2.0f;
        waterCB.FoamIntensity = 0.5f;
        waterCB.ReflectionTextureIndex = 0;
        waterCB.RefractionTextureIndex = 0;
        waterCB.DepthTextureIndex = 0;
        waterCB.NormalTextureIndex = 0;
        waterCB.Pad = 0;

        m_waterCBAddress = m_context->FrameResourceManager->AllocateWaterCB(&waterCB, sizeof(WaterConstants));
    });
}

void GameWorld::CreateGroundPlane() {
    if (!m_registry || !m_renderer || !m_context)
        return;

    // 1. 生成 10x10 平面几何体（XZ 平面，法线朝上）
    GeometryGenerator geoGen;
    auto meshData = geoGen.CreateGrid(10.0f, 10.0f, 10, 10); // 10x10 平面，2x2 顶点

    // 2. 创建 GPU 资源
    auto &gpuMgr = GpuResourceManager::GetInstance();
    auto device = m_context->DeviceContext->GetDevice();

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
    TriangleMesh planeMesh;
    planeMesh.vertexBufferHandle = vbHandle;
    planeMesh.indexBufferHandle = ibHandle;
    planeMesh.vertexCount = static_cast<uint32_t>(meshData.Vertices.size());
    planeMesh.indexCount = static_cast<uint32_t>(meshData.Indices32.size());
    planeMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
    planeMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    planeMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    planeMesh.isGpuReady = true;

    // 包围盒：10x10 平面在 XZ，Y=0
    BoundingAABB bounds;
    bounds.min = XMFLOAT3(-5.0f, 0.0f, -5.0f);
    bounds.max = XMFLOAT3(5.0f, 0.0f, 5.0f);

    auto &geoMgr = m_context->GeometryResourceManager;
    GeometryHandle geoHandle = geoMgr->RegisterTriangleMesh(planeMesh);
    if (!geoHandle.IsValid()) {
        OutputDebugStringW(L"[ERROR] RegisterTriangleMesh for ground plane failed!\n");
        return;
    }

    // 4. 创建 LODMesh
    LODMesh lodMesh;
    lodMesh.lodChain = {geoHandle};
    LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

    // 5. 创建实体 — 平面放在 Y=5.0f，立方体底部与之重合
    m_groundPlaneEntity = m_registry->CreateEntity();

    XMFLOAT3 position(0.0f, 10.0f, 0.0f);
    XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
    XMFLOAT3 scale(1.0f, 1.0f, 1.0f);
    m_registry->AddComponent<TransformComponent>(m_groundPlaneEntity, position, rotation, scale);

    MeshComponent meshComp;
    meshComp.lodMeshHandle = lodHandle;
    meshComp.localBounds = bounds;
    meshComp.materialHandle = m_cubeMaterialHandle; // 复用立方体材质
    meshComp.textureHandle = m_testTextureHandle;
    meshComp.receivesShadow = true;
    m_registry->AddComponent<MeshComponent>(m_groundPlaneEntity, std::move(meshComp));

    OutputDebugStringW(L"[GameWorld] Ground plane created at Y=5.0 (10x10)\n");
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

    // 平面在 Y=5.0f，立方体底部 = 平面Y + 立方体半高 = 5.0 + scale*0.5
    constexpr float PLANE_Y = 10.0f;

    struct CubePlacement {
        XMFLOAT3 position;
        XMFLOAT3 rotation;
        XMFLOAT3 scale;
    };

    // 不同位置、不同大小的立方体，底部均落在 Y=5.0 平面上
    const std::vector<CubePlacement> cubePlacements = {
        // 第一个：中心偏前，标准大小 — cube bottom = 5.0 + 0.5*1 = 5.5
        {{0.0f, PLANE_Y + 0.5f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        // 第二个：右前方，放大 2 倍 — bottom = 5.0 + 0.5*2 = 6.0
        {{3.0f, PLANE_Y + 1.0f, 2.0f}, {0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f}},
        // 第三个：左前方，1.5倍 — bottom = 5.0 + 0.5*1.5 = 5.75
        {{-3.0f, PLANE_Y + 0.75f, 1.0f}, {0.0f, 0.0f, 0.0f}, {1.5f, 1.5f, 1.5f}},
        // 第四个：后方，标准大小 — bottom = 5.5
        {{0.0f, PLANE_Y + 0.5f, -3.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        // 第五个：右侧，3倍 — bottom = 5.0 + 0.5*3 = 6.5
        {{3.0f, PLANE_Y + 1.5f, -1.0f}, {0.0f, 0.0f, 0.0f}, {3.0f, 3.0f, 3.0f}},
        // 第六个：左侧，2倍 — bottom = 6.0
        {{-3.0f, PLANE_Y + 1.0f, -2.0f}, {0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f}},
    };

    m_cubeEntities.clear();
    m_cubeEntities.reserve(cubePlacements.size());

    for (size_t i = 0; i < cubePlacements.size(); ++i) {
        auto entity = m_registry->CreateEntity();

        const auto &placement = cubePlacements[i];
        m_registry->AddComponent<TransformComponent>(entity, placement.position, placement.rotation, placement.scale);

        // 创建 LODMesh（共享同一个 geoHandle）
        LODMesh lodMesh;
        lodMesh.lodChain = {geoHandle};

        LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

        // Mesh 组件
        MeshComponent meshComp;
        meshComp.lodMeshHandle = lodHandle;
        meshComp.localBounds = bounds;
        meshComp.materialHandle = m_cubeMaterialHandle;
        meshComp.textureHandle = m_testTextureHandle;
        m_registry->AddComponent<MeshComponent>(entity, std::move(meshComp));

        m_cubeEntities.push_back(entity);
    }

    // 保持第一个立方体为 m_cubeEntity（兼容旧代码）
    m_cubeEntity = m_cubeEntities.empty() ? INVALID_ENTITY : m_cubeEntities[0];

    // 注册旋转系统
    // RegisterRotationSystem();
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

    // 水材质
    MaterialData waterMaterial;
    waterMaterial.materialId = TYPE_HASH("water_material");
    waterMaterial.name = "water_material";
    waterMaterial.baseColor = XMFLOAT4(0.2f, 0.4f, 0.6f, 0.8f); // 半透明蓝色
    waterMaterial.metallic = 0.0f;
    waterMaterial.roughness = 0.2f; // 光滑表面
    waterMaterial.ambient = 0.5f;
    waterMaterial.alpha = 0.8f; // 半透明
    waterMaterial.rendererTypeHash = TYPE_HASH("TransparentPBR");
    m_waterMaterialHandle = materialMgr->RegisterMaterial(waterMaterial);

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

void GameWorld::CreateSkybox() {
    m_skyRenderer = std::make_unique<SkyRenderer>();
    m_skyRenderer->SetDeviceContext(m_context->DeviceContext);
    m_skyRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_skyRenderer->Initialize();

    // ========================================================================
    // 1. 加载天空盒纹理 (snowcube1024.dds)
    // ========================================================================
    DDSTextureInfo ddsInfo;
    std::wstring texturePath = L"Content/Textures/snowcube1024.dds";

    if (!AssetLoader::GetInstance().LoadTextureFromFile(texturePath, ddsInfo)) {
        m_context->Logging->Error("[GameWorld] Failed to load skybox texture");
        return;
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Device *device = m_context->DeviceContext->GetDevice();

    GpuResourceHandle gpuHandle = gpuMgr.CreateTexture2D(device, ddsInfo.desc, D3D12_RESOURCE_STATE_COMMON);

    if (!gpuHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] Failed to create skybox GPU texture");
        return;
    }

    auto &descriptorHeaps = m_context->DescriptorHeaps;
    uint32_t srvIndex = descriptorHeaps->Allocate(DescriptorHeapType::CbvSrvUav);

    if (srvIndex == UINT32_MAX) {
        m_context->Logging->Error("[GameWorld] Failed to allocate SRV for skybox");
        gpuMgr.Release(gpuHandle, 0);
        return;
    }

    // 创建立方体贴图 SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = ddsInfo.desc.Format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = ddsInfo.desc.MipLevels;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetCpuHandle(DescriptorHeapType::CbvSrvUav, srvIndex);
    device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);

    // 注册到 TextureManager
    TextureManager *texMgr = m_context->TextureMgr;
    m_skyboxTextureHandle = texMgr->RegisterTexture(gpuHandle, srvIndex);

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

    m_context->Logging->Info("[GameWorld] Skybox texture loaded successfully");

    // ========================================================================
    // 2. 创建天空盒几何体（单位立方体）
    // ========================================================================
    GeometryGenerator geoGen;
    auto skyMeshData = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 0);

    size_t vbSize = skyMeshData.Vertices.size() * sizeof(GeometryGenerator::Vertex);
    auto vbHandle = gpuMgr.CreateBuffer(device, vbSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *vbResource = gpuMgr.GetResource(vbHandle);

    if (vbResource) {
        void *vbMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        vbResource->Map(0, &readRange, &vbMapped);
        memcpy(vbMapped, skyMeshData.Vertices.data(), vbSize);
        vbResource->Unmap(0, nullptr);
    }

    size_t ibSize = skyMeshData.Indices32.size() * sizeof(uint32_t);
    auto ibHandle = gpuMgr.CreateBuffer(device, ibSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ibResource = gpuMgr.GetResource(ibHandle);

    if (ibResource) {
        void *ibMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ibResource->Map(0, &readRange, &ibMapped);
        memcpy(ibMapped, skyMeshData.Indices32.data(), ibSize);
        ibResource->Unmap(0, nullptr);
    }

    TriangleMesh skyTriangleMesh;
    skyTriangleMesh.vertexBufferHandle = vbHandle;
    skyTriangleMesh.indexBufferHandle = ibHandle;
    skyTriangleMesh.vertexCount = static_cast<uint32_t>(skyMeshData.Vertices.size());
    skyTriangleMesh.indexCount = static_cast<uint32_t>(skyMeshData.Indices32.size());
    skyTriangleMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
    skyTriangleMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    skyTriangleMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    skyTriangleMesh.isGpuReady = true;

    m_skyboxGeometryHandle = m_context->GeometryResourceManager->RegisterTriangleMesh(skyTriangleMesh);

    if (!m_skyboxGeometryHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] Failed to register skybox geometry");
        return;
    }

    // ========================================================================
    // 3. 创建天空盒 Object CB（单位矩阵，cbPerObject slot 0）
    // ========================================================================
    ObjectConstants skyObjCB;
    DirectX::XMStoreFloat4x4(&skyObjCB.World, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&skyObjCB.WorldInvTranspose, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&skyObjCB.PrevWorld, DirectX::XMMatrixIdentity());
    skyObjCB.MaterialIndex = 0;
    skyObjCB.ReceiveShadow = 0;

    m_skyboxObjectCBAddress = m_context->FrameResourceManager->AllocateObjectCB(&skyObjCB, sizeof(ObjectConstants));

    m_context->Logging->Info("[GameWorld] Skybox created successfully");

    // 注册天空盒任务
    RegisterSkyboxSystem();
}

void GameWorld::RegisterRotationSystem() {
    SystemRegistry::Register({.name = "CubeRotationSystem",
                              .func =
                                  [this](Registry &registry, const MessageContext &ctx) {
                                      float deltaTime = m_context->MainTimer->GetDeltaTime();
                                      // 旋转所有立方体
                                      for (auto &entity : m_cubeEntities) {
                                          if (entity == INVALID_ENTITY)
                                              continue;
                                          auto *transform = registry.TryGetComponent<TransformComponent>(entity);
                                          if (transform) {
                                              transform->rotation.y += deltaTime * 2.0f;
                                          }
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
                 if (m_opaqueQueueStandard.Empty() && m_opaqueQueueInstanced.Empty()) {
                     m_context->Logging->Debug("[CubeRender] Opaque queues are empty, skipping draw");
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

                 // 获取阴影数据 StructuredBuffer SRV (t11,space1) 和阴影贴图纹理 SRV (t14,space1)
                 auto &lightMgr = LightManager::GetInstance();
                 D3D12_GPU_DESCRIPTOR_HANDLE shadowDataSRV = lightMgr.GetShadowDataSRV();
                 D3D12_GPU_DESCRIPTOR_HANDLE shadowMapSRV = lightMgr.GetShadowMapSRV();

                 ID3D12DescriptorHeap *descriptorHeaps[] = {
                     m_context->DescriptorHeaps->GetHeap(DescriptorHeapType::CbvSrvUav)};

                 //  一个堆
                 cmdList.Get()->SetDescriptorHeaps(1, descriptorHeaps);

                 // 开始渲染（传入阴影数据和阴影贴图 SRV）
                 m_renderer->BeginFrame(cmdList, passCBAddr, lightCBAddr, materialBufferSRV, shadowDataSRV,
                                        shadowMapSRV);

                 D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV = {}; // TODO: 从场景获取

                 // 诊断：打印队列状态
                 {
                     static int frameCount = 0;
                     if (frameCount < 5) {
                         char msg[256];
                         size_t stdCount = 0, instCount = 0;
                         D3D12_GPU_DESCRIPTOR_HANDLE firstStdSRV = {}, firstInstSRV = {};
                         for (const auto &item : m_opaqueQueueStandard) {
                             if (item.IsValid()) {
                                 stdCount++;
                                 if (firstStdSRV.ptr == 0)
                                     firstStdSRV = item.textureSRV;
                             }
                         }
                         for (const auto &item : m_opaqueQueueInstanced) {
                             if (item.IsValid()) {
                                 instCount++;
                                 if (firstInstSRV.ptr == 0)
                                     firstInstSRV = item.textureSRV;
                             }
                         }
                         sprintf_s(msg,
                                   "[CubeRender] Frame %d: Standard=%zu items (firstSRV=%llx), Instanced=%zu items "
                                   "(firstSRV=%llx)\n",
                                   frameCount, stdCount, (unsigned long long)firstStdSRV.ptr, instCount,
                                   (unsigned long long)firstInstSRV.ptr);
                         OutputDebugStringA(msg);
                         frameCount++;
                     }
                 }

                 // ================================================================
                 // Phase 1: 绑定 Standard PSO，绘制所有 Standard 物体
                 // ================================================================
                 for (const auto &item : m_opaqueQueueStandard) {
                     if (!item.IsValid())
                         continue;
                     m_renderer->DrawMesh(cmdList, item.geometryHandle, DirectX::XMMatrixIdentity(),
                                          item.standard.constantBuffer, item.textureSRV, envMapSRV);
                 }

                 // ================================================================
                 // Phase 2: 绑定 Instanced PSO，绘制所有 Instanced 批次
                 // ================================================================
                 for (const auto &item : m_opaqueQueueInstanced) {
                     if (!item.IsValid())
                         continue;
                     m_renderer->DrawInstanced(cmdList, item.geometryHandle, item.instanced.instanceBuffer,
                                               item.instanced.instanceCount, item.textureSRV, envMapSRV);
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
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Opaque,
         .alwaysRun = true});
}

void GameWorld::RegisterSkyboxSystem() {
    SystemRegistry::Register(
        {.name = "SkyboxRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 if (!m_skyRenderer || !m_skyboxGeometryHandle.IsValid()) {
                     return;
                 }

                 // 获取命令列表等渲染资源
                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 // ====================================================================
                 // 1. 获取资源
                 // ====================================================================
                 auto backBuffer = m_context->GetBackBuffer();
                 auto rtvHandle = m_context->DeviceContext->GetCurrentBackBufferView();
                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();
                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();

                 D3D12_GPU_DESCRIPTOR_HANDLE skySRV = {0};
                 if (m_skyboxTextureHandle.IsValid()) {
                     skySRV = m_context->TextureMgr->GetSRV(m_skyboxTextureHandle);
                 }

                 // ====================================================================
                 // 2. 资源屏障：确保 BackBuffer 处于 RENDER_TARGET 状态
                 // ====================================================================
                 D3D12_RESOURCE_BARRIER barrier = {};
                 barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 barrier.Transition.pResource = backBuffer;
                 barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                 barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 // ====================================================================
                 // 3. 设置渲染目标、视口、裁剪矩形
                 // ====================================================================
                 const auto &viewport = m_context->DeviceContext->GetViewport();
                 const auto &scissorRect = m_context->DeviceContext->GetScissorRect();
                 cmdList.Get()->RSSetViewports(1, &viewport);
                 cmdList.Get()->RSSetScissorRects(1, &scissorRect);
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 // ====================================================================
                 // 4. 设置描述符堆
                 // ====================================================================
                 ID3D12DescriptorHeap *descriptorHeaps[] = {
                     m_context->DescriptorHeaps->GetHeap(DescriptorHeapType::CbvSrvUav)};
                 cmdList.Get()->SetDescriptorHeaps(1, descriptorHeaps);

                 // ====================================================================
                 // 5. 渲染天空盒
                 // ====================================================================
                 m_skyRenderer->BeginFrame(cmdList, passCBAddr, skySRV);
                 m_skyRenderer->DrawSky(cmdList, m_skyboxGeometryHandle, m_skyboxObjectCBAddress);
                 m_skyRenderer->EndFrame();

                 // ====================================================================
                 // 6. 屏障：转换回 PRESENT 状态（供下一阶段或 Present 使用）
                 // ====================================================================
                 barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::PostProcess, cmdListHandle);

                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::PostProcess,
         .alwaysRun = true});
}

void GameWorld::CreateWater() {
    if (!m_registry || !m_renderer || !m_context)
        return;

    // 1. 生成水面几何体（20x20 平面，64x64 细分）
    GeometryGenerator geoGen;
    auto meshData = geoGen.CreateGrid(256.0f, 256.0f, 64, 64); // width, depth, m, n

    // 2. 创建 GPU 资源（与 CreateTestCube 相同流程）
    auto &gpuMgr = GpuResourceManager::GetInstance();
    auto device = m_context->DeviceContext->GetDevice();

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

    // 3. 构建 TriangleMesh（水面使用三角形网格）
    TriangleMesh waterMesh;
    waterMesh.vertexBufferHandle = vbHandle;
    waterMesh.indexBufferHandle = ibHandle;
    waterMesh.vertexCount = static_cast<uint32_t>(meshData.Vertices.size());
    waterMesh.indexCount = static_cast<uint32_t>(meshData.Indices32.size());
    waterMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
    waterMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    waterMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    waterMesh.isGpuReady = true;

    // 计算包围盒
    BoundingAABB bounds;
    bounds.min = XMFLOAT3(-10.0f, 0.0f, -10.0f);
    bounds.max = XMFLOAT3(10.0f, 0.0f, 10.0f);

    auto &geoMgr = m_context->GeometryResourceManager;
    GeometryHandle geoHandle = geoMgr->RegisterTriangleMesh(waterMesh);

    // 4. 创建 LODMesh
    LODMesh lodMesh;
    lodMesh.lodChain = {geoHandle};
    LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

    // 5. 创建实体
    m_waterEntity = m_registry->CreateEntity();

    XMFLOAT3 position(0.0f, 0.0f, 0.0f); // 在地形高度范围内
    XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
    XMFLOAT3 scale(1.0f, 1.0f, 1.0f);
    m_registry->AddComponent<TransformComponent>(m_waterEntity, position, rotation, scale);

    // 使用 TransparentMeshComponent
    TransparentMeshComponent meshComp;
    meshComp.lodMeshHandle = lodHandle;
    meshComp.localBounds = bounds;
    meshComp.materialHandle = m_waterMaterialHandle;
    meshComp.textureHandle = m_waterTextureHandle;
    m_registry->AddComponent<TransparentMeshComponent>(m_waterEntity, std::move(meshComp));

    // 绘制调用
    RegisterWaterRenderSystem();
}

void GameWorld::RegisterWaterRenderSystem() {
    SystemRegistry::Register(
        {.name = "WaterRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 if (m_transparentQueue.Empty()) {
                     return;
                 }

                 // 获取命令列表等渲染资源
                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 // 屏障：确保 BackBuffer 处于 RENDER_TARGET 状态
                 auto backBuffer = m_context->GetBackBuffer();
                 auto rtvHandle = m_context->DeviceContext->GetCurrentBackBufferView();
                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();

                 D3D12_RESOURCE_BARRIER barrier = {};
                 barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 barrier.Transition.pResource = backBuffer;
                 barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                 barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 // 设置视口、裁剪矩形、渲染目标
                 const auto &viewport = m_context->DeviceContext->GetViewport();
                 const auto &scissorRect = m_context->DeviceContext->GetScissorRect();
                 cmdList.Get()->RSSetViewports(1, &viewport);
                 cmdList.Get()->RSSetScissorRects(1, &scissorRect);
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 // 设置描述符堆
                 ID3D12DescriptorHeap *descriptorHeaps[] = {
                     m_context->DescriptorHeaps->GetHeap(DescriptorHeapType::CbvSrvUav)};
                 cmdList.Get()->SetDescriptorHeaps(1, descriptorHeaps);

                 // 获取 Pass Constant Buffer 地址
                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 D3D12_GPU_VIRTUAL_ADDRESS lightCBAddr = LightManager::GetInstance().GetLightCBAddress();
                 D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV = m_context->MaterialMgr->GetMaterialBufferSRV();

                 // 开始渲染
                 m_waterRenderer->BeginFrame(cmdList, passCBAddr, lightCBAddr, materialBufferSRV, m_waterCBAddress);

                 // 遍历透明物体队列（使用新的 TRenderQueue<TransparentRenderItem>）
                 for (const auto &item : m_transparentQueue.GetItems()) {
                     if (!item.IsValid())
                         continue;
                     D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV = {};
                     if (m_skyboxTextureHandle.IsValid()) {
                         envMapSRV = m_context->TextureMgr->GetSRV(m_skyboxTextureHandle);
                     }
                     m_waterRenderer->DrawWater(cmdList, item.geometryHandle, item.worldMatrix, item.objectCBAddress,
                                                item.textureSRV, envMapSRV);
                 }

                 m_waterRenderer->EndFrame();

                 // 屏障：转换回 PRESENT 状态
                 barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Transparent, cmdListHandle);

                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Transparent,
         .alwaysRun = true});
}

void GameWorld::LoadWaterTexture() {
    DDSTextureInfo ddsInfo;
    std::wstring texturePath = L"Content/Textures/water1.dds";

    if (!AssetLoader::GetInstance().LoadTextureFromFile(texturePath, ddsInfo)) {
        m_context->Logging->Error("[GameWorld] Failed to load water texture");
        // 使用测试纹理作为 fallback
        m_waterTextureHandle = m_testTextureHandle;
        return;
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Device *device = m_context->DeviceContext->GetDevice();
    GpuResourceHandle gpuHandle = gpuMgr.CreateTexture2D(device, ddsInfo.desc, D3D12_RESOURCE_STATE_COMMON);

    if (!gpuHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] Failed to create water GPU texture");
        m_waterTextureHandle = m_testTextureHandle;
        return;
    }

    auto &descriptorHeaps = m_context->DescriptorHeaps;
    uint32_t srvIndex = descriptorHeaps->Allocate(DescriptorHeapType::CbvSrvUav);

    if (srvIndex == UINT32_MAX) {
        m_context->Logging->Error("[GameWorld] Failed to allocate SRV for water texture");
        gpuMgr.Release(gpuHandle, 0);
        m_waterTextureHandle = m_testTextureHandle;
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
    m_waterTextureHandle = texMgr->RegisterTexture(gpuHandle, srvIndex);

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

    m_context->Logging->Info("[GameWorld] Water texture loaded successfully");
}

// ========================================================================
// 异步地形加载（BackgroundExecutor + 消息驱动 System）
// ========================================================================
// 数据流（新架构）：
//   LoadTerrainAsync() → BackgroundExecutor::Submit(TerrainLoadTask)
//     后台线程: CPU加载 → 创建VB/IB (UPLOAD堆) → 创建纹理(DEFAULT堆)
//              → 录制 COPY/DIRECT 命令 → 构造 GpuWorkItem → RegisterGpuWork
//
//   BackgroundExecutor::Tick() (主线程):
//     → 收集 GpuWorkItem → Submit COPY → Signal → Submit DIRECT(Wait) → Signal
//     → Wait DIRECT → onComplete 回调 → PostEvent(TerrainLoaded)
//
//   TerrainGPUCreateSystem (主线程，响应 TerrainLoaded):
//     分配 SRV 描述符 → CreateShaderResourceView → 从 TerrainReadyState 读取 GpuResourceHandle
//     → 注册 GeometryHandle/TextureHandle → PostEvent(TerrainReady)
//
//   TerrainCombineSystem (主线程，响应 TerrainReady):
//     注册 LODMesh → 创建 ECS 实体
//
// 关键设计：
//   - BackgroundExecutor 承担类似帧驱动器的角色，统一提交 GPU 命令
//   - 后台线程只录制命令，不 Submit
//   - 主线程通过 Tick() 统一处理所有后台 GPU 工作的提交
// ========================================================================
void GameWorld::LoadTerrainAsync() {
    if (!m_context || !m_registry || !m_backgroundExecutor) {
        m_context->Logging->Error("[LoadTerrainAsync] Invalid state: context={}, registry={}, executor={}",
                                  static_cast<const void *>(m_context), static_cast<const void *>(m_registry),
                                  static_cast<const void *>(m_backgroundExecutor.get()));
        return;
    }

    static std::atomic<uint32_t> s_nextRequestId{1};
    uint32_t requestId = s_nextRequestId++;
    m_terrainRequestId = requestId;

    m_context->Logging->Info("[LoadTerrainAsync] Starting async terrain loading (request={})...", requestId);

    // 创建 TerrainReadyState（后台线程写入 GPU 资源，主线程读取后注册句柄）
    m_terrainReadyState = std::make_shared<Async::TerrainReadyState>();

    // 构建后台任务输入
    Async::TerrainLoadTaskFactory::Input input;
    input.device = m_context->DeviceContext->GetDevice();
    input.cmdMgr = &m_context->DeviceContext->GetCommandManager();
    input.descriptorHeaps = m_context->DescriptorHeaps;
    input.readyState = m_terrainReadyState;
    input.backgroundExecutor = m_backgroundExecutor.get();

    // 创建后台任务（CPU 加载 + GPU 资源创建 + 上传录制 + 注册 GpuWorkItem）
    // 此时并没有执行任何 GPU 操作，只是创建了任务和数据结构
    Async::TerrainLoadDataPtr terrainData;
    auto loadTask = Async::TerrainLoadTaskFactory::Create(requestId, L"Content/Terrain/heightmap.png", 256.0f, 256.0f,
                                                          20.0f, 257, terrainData, input);
    m_terrainLoadData = terrainData; // 传递指针

    // 提交到 BackgroundExecutor
    // 后台线程会执行任务，创建 GPU 资源（VB/IB/纹理）+ 录制命令 → RegisterGpuWork → 任务结束
    m_backgroundExecutor->Submit(std::move(loadTask));

    m_context->Logging->Info("[LoadTerrainAsync] Task submitted to BackgroundExecutor (pending={}, total={})",
                             m_backgroundExecutor->GetPendingCount(), m_backgroundExecutor->GetTotalSubmitted());
}

// ========================================================================
// 地形异步加载响应 System
//
// 两步消息驱动：TerrainLoaded → TerrainReady
//
// 架构设计（新架构）：
//   - 后台线程: 创建 GPU 资源（VB/IB/纹理）+ 录制命令 → RegisterGpuWork → 任务结束
//   - BackgroundExecutor::Tick() (主线程): Submit COPY → Signal → Submit DIRECT → Wait → onComplete
//   - onComplete (主线程): PostEvent(TerrainLoaded)
//   - TerrainGPUCreateSystem (主线程): 分配 SRV + CreateSRV + 注册句柄 → PostEvent(TerrainReady)
//   - TerrainCombineSystem (主线程): 注册 LODMesh → 创建 ECS 实体
//
// 关键：BackgroundExecutor 承担类似帧驱动器的角色，统一提交 GPU 命令
//       System 不再处理任何 Submit/Wait/Fence 逻辑
// ========================================================================
void GameWorld::RegisterTerrainSystems() {
    // ---------------------------------------------------------------
    // System A: TerrainGPUCreateSystem (主线程)
    // 响应 TerrainLoaded → 分配 SRV + 创建 SRV + 注册句柄 → PostEvent(TerrainReady)
    //
    // 此时 BackgroundExecutor 已完成：
    //   - COPY 队列上传纹理数据
    //   - DIRECT 队列 ResourceTransition (COMMON → PIXEL_SHADER_RESOURCE)
    //   - GPU fence 已等待完成
    //
    // 主线程需要：
    //   - 分配 SRV 描述符索引 (DescriptorHeapCollection::Allocate)
    //   - CreateShaderResourceView
    //   - 注册 GeometryHandle / TextureHandle
    // ---------------------------------------------------------------
    SystemRegistry::Register(
        {.name = "TerrainGPUCreateSystem",
         .func =
             [this](Registry &reg, const MessageContext &ctx) {
                 uint32_t requestId = static_cast<uint32_t>(ctx.payload >> 32);
                 m_context->Logging->Info("[TerrainGPUCreate] Triggered by TerrainLoaded (request={})", requestId);

                 if (!m_terrainReadyState) {
                     m_context->Logging->Error("[TerrainGPUCreate] No TerrainReadyState (request={})", requestId);
                     return;
                 }

                 auto &state = *m_terrainReadyState;

                 // ── 验证几何体 GPU 资源已创建 ──
                 if (!state.geometryCreated.load(std::memory_order_acquire)) {
                     m_context->Logging->Error("[TerrainGPUCreate] Geometry not created yet (request={})", requestId);
                     return;
                 }

                 if (!state.vbHandle.IsValid() || !state.ibHandle.IsValid()) {
                     m_context->Logging->Error("[TerrainGPUCreate] Invalid VB/IB handles (request={})", requestId);
                     return;
                 }

                 // ── 注册 GeometryHandle（主线程，非线程安全 API）──
                 Resource::TriangleMesh mesh;
                 mesh.vertexBufferHandle = state.vbHandle;
                 mesh.indexBufferHandle = state.ibHandle;
                 mesh.vertexCount = static_cast<uint32_t>(m_terrainLoadData->vertices.size());
                 mesh.indexCount = static_cast<uint32_t>(m_terrainLoadData->indices.size());
                 mesh.vertexStride = sizeof(GeometryGenerator::Vertex);
                 mesh.indexFormat = DXGI_FORMAT_R32_UINT;
                 mesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                 mesh.isGpuReady = true;
                 mesh.localBounds = state.bounds;

                 auto geoMgr = m_context->GeometryResourceManager;
                 auto handle = geoMgr->RegisterTriangleMesh(mesh);
                 m_terrainGeometryHandle = handle;
                 m_context->Logging->Info("[TerrainGPUCreate] Geometry registered: handle(idx={}, gen={})",
                                          handle.index, handle.generation);

                 // ── 处理纹理：分配 SRV + 创建 SRV + 注册 TextureHandle ──
                 // BackgroundExecutor 已保证 GPU 工作完成（COPY + DIRECT + fence wait）
                 // textureCreated 由 onComplete 回调设置
                 if (!m_terrainTextureHandle.IsValid() && state.textureCreated.load(std::memory_order_acquire)) {
                     if (state.textureGpuHandle.IsValid()) {
                         // 分配 SRV 描述符（主线程，非线程安全 API）
                         auto &descriptorHeaps = m_context->DescriptorHeaps;
                         uint32_t srvIndex = descriptorHeaps->Allocate(Resource::DescriptorHeapType::CbvSrvUav);
                         if (srvIndex != UINT32_MAX) {
                             auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

                             // 创建 SRV（主线程），使用 DDS 解析出的完整纹理描述
                             D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                             srvDesc.Format = state.textureDesc.Format;
                             srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                             srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                             srvDesc.Texture2D.MostDetailedMip = 0;
                             srvDesc.Texture2D.MipLevels = state.textureDesc.MipLevels;

                             D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
                                 descriptorHeaps->GetCpuHandle(Resource::DescriptorHeapType::CbvSrvUav, srvIndex);
                             ID3D12Device *device = m_context->DeviceContext->GetDevice();
                             device->CreateShaderResourceView(gpuMgr.GetResource(state.textureGpuHandle), &srvDesc,
                                                              cpuHandle);

                             // 注册 TextureHandle（主线程，非线程安全 API）
                             auto *texMgr = m_context->TextureMgr;
                             m_terrainTextureHandle = texMgr->RegisterTexture(state.textureGpuHandle, srvIndex);

                             m_context->Logging->Info(
                                 "[TerrainGPUCreate] Texture registered: handle(idx={}, gen={}), SRV idx={}",
                                 m_terrainTextureHandle.index, m_terrainTextureHandle.generation, srvIndex);
                         } else {
                             m_context->Logging->Error("[TerrainGPUCreate] Failed to allocate SRV descriptor");
                         }
                     } else {
                         m_context->Logging->Warn("[TerrainGPUCreate] Texture GPU handle invalid, skipping");
                     }
                 } else if (m_terrainTextureHandle.IsValid()) {
                     m_context->Logging->Info("[TerrainGPUCreate] Terrain texture already loaded, skipping");
                 }

                 // ── 发送 TerrainReady 事件 ──
                 uint64_t payload = Event::MakeAssetLoadedPayload(requestId, handle.index, handle.generation);
                 bool posted = Event::MessageDispatcher::GetInstance()->PostEvent(
                     static_cast<uint32_t>(Event::EventType::TerrainReadyEvent), 0, payload,
                     Event::EventPriority::P2_Normal);
                 m_context->Logging->Info("[TerrainGPUCreate] PostEvent TerrainReady: posted={} (request={})", posted,
                                          requestId);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Main,
         .priority = TaskPriority::Normal,
         .interestedMessages = {static_cast<uint32_t>(Event::EventType::TerrainLoadedEvent)}});

    // ---------------------------------------------------------------
    // System B: TerrainCombineSystem (主线程)
    // 响应 TerrainReady → 注册 LODMesh → 创建 ECS 实体
    // ---------------------------------------------------------------
    SystemRegistry::Register(
        {.name = "TerrainCombineSystem",
         .func =
             [this](Registry &reg, const MessageContext &ctx) {
                 uint32_t requestId = 0, handleIdx = 0, handleGen = 0;
                 Event::DecodeAssetLoadedPayload(ctx.payload, requestId, handleIdx, handleGen);

                 m_context->Logging->Info(
                     "[TerrainCombine] Triggered by TerrainReady (request={}, handleIdx={}, handleGen={})", requestId,
                     handleIdx, handleGen);

                 // 注册 LODMesh
                 Resource::LODMesh lodMesh;
                 lodMesh.lodChain = {m_terrainGeometryHandle};
                 Resource::LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);
                 m_context->Logging->Info("[TerrainCombine] LODMesh registered: lodHandle idx={}", lodHandle.index);

                 // 创建 ECS 实体
                 auto entity = reg.CreateEntity();
                 m_terrainEntity = entity;

                 XMFLOAT3 position(0.0f, -10.0f, 0.0f);
                 XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
                 XMFLOAT3 scale(1.0f, 1.0f, 1.0f);
                 reg.AddComponent<ECS::TransformComponent>(entity, position, rotation, scale);

                 // 包围盒从 TerrainLoadData 或 TerrainReadyState 获取
                 Math::BoundingAABB bounds;
                 if (m_terrainLoadData) {
                     bounds = m_terrainLoadData->bounds;
                 } else if (m_terrainReadyState) {
                     bounds = m_terrainReadyState->bounds;
                 }

                 ECS::MeshComponent meshComp;
                 meshComp.lodMeshHandle = lodHandle;
                 meshComp.localBounds = bounds;
                 meshComp.materialHandle = m_terrainMaterialHandle;
                 meshComp.textureHandle = m_terrainTextureHandle;
                 reg.AddComponent<ECS::MeshComponent>(entity, std::move(meshComp));

                 m_context->Logging->Info("[TerrainCombine] Entity created: entity={}, request={}, geoHandle={}, "
                                          "texHandle={}, matHandle={}, lodHandle={}",
                                          static_cast<uint32_t>(entity), requestId, m_terrainGeometryHandle.index,
                                          m_terrainTextureHandle.index, m_terrainMaterialHandle.index, lodHandle.index);

                 // 清理状态（句柄已保存到成员变量，不再需要 readyState）
                 m_terrainReadyState.reset();
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Main,
         .priority = TaskPriority::Normal,
         .interestedMessages = {static_cast<uint32_t>(Event::EventType::TerrainReadyEvent)}});
}

void GameWorld::RegisterShadowRenderSystem() {
    SystemRegistry::Register(
        {.name = "ShadowRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 auto &lightMgr = LightManager::GetInstance();
                 D3D12_GPU_VIRTUAL_ADDRESS dirShadowAddr = lightMgr.GetDirShadowAddress();
                 const auto &shadowRes = lightMgr.GetDirShadowResources();

                 // 检查方向光阴影是否可用
                 if (!lightMgr.HasDirShadow() || !shadowRes.isValid || dirShadowAddr == 0 ||
                     (m_opaqueQueueStandard.Empty() && m_opaqueQueueInstanced.Empty())) {
                     return;
                 }

                 // ================================================================
                 // 获取命令列表
                 // ================================================================
                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 // ================================================================
                 // 获取阴影纹理资源
                 // ================================================================
                 auto &gpuMgr = GpuResourceManager::GetInstance();
                 ID3D12Resource *depthTexture = gpuMgr.GetResource(shadowRes.textureHandle);
                 if (!depthTexture) {
                     m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);
                     m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle,
                                                                                 m_context->GetNextSequence());
                     return;
                 }

                 D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
                     m_context->DescriptorHeaps->GetCpuHandle(DescriptorHeapType::Dsv, shadowRes.dsvSlot);

                 // ================================================================
                 // 资源状态转换：SRV -> DEPTH_WRITE
                 // ================================================================
                 CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                     depthTexture,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                     D3D12_RESOURCE_STATE_DEPTH_WRITE);
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 // ================================================================
                 // 开始阴影 Pass
                 // ================================================================
                 //  DSV被设置为渲染目标，深度信息会被写入shadowRes
                 m_shadowRenderer->Begin(cmdList, dirShadowAddr, dsvHandle, shadowRes.resolution, shadowRes.resolution);

                 // ================================================================
                 // 遍历 Standard 队列中的物体，绘制阴影
                 // ================================================================
                 for (const auto &item : m_opaqueQueueStandard) {
                     if (!item.IsValid())
                         continue;

                     m_shadowRenderer->DrawMesh(cmdList, item.geometryHandle, DirectX::XMMatrixIdentity(),
                                                item.standard.constantBuffer);
                 }

                 // ================================================================
                 // 遍历 Instanced 队列中的物体，绘制阴影
                 // ================================================================
                 for (const auto &item : m_opaqueQueueInstanced) {
                     if (!item.IsValid())
                         continue;

                     m_shadowRenderer->DrawInstanced(cmdList, item.geometryHandle, item.instanced.instanceBuffer,
                                                     item.instanced.instanceCount);
                 }

                 // ================================================================
                 // 结束阴影 Pass
                 // ================================================================
                 m_shadowRenderer->End(cmdList);

                 // ================================================================
                 // 资源状态转换：DEPTH_WRITE -> SRV
                 // ================================================================
                 CD3DX12_RESOURCE_BARRIER barrierBack = CD3DX12_RESOURCE_BARRIER::Transition(
                     depthTexture, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                 cmdList.Get()->ResourceBarrier(1, &barrierBack);

                 // ================================================================
                 // 提交命令
                 // ================================================================
                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::PrePass, cmdListHandle);

                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::PrePass,
         .alwaysRun = true});
}

void GameWorld::Update() {
    if (m_backgroundExecutor) {
        m_backgroundExecutor->Tick();
    }
}