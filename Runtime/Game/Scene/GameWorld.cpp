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
#include "Renderer/Pipeline/SkyRenderer.h"
#include "Renderer/Pipeline/WaterRenderer.h"

#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/LightManager.h"
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
#include "Async/AsyncLoadHelpers.h"
#include "Async/BackgroundExecutor.h"
#include "Async/CombineAssetsTask.h"
#include "Async/TerrainGPUCreateTask.h"
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

    LoadTestTexture();
    LoadWaterTexture();

    CreateMaterials();
    CreateSkybox();
    CreateTestCube();

    // 创建后台异步执行器（纯 CPU 线程池，2 个工作线程）
    // 必须在 LoadTerrainAsync() 之前创建！
    m_backgroundExecutor = std::make_unique<BackgroundExecutor>(2);

    // 注册地形异步加载响应 System（消息驱动：BackgroundExecutor 完成 → PostEvent → System 执行）
    RegisterTerrainSystems();

    // 【异步加载尝试】注释掉阻塞调用，使用 BackgroundExecutor 异步加载地形
    // CreateTerrain();
    LoadTerrainAsync();

    CreateWater();

    // 注册水常量立即回调（每帧上传水波动画数据）
    RegisterWaterConstantsCallback();
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

void GameWorld::BuildRenderQueue() {
    if (!m_registry)
        return;

    // 设置每帧数据
    m_opaqueBuilder->SetCullingResult(&m_context->cullingResult);
    m_opaqueBuilder->SetLODResult(&m_context->lodResult);

    m_transparentBuilder->SetCullingResult(&m_context->cullingResult);
    m_transparentBuilder->SetLODResult(&m_context->lodResult);

    // 构建不透明渲染队列
    m_opaqueBuilder->BuildTyped(*m_registry, m_opaqueQueue);

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
                 if (m_opaqueQueue.Empty()) {
                     m_context->Logging->Debug("[CubeRender] Opaque queue is empty, skipping draw");
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

                 // 使用 GameWorld 自己的队列
                 for (const auto &item : m_opaqueQueue) {
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
    float width = 256.0f;    // 地形宽度（X 轴）
    float depth = 256.0f;    // 地形深度（Z 轴）
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

                 //  每二十帧输出一次
                 static int frameCount = 0;
                 frameCount++;
                 if (frameCount % 20 == 0) {
                     auto &transform = registry.GetComponent<TransformComponent>(m_waterEntity);
                     std::wstring debugStr = std::wstring(L"Water position: ") + std::to_wstring(transform.position.x) +
                                             L", " + std::to_wstring(transform.position.y) + L", " +
                                             std::to_wstring(transform.position.z) + L"\n";
                     OutputDebugStringW(debugStr.c_str());
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
// 三步数据流：
//   Step 1: 用户请求 LoadTerrainAsync() → BackgroundExecutor::Submit(TerrainLoadTask)
//   Step 2: 后台线程完成 I/O → PostEvent(TerrainLoaded)
//           TerrainGPUCreateSystem 响应 → 创建 VB/IB + 上传纹理 → PostEvent(TerrainGeometryUploaded)
//   Step 3: TerrainUploadCompletionSystem 响应 → PostEvent(TerrainReady)
//           TerrainCombineSystem 响应 → 注册 LODMesh → 创建 ECS 实体
// ========================================================================
void GameWorld::LoadTerrainAsync() {
    if (!m_context || !m_registry || !m_backgroundExecutor) {
        m_context->Logging->Error("[LoadTerrainAsync] Invalid state: context=%p, registry=%p, executor=%p",
                                  static_cast<const void *>(m_context), static_cast<const void *>(m_registry),
                                  static_cast<const void *>(m_backgroundExecutor.get()));
        return;
    }

    static std::atomic<uint32_t> s_nextRequestId{1};
    uint32_t requestId = s_nextRequestId++;

    m_context->Logging->Info("[LoadTerrainAsync] Starting async terrain loading (request=%u)...", requestId);

    // 创建 terrainData，用于在 GPU 创建 System 中访问
    Async::TerrainLoadDataPtr terrainData;
    auto loadTask = Async::TerrainLoadTaskFactory::Create(requestId, L"Content/Terrain/heightmap.png", 256.0f, 256.0f,
                                                          20.0f, 257, terrainData);

    // 存储 terrainData 到 AssetDataManager（线程安全），供消息驱动的 System 读取
    std::string dataKey = "TerrainLoadData_" + std::to_string(requestId);
    Resource::AssetDataManager::GetInstance().StoreTypedData(dataKey, terrainData);

    m_context->Logging->Info("[LoadTerrainAsync] Data stored in AssetDataManager, terrainData=%p, key=%s (request=%u)",
                             static_cast<const void *>(terrainData.get()), dataKey.c_str(), requestId);

    // 只提交 CPU 加载任务到 BackgroundExecutor
    m_backgroundExecutor->Submit(std::move(loadTask));

    m_context->Logging->Info("[LoadTerrainAsync] Task submitted to BackgroundExecutor (pending=%zu, total=%zu)",
                             m_backgroundExecutor->GetPendingCount(), m_backgroundExecutor->GetTotalSubmitted());
}

// ========================================================================
// 地形异步加载响应 System
// 三步消息驱动：TerrainLoaded → TerrainGeometryUploaded → TerrainReady
// ========================================================================
void GameWorld::RegisterTerrainSystems() {
    // ---------------------------------------------------------------
    // System A: TerrainGPUCreateSystem
    // 响应 TerrainLoaded 事件 → 在 Render 线程创建 VB/IB + 上传纹理
    // ---------------------------------------------------------------
    SystemRegistry::Register(
        {.name = "TerrainGPUCreateSystem",
         .func =
             [this](Registry &reg, const MessageContext &ctx) {
                 uint32_t requestId = static_cast<uint32_t>(ctx.payload >> 32);
                 m_context->Logging->Info("[TerrainGPUCreate] Triggered by TerrainLoaded (request=%u)", requestId);

                 // 从 AssetDataManager 查找对应的 TerrainLoadData（线程安全）
                 std::string dataKey = "TerrainLoadData_" + std::to_string(requestId);
                 auto &assetMgr = Resource::AssetDataManager::GetInstance();
                 Async::TerrainLoadDataPtr data = assetMgr.GetTypedData<Async::TerrainLoadData>(dataKey);

                 if (!data || data->vertices.empty()) {
                     m_context->Logging->Error("[TerrainGPUCreate] No terrain data found in AssetDataManager "
                                               "(request=%u, key=%s, data=%p, vtx=%zu)",
                                               requestId, dataKey.c_str(), static_cast<const void *>(data.get()),
                                               data ? data->vertices.size() : 0);
                     return;
                 }

                 m_context->Logging->Info("[TerrainGPUCreate] Creating VB/IB: %zu vertices, %zu indices (request=%u)",
                                          data->vertices.size(), data->indices.size(), requestId);

                 ID3D12Device *device = m_context->DeviceContext->GetDevice();
                 auto *geoMgr = m_context->GeometryResourceManager;
                 auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

                 // 创建 VB
                 size_t vbSize = data->vertices.size() * sizeof(GeometryGenerator::Vertex);
                 auto vbHandle =
                     gpuMgr.CreateBuffer(device, vbSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                 if (auto *vbRes = gpuMgr.GetResource(vbHandle)) {
                     void *mapped = nullptr;
                     vbRes->Map(0, nullptr, &mapped);
                     memcpy(mapped, data->vertices.data(), vbSize);
                     vbRes->Unmap(0, nullptr);
                     m_context->Logging->Info("[TerrainGPUCreate] VB created: handle=%u, size=%zu bytes",
                                              vbHandle.index, vbSize);
                 } else {
                     m_context->Logging->Error("[TerrainGPUCreate] Failed to create VB!");
                     return;
                 }

                 // 创建 IB
                 size_t ibSize = data->indices.size() * sizeof(uint32_t);
                 auto ibHandle =
                     gpuMgr.CreateBuffer(device, ibSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                 if (auto *ibRes = gpuMgr.GetResource(ibHandle)) {
                     void *mapped = nullptr;
                     ibRes->Map(0, nullptr, &mapped);
                     memcpy(mapped, data->indices.data(), ibSize);
                     ibRes->Unmap(0, nullptr);
                     m_context->Logging->Info("[TerrainGPUCreate] IB created: handle=%u, size=%zu bytes",
                                              ibHandle.index, ibSize);
                 } else {
                     m_context->Logging->Error("[TerrainGPUCreate] Failed to create IB!");
                     return;
                 }

                 // 注册到 GeometryResourceManager
                 Resource::TriangleMesh mesh;
                 mesh.vertexBufferHandle = vbHandle;
                 mesh.indexBufferHandle = ibHandle;
                 mesh.vertexCount = static_cast<uint32_t>(data->vertices.size());
                 mesh.indexCount = static_cast<uint32_t>(data->indices.size());
                 mesh.vertexStride = sizeof(GeometryGenerator::Vertex);
                 mesh.indexFormat = DXGI_FORMAT_R32_UINT;
                 mesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                 mesh.isGpuReady = true;
                 mesh.localBounds = data->bounds;

                 auto handle = geoMgr->RegisterTriangleMesh(mesh);
                 m_terrainGeometryHandle = handle;
                 m_context->Logging->Info("[TerrainGPUCreate] Geometry registered: handle(idx=%u, gen=%u)",
                                          handle.index, handle.generation);

                 m_context->Logging->Info(
                     "[TerrainGPUCreate] Checking terrain texture handle: valid={}, index={}, generation={}",
                     m_terrainTextureHandle.IsValid(), m_terrainTextureHandle.index, m_terrainTextureHandle.generation);

                 // ---- 上传地形纹理 (如果尚未上传) ----
                 if (!m_terrainTextureHandle.IsValid()) {
                     m_context->Logging->Info("[TerrainGPUCreate] Uploading terrain texture...");
                     std::wstring texturePath = L"Content/Textures/heightmap.dds";
                     DDSTextureInfo ddsInfo;
                     if (AssetLoader::GetInstance().LoadTextureFromFile(texturePath, ddsInfo)) {
                         GpuResourceHandle gpuHandle =
                             gpuMgr.CreateTexture2D(device, ddsInfo.desc, D3D12_RESOURCE_STATE_COMMON);
                         if (gpuHandle.IsValid()) {
                             auto &descriptorHeaps = m_context->DescriptorHeaps;
                             uint32_t srvIndex = descriptorHeaps->Allocate(DescriptorHeapType::CbvSrvUav);
                             if (srvIndex != UINT32_MAX) {
                                 D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                                 srvDesc.Format = ddsInfo.desc.Format;
                                 srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                                 srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                                 srvDesc.Texture2D.MipLevels = ddsInfo.desc.MipLevels;
                                 srvDesc.Texture2D.MostDetailedMip = 0;

                                 D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
                                     descriptorHeaps->GetCpuHandle(DescriptorHeapType::CbvSrvUav, srvIndex);
                                 device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);

                                 TextureManager *texMgr = m_context->TextureMgr;
                                 m_terrainTextureHandle = texMgr->RegisterTexture(gpuHandle, srvIndex);

                                 // 上传纹理数据
                                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                                 auto allocatorHandle =
                                     m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                                 auto allocator =
                                     m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                                 auto cmdListHandle =
                                     m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                                 auto cmdList =
                                     m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                                 std::vector<D3D12_SUBRESOURCE_DATA> subresources = ddsInfo.subresources;
                                 UINT64 requiredSize = GetRequiredIntermediateSize(
                                     gpuMgr.GetResource(gpuHandle), 0, static_cast<UINT>(subresources.size()));
                                 GpuResourceHandle uploadHandle = gpuMgr.CreateBuffer(
                                     device, requiredSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

                                 auto barrier1 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle),
                                                                                      D3D12_RESOURCE_STATE_COMMON,
                                                                                      D3D12_RESOURCE_STATE_COPY_DEST);
                                 cmdList.Get()->ResourceBarrier(1, &barrier1);

                                 UpdateSubresources(cmdList.Get(), gpuMgr.GetResource(gpuHandle),
                                                    gpuMgr.GetResource(uploadHandle), 0, 0,
                                                    static_cast<UINT>(subresources.size()), subresources.data());

                                 auto barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(
                                     gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COPY_DEST,
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                                 cmdList.Get()->ResourceBarrier(1, &barrier2);

                                 cmdList.Close();
                                 m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                                      cmdList);
                                 m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);

                                 uint64_t sequence = m_context->GetNextSequence();
                                 gpuMgr.Release(uploadHandle, sequence);
                                 m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);
                                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);

                                 m_context->Logging->Info(
                                     "[TerrainGPUCreate] Terrain texture uploaded: SRV idx=%u, handle=%u", srvIndex,
                                     gpuHandle.index);
                             } else {
                                 m_context->Logging->Error(
                                     "[TerrainGPUCreate] Failed to allocate SRV for terrain texture");
                                 gpuMgr.Release(gpuHandle, 0);
                             }
                         } else {
                             m_context->Logging->Error("[TerrainGPUCreate] Failed to create GPU texture");
                         }
                     } else {
                         m_context->Logging->Error("[TerrainGPUCreate] Failed to load terrain texture file");
                     }
                 } else {
                     m_context->Logging->Info("[TerrainGPUCreate] Terrain texture already loaded, skipping");
                 }

                 // 发送 TerrainGeometryUploaded 事件（Step 3: GPU 上传完成）
                 uint64_t payload = (static_cast<uint64_t>(requestId) << 42) |
                                    (static_cast<uint64_t>(handle.generation) << 32) |
                                    (static_cast<uint64_t>(handle.index) & 0xFFFFFFFF);
                 bool posted = Event::MessageDispatcher::GetInstance()->PostEvent(
                     static_cast<uint32_t>(Event::EventType::TerrainGeometryUploadedEvent), 0, payload,
                     Event::EventPriority::P4_Background);
                 m_context->Logging->Info(
                     "[TerrainGPUCreate] PostEvent TerrainGeometryUploaded: posted=%s (request=%u, handle=%u)",
                     posted ? "true" : "false", requestId, handle.index);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .interestedMessages = {static_cast<uint32_t>(Event::EventType::TerrainLoadedEvent)}});

    // ---------------------------------------------------------------
    // System A2: UploadCompletionSystem (NEW)
    // 响应 TerrainGeometryUploaded → 轮询围栏 → Post TerrainReady
    // ---------------------------------------------------------------
    SystemRegistry::Register(
        {.name = "TerrainUploadCompletionSystem",
         .func =
             [this](Registry &reg, const MessageContext &ctx) {
                 uint32_t requestId = 0, handleIdx = 0, handleGen = 0;
                 Event::DecodeAssetLoadedPayload(ctx.payload, requestId, handleIdx, handleGen);

                 m_context->Logging->Info(
                     "[UploadCompletion] Received TerrainGeometryUploaded (request=%u, handleIdx=%u, handleGen=%u)",
                     requestId, handleIdx, handleGen);

                 // 验证几何体句柄
                 if (!m_terrainGeometryHandle.IsValid() || m_terrainGeometryHandle.index != handleIdx) {
                     m_context->Logging->Warn(
                         "[UploadCompletion] Geometry handle mismatch: expected idx=%u, got idx=%u",
                         m_terrainGeometryHandle.index, handleIdx);
                 }

                 // 在当前帧的 Render 阶段，围栏应该已经通过（因为 Flush 是同步等待的）
                 // 但如果需要异步检查，可以在这里轮询围栏值
                 uint64_t completedFence = m_context->GetCompletedFence();
                 m_context->Logging->Info("[UploadCompletion] Current completed fence: %llu", completedFence);

                 // 检查纹理是否已准备好
                 bool textureReady = m_terrainTextureHandle.IsValid();
                 m_context->Logging->Info("[UploadCompletion] Texture ready: %s, Geometry ready: %s",
                                          textureReady ? "true" : "false",
                                          m_terrainGeometryHandle.IsValid() ? "true" : "false");

                 if (!textureReady || !m_terrainGeometryHandle.IsValid()) {
                     m_context->Logging->Error("[UploadCompletion] Resources not ready, skipping TerrainReady post!");
                     return;
                 }

                 // 发送 TerrainReady 事件（Step 4: 资源就绪）
                 uint64_t payload = (static_cast<uint64_t>(requestId) << 42) |
                                    (static_cast<uint64_t>(m_terrainGeometryHandle.generation) << 32) |
                                    (static_cast<uint64_t>(m_terrainGeometryHandle.index) & 0xFFFFFFFF);
                 bool posted = Event::MessageDispatcher::GetInstance()->PostEvent(
                     static_cast<uint32_t>(Event::EventType::TerrainReadyEvent), 0, payload,
                     Event::EventPriority::P3_Low);
                 m_context->Logging->Info("[UploadCompletion] PostEvent TerrainReady: posted=%s (request=%u)",
                                          posted ? "true" : "false", requestId);
             },
         .phase = TaskPhase::Update,
         .threadType = ThreadType::Main,
         .priority = TaskPriority::Normal,
         .interestedMessages = {static_cast<uint32_t>(Event::EventType::TerrainGeometryUploadedEvent)}});

    // ---------------------------------------------------------------
    // System B: TerrainCombineSystem
    // 响应 TerrainReady 事件（Step 4: 围栏通过） → 在 Main 线程创建 ECS 实体
    // ---------------------------------------------------------------
    SystemRegistry::Register(
        {.name = "TerrainCombineSystem",
         .func =
             [this](Registry &reg, const MessageContext &ctx) {
                 uint32_t requestId = 0, handleIdx = 0, handleGen = 0;
                 Event::DecodeAssetLoadedPayload(ctx.payload, requestId, handleIdx, handleGen);

                 m_context->Logging->Info(
                     "[TerrainCombine] Triggered by TerrainReady (request=%u, handleIdx=%u, handleGen=%u)", requestId,
                     handleIdx, handleGen);

                 // 从 AssetDataManager 查找对应的 TerrainLoadData 以获取包围盒（用完清理）
                 std::string dataKey = "TerrainLoadData_" + std::to_string(requestId);
                 auto &assetMgr = Resource::AssetDataManager::GetInstance();
                 Async::TerrainLoadDataPtr data = assetMgr.GetTypedData<Async::TerrainLoadData>(dataKey);
                 if (data) {
                     assetMgr.RemoveTypedData(dataKey); // 清理，防止内存泄漏
                 }

                 // 注册 LODMesh（关键：OpaqueRenderItemBuilder 通过 LODResult 获取几何体句柄）
                 Resource::LODMesh lodMesh;
                 lodMesh.lodChain = {m_terrainGeometryHandle};
                 Resource::LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);
                 m_context->Logging->Info("[TerrainCombine] LODMesh registered: lodHandle idx=%u", lodHandle.index);

                 // 创建 ECS 实体
                 auto entity = reg.CreateEntity();
                 m_terrainEntity = entity;

                 XMFLOAT3 position(0.0f, -10.0f, 0.0f);
                 XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
                 XMFLOAT3 scale(1.0f, 1.0f, 1.0f);
                 reg.AddComponent<ECS::TransformComponent>(entity, position, rotation, scale);

                 Math::BoundingAABB bounds;
                 if (data) {
                     bounds.min = XMFLOAT3(-data->width * 0.5f, 0.0f, -data->depth * 0.5f);
                     bounds.max = XMFLOAT3(data->width * 0.5f, data->maxHeight, data->depth * 0.5f);
                 }

                 ECS::MeshComponent meshComp;
                 meshComp.lodMeshHandle = lodHandle;
                 meshComp.localBounds = bounds;
                 meshComp.materialHandle = m_terrainMaterialHandle;
                 meshComp.textureHandle = m_terrainTextureHandle;
                 reg.AddComponent<ECS::MeshComponent>(entity, std::move(meshComp));

                 m_context->Logging->Info("[TerrainCombine] Entity created: entity=%u, request=%u, geoHandle=%u, "
                                          "texHandle=%u, matHandle=%u, lodHandle=%u",
                                          static_cast<uint32_t>(entity), requestId, m_terrainGeometryHandle.index,
                                          m_terrainTextureHandle.index, m_terrainMaterialHandle.index, lodHandle.index);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Main,
         .priority = TaskPriority::Normal,
         .interestedMessages = {static_cast<uint32_t>(Event::EventType::TerrainReadyEvent)}});
}

void GameWorld::Update() {
    if (m_backgroundExecutor) {
        m_backgroundExecutor->Tick();
    }
}