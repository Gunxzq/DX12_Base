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

#include "Renderer/Pipeline/BillboardRenderer.h"
#include "Renderer/Pipeline/OpaqueRenderer.h"
#include "Renderer/Pipeline/ShadowRenderer.h"
#include "Renderer/Pipeline/SkyRenderer.h"
#include "Renderer/Pipeline/TerrainRenderer.h"
#include "Renderer/Pipeline/WaterRenderer.h"

#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/LightManager/LightManager.h"
#include "Renderer/Scene/ReflectionProbeManager/ReflectionProbeManager.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/Asset/LODMesh.h"
#include "Resource/AssetDataManager.h"
#include "Resource/AssetLoader/AssetLoader.h"
#include "Resource/AssetLoader/Loader/DDSLoader.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Geometry/GridGeometry.h"
#include "Resource/Geometry/PatchMesh.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Manager/MaterialManager.h"
#include "Resource/Material/MaterialResource.h"
#include "Resource/Pool/RenderTargetPool.h"
#include "Resource/Texture/TextureManager.h"
#include "Scheduler/FrameDriver.h"

// 异步加载任务
#include "Async/BackgroundExecutor.h"
#include "Async/ResourceTransitionTask.h"
#include "Async/TerrainLoadTask.h"

#include "Renderer/Scene/AmbientOcclusionManager/AmbientOcclusionManager.h"
#include "Renderer/Scene/TerrainManager/TerrainManager.h"

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

namespace ProbeHelpers {
void FillCaptureCB(const DirectX::XMFLOAT3 &probePos, D3D12_GPU_VIRTUAL_ADDRESS &outCBAddress,
                   DX12Engine::Renderer::FrameResourceManager &frameMgr) {
    DirectX::XMVECTOR pos = DirectX::XMLoadFloat3(&probePos);
    static const float s_fd[18] = {1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1};
    static const float s_fu[18] = {0, 1, 0, 0, 1, 0, 0, 0, -1, 0, 0, 1, 0, 1, 0, 0, 1, 0};
    ProbeCaptureCB captureCB = {};
    for (uint32_t f = 0; f < 6; ++f) {
        DirectX::XMVECTOR dir = DirectX::XMVectorSet(s_fd[f * 3], s_fd[f * 3 + 1], s_fd[f * 3 + 2], 0);
        DirectX::XMVECTOR up = DirectX::XMVectorSet(s_fu[f * 3], s_fu[f * 3 + 1], s_fu[f * 3 + 2], 0);
        DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(pos, pos + dir, up);
        DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV2, 1.0f, 0.1f, 1000.0f);
        DirectX::XMMATRIX viewProj = view * proj;

        DirectX::XMStoreFloat4x4(&captureCB.faceViewProj[f], viewProj);
    }
    captureCB.probePosition = probePos;
    outCBAddress = frameMgr.AllocateObjectCB(&captureCB, sizeof(ProbeCaptureCB));
}
} // namespace ProbeHelpers

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

    // 初始化地形渲染器（曲面细分 PSO）
    m_terrainRenderer = std::make_unique<TerrainRenderer>();
    m_terrainRenderer->SetDeviceContext(m_context->DeviceContext);
    m_terrainRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_terrainRenderer->SetMaterialManager(m_context->MaterialMgr);
    m_terrainRenderer->Initialize();

    // 初始化地形常量缓冲区管理器
    TerrainManager::GetInstance().Initialize(m_context->DeviceContext->GetDevice());

    // 初始化地形渲染项构建器
    m_terrainBuilder =
        std::make_unique<TerrainRenderItemBuilder>(m_context->FrameResourceManager, m_context->TextureMgr);

    LoadTestTexture();
    LoadBrickTextures();

    m_probeBuilder =
        std::make_unique<ProbeBuilder>(m_context->FrameResourceManager, m_context->MaterialMgr, m_context->TextureMgr);
    m_probeRenderer = std::make_unique<ReflectionProbeRenderer>();
    m_probeRenderer->SetDeviceContext(m_context->DeviceContext);
    m_probeRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_probeRenderer->SetMaterialManager(m_context->MaterialMgr);
    m_probeRenderer->Initialize();
    LoadWaterTexture();

    // 创建 1x1 纯白纹理（用于反射测试立方体，避免木箱纹理干扰反射观察）
    // 该纹理在 CreateMaterials() 之前创建，确保 m_whiteTextureHandle 已被赋值
    {
        auto &gpuMgr = GpuResourceManager::GetInstance();
        ID3D12Device *device = m_context->DeviceContext->GetDevice();

        D3D12_RESOURCE_DESC whiteDesc = {};
        whiteDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        whiteDesc.Width = 1;
        whiteDesc.Height = 1;
        whiteDesc.DepthOrArraySize = 1;
        whiteDesc.MipLevels = 1;
        whiteDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        whiteDesc.SampleDesc.Count = 1;
        whiteDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        whiteDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        GpuResourceHandle whiteTexHandle = gpuMgr.CreateTexture2D(device, whiteDesc, D3D12_RESOURCE_STATE_COMMON);
        if (whiteTexHandle.IsValid()) {
            uint32_t whiteSrvSlot = m_context->DescriptorHeaps->Allocate(PartitionType::Texture);
            if (whiteSrvSlot != UINT32_MAX) {
                // 创建一个1x1白色纹理 D3D12_SUBRESOURCE_DATA
                uint32_t whitePixel = 0xFFFFFFFFu; // RGBA: 255,255,255,255
                D3D12_SUBRESOURCE_DATA subData = {};
                subData.pData = &whitePixel;
                subData.RowPitch = 4;
                subData.SlicePitch = 4;

                // 上传到 GPU
                uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                auto allocHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle);
                auto cmdHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdHandle);

                UINT64 uploadSize = GetRequiredIntermediateSize(gpuMgr.GetResource(whiteTexHandle), 0, 1);
                GpuResourceHandle uploadBuf =
                    gpuMgr.CreateBuffer(device, uploadSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

                auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                    gpuMgr.GetResource(whiteTexHandle), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                cmdList.Get()->ResourceBarrier(1, &barrier);

                UpdateSubresources(cmdList.Get(), gpuMgr.GetResource(whiteTexHandle), gpuMgr.GetResource(uploadBuf), 0,
                                   0, 1, &subData);

                auto barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(whiteTexHandle),
                                                                     D3D12_RESOURCE_STATE_COPY_DEST,
                                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                cmdList.Get()->ResourceBarrier(1, &barrier2);

                cmdList.Close();
                m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList);
                m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);

                uint64_t seq = m_context->GetNextSequence();
                gpuMgr.Release(uploadBuf, seq);
                m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdHandle);
                m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle, seq);

                // 创建 SRV
                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = 1;
                srvDesc.Texture2D.MostDetailedMip = 0;

                D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
                    m_context->DescriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, whiteSrvSlot);
                device->CreateShaderResourceView(gpuMgr.GetResource(whiteTexHandle), &srvDesc, cpuHandle);

                m_whiteTextureHandle = m_context->TextureMgr->RegisterTexture(whiteTexHandle, whiteSrvSlot);
                m_whiteTextureSrvSlot = whiteSrvSlot;
            }
        }
    }
    CreateMaterials();
    CreateSkybox();
    CreateGroundPlane();
    CreateTestCube();
    CreateTestCylinder();
    CreateTestTorus(); // 程序化环面（SSAO 测试：内圈凹陷）

    // 压力测试：大量动态物体（已注释，后续需要时启用）
    // CreateStressTestScene();

    // 初始化公告牌系统（SSAO 开发期间注释）
    // LoadBillboardTextures();
    // CreateBillboardTrees();

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

    // 注册地形常量立即回调（LightManager 模式：Immediate 中分配+上传地形常量）
    RegisterTerrainImmediateCallback();

    // 注册探针场景数据上传回调（SceneDataUpload 阶段：填充 ProbeCaptureInfo + 分配 CB）
    RegisterProbeSceneDataCallback();

    // 注册清除系统（PrePass 阶段，独立于任何实体渲染）
    RegisterClearSystem();

    // 注册阴影渲染系统
    RegisterShadowRenderSystem();
    // 注册 SSAO 系统（PrePass 深度就绪后，Opaque 光照前）
    // RegisterSsaoSystem();
    {
        auto &aoMgr = AmbientOcclusionManager::GetInstance();
        aoMgr.SetEnabled(false);
        if (m_whiteTextureHandle.IsValid())
            aoMgr.SetFallbackWhiteSRV(m_context->TextureMgr->GetSRV(m_whiteTextureHandle));
    }
    // 注册反射探针捕获系统
    RegisterProbeCaptureSystem();

    // [SSAO开发期间注释] 注册地形渲染系统
    // RegisterTerrainRenderSystem();

    // 注册构建器
    RegisterBuilderSystems();
}

void GameWorld::Clear() {
    if (!m_registry)
        return;

    // TODO(StaticComponent): 静态优化暂未启用，销毁时无需清理持久化 GPU 资源
    auto DestroyEntityWithCleanup = [this](ECS::Entity entity) {
        if (entity == INVALID_ENTITY)
            return;
        m_registry->DestroyEntity(entity);
    };

    // 移除所有测试立方体
    for (auto entity : m_cubeEntities) {
        DestroyEntityWithCleanup(entity);
    }
    m_cubeEntities.clear();
    m_cubeEntity = INVALID_ENTITY;

    // 移除地面平面
    DestroyEntityWithCleanup(m_groundPlaneEntity);
    m_groundPlaneEntity = INVALID_ENTITY;

    // 移除压力测试实体（当前已注释，保留清理代码备查）
    // for (auto entity : m_stressEntities) {
    //     DestroyEntityWithCleanup(entity);
    // }
    // m_stressEntities.clear();
}

void GameWorld::RegisterBuilderSystems() {
    // =========================================================================
    // 所有构建器合并在一个 System 中串行执行，避免 RingBuffer 多线程竞争
    // =========================================================================

    SystemRegistry::Register({
        .name = "RenderBuilders",
        .func =
            [this](Registry &reg, const MessageContext &) {
                // 4. 地形构建器
                if (m_terrainBuilder) {
                    m_terrainBuilder->SetCullingResult(&m_context->cullingResult);
                    m_terrainBuilder->BuildTyped(reg, m_terrainQueue);
                }

                // 1. 不透明构建器（统一实例化模式）
                m_opaqueBuilder->SetCullingResult(&m_context->cullingResult);
                m_opaqueBuilder->SetLODResult(&m_context->lodResult);
                m_opaqueBuilder->BuildTyped(reg, m_opaqueQueue);

                // 3. 公告牌构建器（需要相机位置做朝向计算）
                if (m_billboardBuilder) {
                    m_billboardBuilder->SetCullingResult(&m_context->cullingResult);
                    m_billboardBuilder->SetLODResult(&m_context->lodResult);
                    m_billboardBuilder->SetCameraPosition(m_context->CameraMgr->GetMainCamera().Position);
                    m_billboardBuilder->BuildTyped(reg, m_billboardQueue);
                }

                // 2. 透明构建器（远到近排序，需要相机位置）
                m_transparentBuilder->SetCullingResult(&m_context->cullingResult);
                m_transparentBuilder->SetLODResult(&m_context->lodResult);
                m_transparentBuilder->BuildTyped(reg, m_transparentQueue);

                // 5. 反射探头构建器（使用 SceneDataUpload 准备好的 m_probeCaptureInfo）
                if (m_probeBuilder && m_activeProbeCount > 0) {
                    uint32_t probeCount = m_context->ReflectionProbeMgr->GetActiveProbeCount();
                    m_activeProbeCount = probeCount;
                    if (probeCount > 0) {
                        ProbeCaptureInfo probeSet[64];
                        m_probeBuilder->SetLODSystem(m_context->LODSystem);

                        m_probeBuilder->Build(m_probeCaptureInfo, m_activeProbeCount, reg, m_probeQueues);
                    }
                }
            },
        .phase = TaskPhase::PreRender,
        .threadType = ThreadType::Worker,
        .alwaysRun = true,
    });
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

void GameWorld::RegisterTerrainImmediateCallback() {
    if (!m_context || !m_context->FrameDriver || !m_registry)
        return;

    m_context->FrameDriver->RegisterImmediateCallback(
        [this]() {
            auto &terrainMgr = TerrainManager::GetInstance();

            // 收集所有可见地形块的 TerrainConstants
            // 注意：此时尚未做剔除，我们遍历所有 TerrainComponent
            // 如果后续有剔除需求，可以只上传可见块
            std::vector<TerrainConstants> pendingConstants;
            pendingConstants.reserve(16);

            auto view = m_registry->view<TerrainComponent>();
            for (auto entity : view) {
                auto *terrainComp = m_registry->TryGetComponent<TerrainComponent>(entity);
                if (!terrainComp || !terrainComp->geometryHandle.IsValid())
                    continue;

                TerrainConstants constants = {};
                DirectX::XMStoreFloat4x4(&constants.World, DirectX::XMMatrixIdentity());
                DirectX::XMStoreFloat4x4(&constants.WorldInvTranspose, DirectX::XMMatrixIdentity());
                DirectX::XMStoreFloat4x4(&constants.PrevWorld, DirectX::XMMatrixIdentity());
                constants.MaterialIndex = terrainComp->materialIndex;
                constants.ReceiveShadow = 1;
                constants.HeightScale = terrainComp->heightScale;
                constants.HeightOffset = terrainComp->heightOffset;
                constants.TessellationFactor = terrainComp->tessellationFactor;
                constants.TessellationDistanceMin = terrainComp->tessellationDistanceMin;
                constants.TessellationDistanceMax = terrainComp->tessellationDistanceMax;
                constants.HeightMapIndex = 0; // 高度图 — gTerrainTextures[0] (H_Runtime_heightmap)
                constants.AlbedoMapIndex = 1; // 漫反射 — gTerrainTextures[1] (D_heightmap)
                constants.NormalMapIndex = 2; // 法线贴图 — gTerrainTextures[2] (N_heightmap)

                pendingConstants.push_back(constants);
            }

            // 设置待上传数据并一次性分配+上传
            if (!pendingConstants.empty()) {
                terrainMgr.SetPendingConstants(std::move(pendingConstants));
                terrainMgr.UpdateAndUpload(m_context->GetNextFence());
            }
        },
        "TerrainImmediate");
}

void GameWorld::RegisterProbeSceneDataCallback() {
    if (!m_context || !m_context->FrameDriver || !m_context->ReflectionProbeMgr)
        return;

    m_context->FrameDriver->RegisterSceneDataCallback([this]() {
        auto *probeMgr = m_context->ReflectionProbeMgr;
        uint32_t probeCount = probeMgr->GetActiveProbeCount();
        m_activeProbeCount = probeCount;
        if (probeCount == 0)
            return;

        for (uint32_t i = 0; i < probeCount; ++i) {
            m_probeCaptureInfo[i].position = probeMgr->GetProbePosition(i);
            m_probeCaptureInfo[i].captureRange = probeMgr->GetProbeCaptureRange(i);
            m_probeCaptureInfo[i].probeIndex = i;
            m_probeCaptureInfo[i].resolution = probeMgr->GetProbeResources(i).resolution;
            m_probeCaptureInfo[i].dsvSlot = probeMgr->GetProbeDepthSlot(i);
            {
                auto &res = probeMgr->GetProbeResources(i);
                if (res.rtHandle.IsValid()) {
                    m_probeCaptureInfo[i].rtvBaseSlot = res.rtHandle.rtvSlot;
                    m_probeCaptureInfo[i].cubemapResource = RenderTargetPool::GetInstance().GetResource(res.rtHandle);
                } else {
                    m_probeCaptureInfo[i].rtvBaseSlot = UINT32_MAX;
                    m_probeCaptureInfo[i].cubemapResource = nullptr;
                }
            }
            ProbeHelpers::FillCaptureCB(m_probeCaptureInfo[i].position, m_probeCaptureInfo[i].captureCBAddress,
                                        *m_context->FrameResourceManager);
        }
    });
}

void GameWorld::CreateGroundPlane() {
    if (!m_registry || !m_renderer || !m_context)
        return;

    // 1. 生成 10x10 平面几何体（XZ 平面，法线朝上）
    GeometryGenerator geoGen;
    auto meshData = geoGen.CreateGrid(10.0f, 10.0f, 10, 10);

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
    bounds.min = XMFLOAT3(-5.0f, 20.0f, -5.0f);
    bounds.max = XMFLOAT3(5.0f, 0.0f, 5.0f);

    auto &geoMgr = m_context->GeometryResourceManager;
    GeometryHandle geoHandle = geoMgr->RegisterGeometry<TriangleMesh>(planeMesh);
    if (!geoHandle.IsValid()) {
        OutputDebugStringW(L"[ERROR] RegisterGeometry for ground plane failed!\n");
        return;
    }

    // 4. 创建 LODMesh
    LODMesh lodMesh;
    lodMesh.lodChain = {geoHandle};
    LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

    // 5. 创建实体 — 平面抬高到远离水面（水面 Y=0，地形 Y=-30）
    m_groundPlaneEntity = m_registry->CreateEntity();

    XMFLOAT3 position(0.0f, 30.0f, 0.0f);
    XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
    XMFLOAT3 scale(1.0f, 1.0f, 1.0f);
    m_registry->AddComponent<TransformComponent>(m_groundPlaneEntity, position, rotation, scale);

    MeshComponent meshComp;
    meshComp.lodMeshHandle = lodHandle;
    meshComp.localBounds = bounds;
    meshComp.materialHandle = m_brickMaterialHandle; // 砖块材质（含法线贴图）
    meshComp.textureHandle = m_brickTextureHandle;
    meshComp.receivesShadow = true;
    m_registry->AddComponent<MeshComponent>(m_groundPlaneEntity, std::move(meshComp));

    // m_registry->AddComponent<StaticComponent>(m_groundPlaneEntity);

    OutputDebugStringW(L"[GameWorld] Ground plane created at Y=60.0 (10x10)\n");
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
    GeometryHandle geoHandle = geoMgr->RegisterGeometry<TriangleMesh>(triangleMesh);

    if (!geoHandle.IsValid()) {
        OutputDebugStringW(L"[ERROR] RegisterGeometry failed!\n");
        return;
    }

    const TriangleMesh *testMesh = geoMgr->GetGeometry<TriangleMesh>(geoHandle);
    if (!testMesh) {
        OutputDebugStringW(L"[ERROR] GetGeometry returned null!\n");
        return;
    }

    // 立方体抬高到远离水面（水面 Y=0，地形 Y=-30）
    constexpr float PLANE_Y = 30.0f;

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

        // 第二个立方体（放大 2 倍）添加拾取组件
        if (i == 1) {
            m_registry->AddComponent<PickingComponent>(entity);
        }

        // m_registry->AddComponent<StaticComponent>(entity);

        m_cubeEntities.push_back(entity);
    }

    // 保持第一个立方体为 m_cubeEntity（兼容旧代码）
    m_cubeEntity = m_cubeEntities.empty() ? INVALID_ENTITY : m_cubeEntities[0];

    // 反射探针测试立方体 — 位于探针位置 (0, 20, 0)，采样捕获的 Cubemap
    {
        m_reflectionCubeEntity = m_registry->CreateEntity();
        m_registry->AddComponent<TransformComponent>(m_reflectionCubeEntity, XMFLOAT3(10.0f, 32.0f, 3.0f),
                                                     XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(1.0f, 1.0f, 1.0f));

        LODMesh lodMesh;
        lodMesh.lodChain = {geoHandle};
        LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

        MeshComponent meshComp;
        meshComp.lodMeshHandle = lodHandle;
        meshComp.localBounds = bounds;
        meshComp.materialHandle = m_reflectionTestMaterialHandle; // 金属材质
        meshComp.textureHandle = m_whiteTextureHandle;
        m_registry->AddComponent<MeshComponent>(m_reflectionCubeEntity, std::move(meshComp));

        // 绑定反射探针索引 0，使该立方体在渲染时采样探针 Cubemap
        m_registry->AddComponent<ReflectionConsumerComponent>(
            m_reflectionCubeEntity, ReflectionConsumerComponent{.probeIndex = 0, .useDynamicFallback = false});
    }

    // 注册旋转系统
    // RegisterRotationSystem();
    // 注册立方体渲染系统
    RegisterCubeRenderSystem();
}

void GameWorld::CreateStressTestScene() {
    if (!m_registry || !m_renderer || !m_context) {
        return;
    }

    auto &geoMgr = m_context->GeometryResourceManager;

    GeometryGenerator geoGen;
    auto meshData = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 0);

    auto &gpuMgr = GpuResourceManager::GetInstance();
    auto device = m_context->DeviceContext->GetDevice();

    // 顶点缓冲
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

    // 索引缓冲
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

    TriangleMesh triangleMesh;
    triangleMesh.vertexBufferHandle = vbHandle;
    triangleMesh.indexBufferHandle = ibHandle;
    triangleMesh.vertexCount = static_cast<uint32_t>(meshData.Vertices.size());
    triangleMesh.indexCount = static_cast<uint32_t>(meshData.Indices32.size());
    triangleMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
    triangleMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    triangleMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    triangleMesh.isGpuReady = true;

    BoundingAABB bounds;
    bounds.min = XMFLOAT3(-0.5f, -0.5f, -0.5f);
    bounds.max = XMFLOAT3(0.5f, 0.5f, 0.5f);

    GeometryHandle geoHandle = geoMgr->RegisterGeometry<TriangleMesh>(triangleMesh);
    if (!geoHandle.IsValid()) {
        OutputDebugStringW(L"[StressTest] Failed to register geometry\n");
        return;
    }

    constexpr float PLANE_Y = 30.0f;
    constexpr int GRID_HALF = 30;
    constexpr float SPACING = 2.0f;

    m_stressEntities.clear();
    m_stressEntities.reserve((GRID_HALF * 2) * (GRID_HALF * 2));

    XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
    XMFLOAT3 scale(1.0f, 1.0f, 1.0f);

    for (int x = -GRID_HALF; x < GRID_HALF; ++x) {
        for (int z = -GRID_HALF; z < GRID_HALF; ++z) {
            auto entity = m_registry->CreateEntity();

            XMFLOAT3 position(x * SPACING, PLANE_Y + 0.5f, z * SPACING);
            m_registry->AddComponent<TransformComponent>(entity, position, rotation, scale);

            LODMesh lodMesh;
            lodMesh.lodChain = {geoHandle};
            LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

            MeshComponent meshComp;
            meshComp.lodMeshHandle = lodHandle;
            meshComp.localBounds = bounds;
            meshComp.materialHandle = m_cubeMaterialHandle;
            meshComp.textureHandle = m_testTextureHandle;
            meshComp.receivesShadow = true;
            m_registry->AddComponent<MeshComponent>(entity, std::move(meshComp));
            // m_registry->AddComponent<ECS::StaticComponent>(entity);

            m_stressEntities.push_back(entity);
        }
    }
}

void GameWorld::CreateTestCylinder() {
    if (!m_registry || !m_renderer || !m_context)
        return;

    GeometryGenerator geoGen;
    auto meshData = geoGen.CreateCylinder(0.5f, 0.5f, 3.0f, 20, 8);

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

    TriangleMesh triangleMesh;
    triangleMesh.vertexBufferHandle = vbHandle;
    triangleMesh.indexBufferHandle = ibHandle;
    triangleMesh.vertexCount = static_cast<uint32_t>(meshData.Vertices.size());
    triangleMesh.indexCount = static_cast<uint32_t>(meshData.Indices32.size());
    triangleMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
    triangleMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    triangleMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    triangleMesh.isGpuReady = true;

    Math::BoundingAABB bounds;
    bounds.min = XMFLOAT3(-1.0f, -1.0f, -1.0f);
    bounds.max = XMFLOAT3(1.0f, 1.0f, 1.0f);

    auto &geoMgr = m_context->GeometryResourceManager;
    GeometryHandle geoHandle = geoMgr->RegisterGeometry<TriangleMesh>(triangleMesh);
    if (!geoHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] RegisterGeometry for cylinder failed!");
        return;
    }

    LODMesh lodMesh;
    lodMesh.lodChain = {geoHandle};
    LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

    auto entity = m_registry->CreateEntity();
    m_registry->AddComponent<TransformComponent>(entity, XMFLOAT3(4.0f, 32.0f, -3.0f), XMFLOAT3(0.0f, 0.0f, 0.0f),
                                                 XMFLOAT3(1.0f, 1.0f, 1.0f));
    MeshComponent meshComp;
    meshComp.lodMeshHandle = lodHandle;
    meshComp.localBounds = bounds;
    meshComp.materialHandle = m_brickMaterialHandle;
    meshComp.textureHandle = m_brickTextureHandle;
    meshComp.receivesShadow = true;
    m_registry->AddComponent<MeshComponent>(entity, std::move(meshComp));
    m_context->Logging->Info("[GameWorld] Test cylinder created with brick material + normal map");
}

// ========================================================================
// CreateTestTorus — 程序化生成环形圆环
// 用途：SSAO 测试，圆环内圈有明确的自遮挡凹陷区域
// ========================================================================
void GameWorld::CreateTestTorus() {
    if (!m_registry || !m_renderer || !m_context)
        return;

    constexpr float R = 2.0f;   // 主半径（圆心到管中心的距离）
    constexpr float r = 0.8f;   // 次半径（管的半径）
    constexpr UINT slices = 48; // 主方向分段
    constexpr UINT stacks = 24; // 管方向分段
    constexpr float PI = 3.14159265358979f;

    std::vector<GeometryGenerator::Vertex> vertices;
    std::vector<uint32_t> indices;

    // 生成顶点
    for (UINT i = 0; i <= slices; ++i) {
        float theta = (float)i / slices * 2.0f * PI;
        float cosTheta = cosf(theta);
        float sinTheta = sinf(theta);

        for (UINT j = 0; j <= stacks; ++j) {
            float phi = (float)j / stacks * 2.0f * PI;
            float cosPhi = cosf(phi);
            float sinPhi = sinf(phi);

            // 位置
            float px = (R + r * cosPhi) * cosTheta;
            float py = r * sinPhi;
            float pz = (R + r * cosPhi) * sinTheta;

            // 法线（从管表面指向外）
            float nx = cosPhi * cosTheta;
            float ny = sinPhi;
            float nz = cosPhi * sinTheta;

            // 切线（沿主方向）
            float tx = -sinTheta;
            float ty = 0.0f;
            float tz = cosTheta;

            // UV
            float u = (float)i / slices;
            float v = (float)j / stacks;

            vertices.emplace_back(px, py, pz, nx, ny, nz, tx, ty, tz, u, v);
        }
    }

    // 生成索引（网格拓扑）
    for (UINT i = 0; i < slices; ++i) {
        for (UINT j = 0; j < stacks; ++j) {
            UINT i0 = i * (stacks + 1) + j;
            UINT i1 = i0 + 1;
            UINT i2 = (i + 1) * (stacks + 1) + j;
            UINT i3 = i2 + 1;

            // 两个三角形构成一个四边形
            indices.push_back(i0);
            indices.push_back(i1);
            indices.push_back(i2);

            indices.push_back(i1);
            indices.push_back(i3);
            indices.push_back(i2);
        }
    }

    // 创建 GPU 缓冲区
    auto &gpuMgr = GpuResourceManager::GetInstance();
    auto device = m_context->DeviceContext->GetDevice();

    size_t vbSize = vertices.size() * sizeof(GeometryGenerator::Vertex);
    auto vbHandle = gpuMgr.CreateBuffer(device, vbSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *vbResource = gpuMgr.GetResource(vbHandle);
    if (vbResource) {
        void *vbMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        vbResource->Map(0, &readRange, &vbMapped);
        memcpy(vbMapped, vertices.data(), vbSize);
        vbResource->Unmap(0, nullptr);
    }

    size_t ibSize = indices.size() * sizeof(uint32_t);
    auto ibHandle = gpuMgr.CreateBuffer(device, ibSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ibResource = gpuMgr.GetResource(ibHandle);
    if (ibResource) {
        void *ibMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ibResource->Map(0, &readRange, &ibMapped);
        memcpy(ibMapped, indices.data(), ibSize);
        ibResource->Unmap(0, nullptr);
    }

    TriangleMesh triangleMesh;
    triangleMesh.vertexBufferHandle = vbHandle;
    triangleMesh.indexBufferHandle = ibHandle;
    triangleMesh.vertexCount = static_cast<uint32_t>(vertices.size());
    triangleMesh.indexCount = static_cast<uint32_t>(indices.size());
    triangleMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
    triangleMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    triangleMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    triangleMesh.isGpuReady = true;

    // 计算包围盒
    float extent = R + r;
    Math::BoundingAABB bounds;
    bounds.min = XMFLOAT3(-extent, -r, -extent);
    bounds.max = XMFLOAT3(extent, r, extent);

    auto &geoMgr = m_context->GeometryResourceManager;
    GeometryHandle geoHandle = geoMgr->RegisterGeometry<TriangleMesh>(triangleMesh);
    if (!geoHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] RegisterGeometry for torus failed!");
        return;
    }

    // 注册 LOD
    LODMesh lodMesh;
    lodMesh.lodChain = {geoHandle};
    LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

    // 创建 ECS 实体，放在场景中央偏前位置
    auto entity = m_registry->CreateEntity();
    m_registry->AddComponent<TransformComponent>(entity, XMFLOAT3(0.0f, 32.0f, 0.0f), XMFLOAT3(0.0f, 0.0f, 0.0f),
                                                 XMFLOAT3(1.0f, 1.0f, 1.0f));
    MeshComponent meshComp;
    meshComp.lodMeshHandle = lodHandle;
    meshComp.localBounds = bounds;
    meshComp.materialHandle = m_brickMaterialHandle;
    meshComp.textureHandle = m_brickTextureHandle;
    meshComp.receivesShadow = true;
    m_registry->AddComponent<MeshComponent>(entity, std::move(meshComp));

    m_context->Logging->Info("[GameWorld] Test torus created: {} vertices, {} triangles (SSAO test)", vertices.size(),
                             indices.size() / 3);
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
    uint32_t srvIndex = descriptorHeaps->Allocate(PartitionType::Texture);
    m_context->Logging->Info("[SlotDBG] CreateTestTexture Allocate srvIndex={}", srvIndex);

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

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, srvIndex);
    device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);

    // 注册到 TextureManager
    TextureManager *texMgr = m_context->TextureMgr;
    m_testTextureHandle = texMgr->RegisterTexture(gpuHandle, srvIndex);
    m_testTextureSrvSlot = srvIndex;

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

void GameWorld::LoadBrickTextures() {
    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Device *device = m_context->DeviceContext->GetDevice();
    auto &descriptorHeaps = m_context->DescriptorHeaps;

    // 加载 bricks2.dds（漫反射）
    {
        DDSTextureInfo ddsInfo;
        if (!AssetLoader::GetInstance().LoadTextureFromFile(L"Content/Textures/bricks2.dds", ddsInfo)) {
            m_context->Logging->Warn("[GameWorld] Failed to load bricks2.dds, skipping brick material");
            return;
        }

        GpuResourceHandle gpuHandle = gpuMgr.CreateTexture2D(device, ddsInfo.desc, D3D12_RESOURCE_STATE_COMMON);
        if (!gpuHandle.IsValid())
            return;

        uint32_t srvSlot = descriptorHeaps->Allocate(PartitionType::Texture);
        if (srvSlot == UINT32_MAX) {
            gpuMgr.Release(gpuHandle, 0);
            return;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = ddsInfo.desc.Format;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = ddsInfo.desc.MipLevels;
        srvDesc.Texture2D.MostDetailedMip = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, srvSlot);
        device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);

        m_brickTextureHandle = m_context->TextureMgr->RegisterTexture(gpuHandle, srvSlot);
        m_brickTextureSrvSlot = srvSlot;

        // 上传
        uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
        auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
        auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
        auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
        auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);

        auto subresources = ddsInfo.subresources;
        UINT64 uploadSize =
            GetRequiredIntermediateSize(gpuMgr.GetResource(gpuHandle), 0, static_cast<UINT>(subresources.size()));
        GpuResourceHandle uploadBuf =
            gpuMgr.CreateBuffer(device, uploadSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COMMON,
                                                       D3D12_RESOURCE_STATE_COPY_DEST);
        cmd.Get()->ResourceBarrier(1, &b1);
        UpdateSubresources(cmd.Get(), gpuMgr.GetResource(gpuHandle), gpuMgr.GetResource(uploadBuf), 0, 0,
                           static_cast<UINT>(subresources.size()), subresources.data());
        auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COPY_DEST,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd.Get()->ResourceBarrier(1, &b2);
        cmd.Close();
        m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmd);
        m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);
        uint64_t seq = m_context->GetNextSequence();
        gpuMgr.Release(uploadBuf, seq);
        m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
        m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
    }

    // 加载 bricks2_NRM.dds（CrazyBump 生成的法线贴图）
    {
        DDSTextureInfo ddsInfo;
        if (!AssetLoader::GetInstance().LoadTextureFromFile(L"Content/Textures/bricks2_NRM.dds", ddsInfo)) {
            m_context->Logging->Warn("[GameWorld] Failed to load bricks2_nmap.dds, skipping normal map");
            return;
        }

        GpuResourceHandle gpuHandle = gpuMgr.CreateTexture2D(device, ddsInfo.desc, D3D12_RESOURCE_STATE_COMMON);
        if (!gpuHandle.IsValid())
            return;

        uint32_t srvSlot = descriptorHeaps->Allocate(PartitionType::Texture);
        if (srvSlot == UINT32_MAX) {
            gpuMgr.Release(gpuHandle, 0);
            return;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = ddsInfo.desc.Format;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = ddsInfo.desc.MipLevels;
        srvDesc.Texture2D.MostDetailedMip = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, srvSlot);
        device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);

        // 法线贴图只需记录 SRV 槽位，不上传到 TextureManager（仅采样用）
        m_brickNormalSrvSlot = srvSlot;
        m_context->Logging->Info("[SlotDBG] Brick normal map SRV slot={}, format={}", srvSlot,
                                 static_cast<int>(ddsInfo.desc.Format));

        // 上传
        uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
        auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
        auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
        auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
        auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);

        auto subresources = ddsInfo.subresources;
        UINT64 uploadSize =
            GetRequiredIntermediateSize(gpuMgr.GetResource(gpuHandle), 0, static_cast<UINT>(subresources.size()));
        GpuResourceHandle uploadBuf =
            gpuMgr.CreateBuffer(device, uploadSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COMMON,
                                                       D3D12_RESOURCE_STATE_COPY_DEST);
        cmd.Get()->ResourceBarrier(1, &b1);
        UpdateSubresources(cmd.Get(), gpuMgr.GetResource(gpuHandle), gpuMgr.GetResource(uploadBuf), 0, 0,
                           static_cast<UINT>(subresources.size()), subresources.data());
        auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COPY_DEST,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd.Get()->ResourceBarrier(1, &b2);
        cmd.Close();
        m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmd);
        m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);
        uint64_t seq = m_context->GetNextSequence();
        gpuMgr.Release(uploadBuf, seq);
        m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
        m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
    }

    // ============================================================
    // 加载 bricks2_OCC.dds（AO 贴图）
    // ============================================================
    {
        DDSTextureInfo ddsInfo;
        if (!AssetLoader::GetInstance().LoadTextureFromFile(L"Content/Textures/bricks2_OCC.dds", ddsInfo)) {
            m_context->Logging->Warn("[GameWorld] Failed to load bricks2_OCC.dds, skipping AO map");
        } else {
            GpuResourceHandle gpuHandle = gpuMgr.CreateTexture2D(device, ddsInfo.desc, D3D12_RESOURCE_STATE_COMMON);
            if (gpuHandle.IsValid()) {
                uint32_t srvSlot = descriptorHeaps->Allocate(PartitionType::Texture);
                if (srvSlot != UINT32_MAX) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Format = ddsInfo.desc.Format;
                    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = ddsInfo.desc.MipLevels;
                    srvDesc.Texture2D.MostDetailedMip = 0;
                    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
                        descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, srvSlot);
                    device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);
                    m_brickOcclusionSrvSlot = srvSlot;

                    uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                    auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                    auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                    auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                    auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
                    auto subresources = ddsInfo.subresources;
                    UINT64 uploadSize = GetRequiredIntermediateSize(gpuMgr.GetResource(gpuHandle), 0,
                                                                    static_cast<UINT>(subresources.size()));
                    GpuResourceHandle uploadBuf = gpuMgr.CreateBuffer(device, uploadSize, D3D12_HEAP_TYPE_UPLOAD,
                                                                      D3D12_RESOURCE_STATE_GENERIC_READ);
                    auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(
                        gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                    cmd.Get()->ResourceBarrier(1, &b1);
                    UpdateSubresources(cmd.Get(), gpuMgr.GetResource(gpuHandle), gpuMgr.GetResource(uploadBuf), 0, 0,
                                       static_cast<UINT>(subresources.size()), subresources.data());
                    auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle),
                                                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    cmd.Get()->ResourceBarrier(1, &b2);
                    cmd.Close();
                    m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmd);
                    m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);
                    uint64_t seq = m_context->GetNextSequence();
                    gpuMgr.Release(uploadBuf, seq);
                    m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
                    m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
                } else {
                    gpuMgr.Release(gpuHandle, 0);
                }
            }
        }
    }

    // ============================================================
    // 加载 bricks2_SPEC.dds（金属度-粗糙度贴图）
    // ============================================================
    {
        DDSTextureInfo ddsInfo;
        if (!AssetLoader::GetInstance().LoadTextureFromFile(L"Content/Textures/bricks2_SPEC.dds", ddsInfo)) {
            m_context->Logging->Warn("[GameWorld] Failed to load bricks2_SPEC.dds, skipping metallicRoughness map");
        } else {
            GpuResourceHandle gpuHandle = gpuMgr.CreateTexture2D(device, ddsInfo.desc, D3D12_RESOURCE_STATE_COMMON);
            if (gpuHandle.IsValid()) {
                uint32_t srvSlot = descriptorHeaps->Allocate(PartitionType::Texture);
                if (srvSlot != UINT32_MAX) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Format = ddsInfo.desc.Format;
                    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = ddsInfo.desc.MipLevels;
                    srvDesc.Texture2D.MostDetailedMip = 0;
                    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
                        descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, srvSlot);
                    device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);
                    m_brickMetallicRoughnessSrvSlot = srvSlot;

                    uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                    auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                    auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                    auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                    auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
                    auto subresources = ddsInfo.subresources;
                    UINT64 uploadSize = GetRequiredIntermediateSize(gpuMgr.GetResource(gpuHandle), 0,
                                                                    static_cast<UINT>(subresources.size()));
                    GpuResourceHandle uploadBuf = gpuMgr.CreateBuffer(device, uploadSize, D3D12_HEAP_TYPE_UPLOAD,
                                                                      D3D12_RESOURCE_STATE_GENERIC_READ);
                    auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(
                        gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                    cmd.Get()->ResourceBarrier(1, &b1);
                    UpdateSubresources(cmd.Get(), gpuMgr.GetResource(gpuHandle), gpuMgr.GetResource(uploadBuf), 0, 0,
                                       static_cast<UINT>(subresources.size()), subresources.data());
                    auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle),
                                                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    cmd.Get()->ResourceBarrier(1, &b2);
                    cmd.Close();
                    m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmd);
                    m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);
                    uint64_t seq = m_context->GetNextSequence();
                    gpuMgr.Release(uploadBuf, seq);
                    m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
                    m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
                } else {
                    gpuMgr.Release(gpuHandle, 0);
                }
            }
        }
    }

    // ============================================================
    // 加载 bricks2_DISP.dds（高度/位移贴图）
    // ============================================================
    {
        DDSTextureInfo ddsInfo;
        if (!AssetLoader::GetInstance().LoadTextureFromFile(L"Content/Textures/bricks2_DISP.dds", ddsInfo)) {
            m_context->Logging->Warn("[GameWorld] Failed to load bricks2_DISP.dds, skipping height map");
        } else {
            GpuResourceHandle gpuHandle = gpuMgr.CreateTexture2D(device, ddsInfo.desc, D3D12_RESOURCE_STATE_COMMON);
            if (gpuHandle.IsValid()) {
                uint32_t srvSlot = descriptorHeaps->Allocate(PartitionType::Texture);
                if (srvSlot != UINT32_MAX) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Format = ddsInfo.desc.Format;
                    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = ddsInfo.desc.MipLevels;
                    srvDesc.Texture2D.MostDetailedMip = 0;
                    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
                        descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, srvSlot);
                    device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);
                    m_brickHeightSrvSlot = srvSlot;

                    uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                    auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                    auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                    auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                    auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
                    auto subresources = ddsInfo.subresources;
                    UINT64 uploadSize = GetRequiredIntermediateSize(gpuMgr.GetResource(gpuHandle), 0,
                                                                    static_cast<UINT>(subresources.size()));
                    GpuResourceHandle uploadBuf = gpuMgr.CreateBuffer(device, uploadSize, D3D12_HEAP_TYPE_UPLOAD,
                                                                      D3D12_RESOURCE_STATE_GENERIC_READ);
                    auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(
                        gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                    cmd.Get()->ResourceBarrier(1, &b1);
                    UpdateSubresources(cmd.Get(), gpuMgr.GetResource(gpuHandle), gpuMgr.GetResource(uploadBuf), 0, 0,
                                       static_cast<UINT>(subresources.size()), subresources.data());
                    auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle),
                                                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    cmd.Get()->ResourceBarrier(1, &b2);
                    cmd.Close();
                    m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmd);
                    m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);
                    uint64_t seq = m_context->GetNextSequence();
                    gpuMgr.Release(uploadBuf, seq);
                    m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
                    m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
                } else {
                    gpuMgr.Release(gpuHandle, 0);
                }
            }
        }
    }

    m_context->Logging->Info("[GameWorld] Brick textures loaded");
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
    cubeMaterial.baseColorTextureId = m_testTextureSrvSlot != UINT32_MAX ? m_testTextureSrvSlot : 0;
    cubeMaterial.rendererTypeHash = TYPE_HASH("OpaquePBR");
    m_cubeMaterialHandle = materialMgr->RegisterMaterial(cubeMaterial);

    // 砖块材质（PBR 贴图驱动）
    MaterialData brickMaterial;
    brickMaterial.materialId = TYPE_HASH("brick_material");
    brickMaterial.name = "brick_material";
    brickMaterial.baseColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    brickMaterial.metallic = 0.0f;  // 砖块是非金属
    brickMaterial.roughness = 0.4f; // 固定值回退
    brickMaterial.ambient = 0.5f;   // 固定值回退
    brickMaterial.alpha = 1.0f;
    brickMaterial.normalIntensity = 1.0f;
    brickMaterial.baseColorTextureId = m_brickTextureSrvSlot != UINT32_MAX ? m_brickTextureSrvSlot : 0;
    brickMaterial.normalTextureId = m_brickNormalSrvSlot != UINT32_MAX ? m_brickNormalSrvSlot : 0;
    brickMaterial.metallicRoughnessTextureId =
        m_brickMetallicRoughnessSrvSlot != UINT32_MAX ? m_brickMetallicRoughnessSrvSlot : 0;
    brickMaterial.occlusionTextureId = m_brickOcclusionSrvSlot != UINT32_MAX ? m_brickOcclusionSrvSlot : 0;
    brickMaterial.heightTextureId = m_brickHeightSrvSlot != UINT32_MAX ? m_brickHeightSrvSlot : 0;
    brickMaterial.rendererTypeHash = TYPE_HASH("OpaquePBR");
    m_brickMaterialHandle = materialMgr->RegisterMaterial(brickMaterial);

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

    // 反射探针测试立方体材质（高金属度、低粗糙度，用于观察反射效果）
    MaterialData reflectionTestMaterial;
    reflectionTestMaterial.materialId = TYPE_HASH("reflection_test_material");
    reflectionTestMaterial.name = "reflection_test_material";
    reflectionTestMaterial.baseColor = XMFLOAT4(0.9f, 0.9f, 1.0f, 1.0f); // 浅灰蓝色，与普通立方体区分
    reflectionTestMaterial.metallic = 1.0f;                              // 全金属，反射最明显
    reflectionTestMaterial.roughness = 0.2f;                             // 光滑表面，清晰反射
    reflectionTestMaterial.ambient = 0.1f;
    reflectionTestMaterial.alpha = 1.0f;
    reflectionTestMaterial.baseColorTextureId = m_whiteTextureSrvSlot != UINT32_MAX ? m_whiteTextureSrvSlot : 0;
    reflectionTestMaterial.rendererTypeHash = TYPE_HASH("OpaquePBR");
    m_reflectionTestMaterialHandle = materialMgr->RegisterMaterial(reflectionTestMaterial);

    // 公告牌材质（非金属、中等粗糙度，适合树木等植被）
    MaterialData billboardMaterial;
    billboardMaterial.materialId = TYPE_HASH("billboard_material");
    billboardMaterial.name = "billboard_material";
    billboardMaterial.baseColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f); // 白色，颜色由纹理决定
    billboardMaterial.metallic = 0.0f;
    billboardMaterial.roughness = 0.8f;
    billboardMaterial.ambient = 1.0f;
    billboardMaterial.alpha = 1.0f;
    billboardMaterial.rendererTypeHash = TYPE_HASH("OpaquePBR");
    m_billboardMaterialHandle = materialMgr->RegisterMaterial(billboardMaterial);

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
    uint32_t srvIndex = descriptorHeaps->Allocate(PartitionType::Buffer); // MaterialBuffer StructuredBuffer
    m_context->Logging->Info("[SlotDBG] CreateMaterials Allocate srvIndex={}", srvIndex);
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

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetPartitionCpuHandle(PartitionType::Buffer, srvIndex);
    device->CreateShaderResourceView(gpuMgr.GetResource(bufferHandle), &srvDesc, cpuHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = descriptorHeaps->GetPartitionGpuHandle(PartitionType::Buffer, srvIndex);
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
    uint32_t srvIndex = descriptorHeaps->Allocate(PartitionType::Texture);
    m_context->Logging->Info("[SlotDBG] CreateSkybox Allocate srvIndex={}", srvIndex);

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

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, srvIndex);
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

    m_skyboxGeometryHandle = m_context->GeometryResourceManager->RegisterGeometry<TriangleMesh>(skyTriangleMesh);

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
    // SystemRegistry::Register({.name = "CubeRotationSystem",
    //                           .func =
    //                               [this](Registry &registry, const MessageContext &ctx) {
    //                                   float deltaTime = m_context->MainTimer->GetDeltaTime();
    //                                   // 旋转所有立方体
    //                                   for (auto &entity : m_cubeEntities) {
    //                                       if (entity == INVALID_ENTITY)
    //                                           continue;
    //                                       auto *transform = registry.TryGetComponent<TransformComponent>(entity);
    //                                       if (transform) {
    //                                           transform->rotation.y += deltaTime * 2.0f;
    //                                       }
    //                                   }
    //                               },
    //                           .phase = TaskPhase::Update,
    //                           .threadType = ThreadType::Worker,
    //                           .priority = TaskPriority::Normal,
    //                           .alwaysRun = true});
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
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};

                 //  一个堆
                 cmdList.Get()->SetDescriptorHeaps(1, descriptorHeaps);

                 // 获取反射探针 Cubemap Array SRV
                 D3D12_GPU_DESCRIPTOR_HANDLE cubemapArraySRV = m_context->ReflectionProbeMgr->GetProbeCubemapArraySRV();

                 // 获取纹理数组堆起始 GPU handle（TextureSrv 分区起始）
                 D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart =
                     m_context->DescriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, 0);

                 // 获取环境贴图 SRV（天空盒）
                 D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV = {};
                 if (m_skyboxTextureHandle.IsValid()) {
                     envMapSRV = m_context->TextureMgr->GetSRV(m_skyboxTextureHandle);
                 }

                 // 获取 SSAO Map SRV（若未初始化则为空 handle，shader 中跳过）
                 D3D12_GPU_DESCRIPTOR_HANDLE aoMapSRV = AmbientOcclusionManager::GetInstance().GetAmbientMapSRV();

                 // 开始渲染（传入阴影数据、反射探针Cubemap Array SRV、纹理堆起始、环境贴图、SSAO Map）
                 m_renderer->BeginFrame(cmdList, passCBAddr, lightCBAddr, materialBufferSRV, shadowDataSRV,
                                        shadowMapSRV, cubemapArraySRV, textureHeapStart, envMapSRV, aoMapSRV);

                 for (const auto &item : m_opaqueQueue) {
                     if (!item.IsValid())
                         continue;
                     m_renderer->DrawInstanced(cmdList, item.geometryHandle, item.instanceBuffer, item.instanceCount);
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
                     m_context->Logging->Info("[SkyboxRenderSystem] skyRenderer or skyboxGeometryHandle is null");
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
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
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

    // 3. 构建 GridGeometry（水面使用规则网格几何体）
    GridGeometry waterMesh;
    waterMesh.vertexBufferHandle = vbHandle;
    waterMesh.indexBufferHandle = ibHandle;
    waterMesh.vertexCount = static_cast<uint32_t>(meshData.Vertices.size());
    waterMesh.indexCount = static_cast<uint32_t>(meshData.Indices32.size());
    waterMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
    waterMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    waterMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    waterMesh.isGpuReady = true;
    waterMesh.widthSegments = 64;
    waterMesh.depthSegments = 64;

    // 计算包围盒
    BoundingAABB bounds;
    bounds.min = XMFLOAT3(-128.0f, 0.0f, -128.0f);
    bounds.max = XMFLOAT3(128.0f, 0.1f, 128.0f);

    auto &geoMgr = m_context->GeometryResourceManager;
    GeometryHandle geoHandle = geoMgr->RegisterGeometry<GridGeometry>(waterMesh);

    // 4. 创建 LODMesh
    LODMesh lodMesh;
    lodMesh.lodChain = {geoHandle};
    LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

    // 5. 创建实体
    m_waterEntity = m_registry->CreateEntity();

    XMFLOAT3 position(0.0f, 10.0f, 0.0f); // 水面在地形上方，确保可见
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

    // m_registry->AddComponent<StaticComponent>(m_waterEntity);

    // [SSAO开发期间注释] 绘制调用
    // RegisterWaterRenderSystem();
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
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
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
    uint32_t srvIndex = descriptorHeaps->Allocate(PartitionType::Texture);

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

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, srvIndex);
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
    // H_Source_heightmap.png: 1280 灰度图，用于 CPU 端顶点生成
    // H_Runtime_heightmap.dds: 256x256 DDS，用于 GPU 端 DS 顶点置换
    // D_heightmap.dds: 1280x1280 DDS，用于 PS 漫反射采样
    auto loadTask = Async::TerrainLoadTaskFactory::Create(requestId, L"Content/Terrain/H_Source_heightmap.png", 256.0f,
                                                          256.0f, 20.0f, 256, terrainData, input);
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
                 uint32_t indexCount = static_cast<uint32_t>(m_terrainLoadData->indices.size());

                 Resource::PatchMesh mesh;
                 mesh.vertexBufferHandle = state.vbHandle;
                 mesh.indexBufferHandle = state.ibHandle;
                 mesh.vertexCount = static_cast<uint32_t>(m_terrainLoadData->vertices.size());
                 mesh.indexCount = indexCount;
                 mesh.patchCount = indexCount / 4; // 四边形面片，4 索引/patch
                 mesh.vertexStride = sizeof(GeometryGenerator::Vertex);
                 mesh.indexFormat = DXGI_FORMAT_R32_UINT;
                 mesh.patchType = Resource::PatchType::Quad;
                 mesh.isGpuReady = true;
                 mesh.localBounds = state.bounds;

                 auto geoMgr = m_context->GeometryResourceManager;
                 auto handle = geoMgr->RegisterGeometry<PatchMesh>(mesh);
                 m_terrainGeometryHandle = handle;
                 m_context->Logging->Info("[TerrainGPUCreate] Geometry registered: handle(idx={}, gen={})",
                                          handle.index, handle.generation);

                 // ── 处理纹理：分配 SRV + 创建 SRV + 注册 TextureHandle ──
                 // BackgroundExecutor 已保证 GPU 工作完成（COPY + DIRECT + fence wait）
                 //
                 // 地形纹理绑定到 space2 t0，与实体纹理 (space0 t0) 完全隔离。
                 // 分配在同一个 CbvSrvUav 堆中，但 shader 通过不同 register space 区分。
                 // 根签名固定为 3 个连续槽位 [0]=高度图 [1]=漫反射 [2]=法线贴图
                 // 必须始终用 AllocateConsecutive(3)，即使某个纹理暂不可用也要占住槽位
                 if ((!m_terrainTextureHandle.IsValid() || !m_terrainAlbedoHandle.IsValid() ||
                      !m_terrainNormalHandle.IsValid()) &&
                     state.heightMapCreated.load(std::memory_order_acquire) &&
                     state.albedoCreated.load(std::memory_order_acquire) &&
                     state.normalCreated.load(std::memory_order_acquire)) {

                     auto &descriptorHeaps = m_context->DescriptorHeaps;
                     ID3D12Device *device = m_context->DeviceContext->GetDevice();
                     auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
                     auto *texMgr = m_context->TextureMgr;

                     bool hasHeight =
                         state.heightMapCreated.load(std::memory_order_acquire) && state.heightMapGpuHandle.IsValid();
                     bool hasAlbedo =
                         state.albedoCreated.load(std::memory_order_acquire) && state.albedoGpuHandle.IsValid();
                     bool hasNormal =
                         state.normalCreated.load(std::memory_order_acquire) && state.normalMapGpuHandle.IsValid();

                     // 始终分配连续 3 个槽位
                     uint32_t baseSrvIdx = descriptorHeaps->AllocateConsecutive(PartitionType::Texture, 3);
                     m_context->Logging->Info("[SlotDBG] TerrainGPUCreate AllocateConsecutive(3) baseSrvIdx={} "
                                              "(hasHeight={} hasAlbedo={} hasNormal={})",
                                              baseSrvIdx, hasHeight, hasAlbedo, hasNormal);

                     if (baseSrvIdx != UINT32_MAX) {
                         // 创建高度图 SRV (slot 0)
                         if (hasHeight && !m_terrainTextureHandle.IsValid()) {
                             D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                             srvDesc.Format = state.heightMapDesc.Format;
                             srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                             srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                             srvDesc.Texture2D.MostDetailedMip = 0;
                             srvDesc.Texture2D.MipLevels = state.heightMapDesc.MipLevels;

                             D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
                                 descriptorHeaps->GetCpuHandle(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, baseSrvIdx);
                             device->CreateShaderResourceView(gpuMgr.GetResource(state.heightMapGpuHandle), &srvDesc,
                                                              cpuHandle);

                             m_terrainTextureHandle = texMgr->RegisterTexture(state.heightMapGpuHandle, baseSrvIdx);
                         }

                         // 创建漫反射 SRV (slot 1)
                         if (hasAlbedo && !m_terrainAlbedoHandle.IsValid()) {
                             D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                             srvDesc.Format = state.albedoDesc.Format;
                             srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                             srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                             srvDesc.Texture2D.MostDetailedMip = 0;
                             srvDesc.Texture2D.MipLevels = state.albedoDesc.MipLevels;

                             D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
                                 descriptorHeaps->GetCpuHandle(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, baseSrvIdx + 1);
                             device->CreateShaderResourceView(gpuMgr.GetResource(state.albedoGpuHandle), &srvDesc,
                                                              cpuHandle);

                             m_terrainAlbedoHandle = texMgr->RegisterTexture(state.albedoGpuHandle, baseSrvIdx + 1);
                         }

                         // 创建法线贴图 SRV (slot 2)
                         if (hasNormal && !m_terrainNormalHandle.IsValid()) {
                             D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                             srvDesc.Format = state.normalMapDesc.Format;
                             srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                             srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                             srvDesc.Texture2D.MostDetailedMip = 0;
                             srvDesc.Texture2D.MipLevels = state.normalMapDesc.MipLevels;

                             D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
                                 descriptorHeaps->GetCpuHandle(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, baseSrvIdx + 2);
                             device->CreateShaderResourceView(gpuMgr.GetResource(state.normalMapGpuHandle), &srvDesc,
                                                              cpuHandle);

                             m_terrainNormalHandle = texMgr->RegisterTexture(state.normalMapGpuHandle, baseSrvIdx + 2);
                         }

                         m_context->Logging->Info(
                             "[TerrainGPUCreate] Registered: heightMap(idx={}) albedo(idx={}) normal(idx={})",
                             m_terrainTextureHandle.index, m_terrainAlbedoHandle.index, m_terrainNormalHandle.index);
                     } else {
                         m_context->Logging->Error("[TerrainGPUCreate] Failed to allocate consecutive SRVs");
                     }
                 } else if (m_terrainTextureHandle.IsValid() && m_terrainAlbedoHandle.IsValid() &&
                            m_terrainNormalHandle.IsValid()) {
                     m_context->Logging->Info("[TerrainGPUCreate] Terrain textures already loaded, skipping");
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

                 // 创建 ECS 实体
                 auto entity = reg.CreateEntity();
                 m_terrainEntity = entity;

                 XMFLOAT3 position(0.0f, -50.0f, 0.0f);
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

                 ECS::TerrainComponent terrainComp;
                 terrainComp.geometryHandle = m_terrainGeometryHandle;
                 terrainComp.heightMapHandle = m_terrainTextureHandle;
                 terrainComp.albedoHandle = m_terrainAlbedoHandle;
                 terrainComp.normalHandle = m_terrainNormalHandle;
                 terrainComp.localBounds = bounds;
                 terrainComp.heightScale = m_terrainLoadData ? m_terrainLoadData->maxHeight : 20.0f;
                 reg.AddComponent<ECS::TerrainComponent>(entity, std::move(terrainComp));

                 //  reg.AddComponent<ECS::StaticComponent>(entity);

                 m_context->Logging->Info("[TerrainCombine] Entity created: entity={}, request={}, geoHandle={}, "
                                          "heightMapHandle={}, albedoHandle={}",
                                          static_cast<uint32_t>(entity), requestId, m_terrainGeometryHandle.index,
                                          m_terrainTextureHandle.index, m_terrainAlbedoHandle.index);

                 // 清理状态（句柄已保存到成员变量，不再需要 readyState）
                 m_terrainReadyState.reset();
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Main,
         .priority = TaskPriority::Normal,
         .interestedMessages = {static_cast<uint32_t>(Event::EventType::TerrainReadyEvent)}});
}

void GameWorld::RegisterTerrainRenderSystem() {
    SystemRegistry::Register(
        {.name = "TerrainRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 if (m_terrainQueue.Empty()) {
                     return;
                 }

                 // 获取命令列表
                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 // 设置视口、裁剪矩形、渲染目标（复用 Opaque 阶段的深度缓冲区）
                 auto backBuffer = m_context->GetBackBuffer();
                 auto rtvHandle = m_context->DeviceContext->GetCurrentBackBufferView();
                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();

                 // 屏障：PRESENT -> RENDER_TARGET（Opaque 阶段结束时已将 BackBuffer 转为 PRESENT）
                 D3D12_RESOURCE_BARRIER beginBarrier = {};
                 beginBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 beginBarrier.Transition.pResource = backBuffer;
                 beginBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                 beginBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 beginBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &beginBarrier);

                 const auto &viewport = m_context->DeviceContext->GetViewport();
                 const auto &scissorRect = m_context->DeviceContext->GetScissorRect();
                 cmdList.Get()->RSSetViewports(1, &viewport);
                 cmdList.Get()->RSSetScissorRects(1, &scissorRect);
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 // 设置描述符堆
                 ID3D12DescriptorHeap *descriptorHeaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 cmdList.Get()->SetDescriptorHeaps(1, descriptorHeaps);

                 // 获取通用资源
                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 D3D12_GPU_VIRTUAL_ADDRESS lightCBAddr = LightManager::GetInstance().GetLightCBAddress();
                 D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV = m_context->MaterialMgr->GetMaterialBufferSRV();

                 // 获取阴影资源
                 auto &lightMgr = LightManager::GetInstance();
                 D3D12_GPU_DESCRIPTOR_HANDLE shadowDataSRV = lightMgr.GetShadowDataSRV();
                 D3D12_GPU_DESCRIPTOR_HANDLE shadowMapSRV = lightMgr.GetShadowMapSRV();

                 // 开始地形渲染
                 m_terrainRenderer->BeginFrame(cmdList, passCBAddr, lightCBAddr, materialBufferSRV, shadowDataSRV,
                                               shadowMapSRV);

                 // 遍历地形队列
                 static int s_dbgTerrainCount = 0;
                 for (const auto &item : m_terrainQueue.GetItems()) {
                     if (!item.IsValid())
                         continue;
                     if (s_dbgTerrainCount++ < 3) {
                         m_context->Logging->Info("[DrawDBG] DrawTerrain texTableSRV.ptr=0x{:X}", item.texTableSRV.ptr);
                     }
                     m_terrainRenderer->DrawTerrain(cmdList, item);
                 }

                 m_terrainRenderer->EndFrame();

                 // 屏障：RENDER_TARGET -> PRESENT
                 D3D12_RESOURCE_BARRIER endBarrier = {};
                 endBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 endBarrier.Transition.pResource = backBuffer;
                 endBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 endBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 endBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &endBarrier);

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

void GameWorld::RegisterClearSystem() {
    SystemRegistry::Register(
        {.name = "ClearRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 // ================================================================
                 // 获取命令列表
                 // ================================================================
                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 auto backBuffer = m_context->GetBackBuffer();

                 // ================================================================
                 // 屏障：Present -> RenderTarget
                 // ================================================================
                 D3D12_RESOURCE_BARRIER beginBarrier = {};
                 beginBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 beginBarrier.Transition.pResource = backBuffer;
                 beginBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                 beginBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 beginBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &beginBarrier);

                 // ================================================================
                 // 设置视口和渲染目标
                 // ================================================================
                 const auto &viewport = m_context->DeviceContext->GetViewport();
                 const auto &scissorRect = m_context->DeviceContext->GetScissorRect();
                 cmdList.Get()->RSSetViewports(1, &viewport);
                 cmdList.Get()->RSSetScissorRects(1, &scissorRect);

                 auto rtvHandle = m_context->DeviceContext->GetCurrentBackBufferView();
                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 // ================================================================
                 // 清除 RTV + DSV
                 // ================================================================
                 const float clearColor[] = {0.0f, 0.2f, 0.4f, 1.0f};
                 cmdList.Get()->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
                 cmdList.Get()->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                                      1.0f, 0, 0, nullptr);

                 // ================================================================
                 // 屏障：RenderTarget -> Present（为后续 PrePass 恢复 Present 状态）
                 // 注意：Opaque pass 会再做 Present->RT->Present 转换，因此这里需要回到 Present
                 // ================================================================
                 D3D12_RESOURCE_BARRIER endBarrier = {};
                 endBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 endBarrier.Transition.pResource = backBuffer;
                 endBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 endBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 endBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &endBarrier);

                 // ================================================================
                 // 关闭并提交
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

void GameWorld::RegisterShadowRenderSystem() {
    SystemRegistry::Register(
        {.name = "ShadowRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 auto &lightMgr = LightManager::GetInstance();
                 D3D12_GPU_VIRTUAL_ADDRESS dirShadowAddr = lightMgr.GetDirShadowAddress();
                 const auto &shadowRes = lightMgr.GetDirShadowResources();

                 // 检查方向光阴影是否可用
                 if (!lightMgr.HasDirShadow() || !shadowRes.isValid || dirShadowAddr == 0 || m_opaqueQueue.Empty()) {
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
                     m_context->DescriptorHeaps->GetCpuHandle(PartitionType::Dsv, shadowRes.dsvSlot);

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
                 m_shadowRenderer->BeginOffscreen(cmdList, dirShadowAddr, dsvHandle, shadowRes.resolution,
                                                  shadowRes.resolution);

                 // ================================================================
                 // 遍历队列中的物体，绘制阴影（统一实例化模式）
                 // ================================================================
                 for (const auto &item : m_opaqueQueue) {
                     if (!item.IsValid())
                         continue;
                     m_shadowRenderer->DrawInstanced(cmdList, item.geometryHandle, item.instanceBuffer,
                                                     item.instanceCount);
                 }

                 // ================================================================
                 // 结束阴影 Pass
                 // ================================================================
                 m_shadowRenderer->EndOffscreen(cmdList);

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

void GameWorld::RegisterSsaoSystem() {
    SystemRegistry::Register(
        {.name = "SsaoSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 auto &aoMgr = AmbientOcclusionManager::GetInstance();
                 if (!aoMgr.IsInitialized())
                     return;

                 // 获取命令列表
                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 // 获取深度缓冲 SRV（私有深度缓冲，DrawNormals 写入后供 ComputeAO 采样）
                 D3D12_GPU_DESCRIPTOR_HANDLE depthSRV = aoMgr.GetPrivateDepthSRV();

                 // 过渡正常和 AO RT 到 RENDER_TARGET 状态
                 auto *normalRes = aoMgr.GetNormalResource();
                 auto *ambient0 = aoMgr.GetAmbientResource0();
                 auto *ambient1 = aoMgr.GetAmbientResource1();
                 auto barrier = [&](ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
                     if (!res)
                         return;
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, from, to);
                     cmdList.Get()->ResourceBarrier(1, &b);
                 };
                 barrier(normalRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                 barrier(ambient0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                 barrier(ambient1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

                 // ── 法线/深度 Pass（仿龙书 DrawNormalsAndDepth，使用私有 DSV）──
                 D3D12_CPU_DESCRIPTOR_HANDLE normalRTV = aoMgr.GetNormalMapRTV();
                 D3D12_CPU_DESCRIPTOR_HANDLE depthDSV = aoMgr.GetPrivateDepthDSV();
                 auto *nativeCL = cmdList.Get();

                 // 设置视口、裁剪矩形
                 {
                     const auto &viewport = m_context->DeviceContext->GetViewport();
                     const auto &scissorRect = m_context->DeviceContext->GetScissorRect();
                     nativeCL->RSSetViewports(1, &viewport);
                     nativeCL->RSSetScissorRects(1, &scissorRect);
                 }

                 // 龙书模式：先清除 RTV 和 DSV（不依赖 OMSetRenderTargets），再绑定
                 const float clearBlue[] = {0.0f, 0.0f, 1.0f, 0.0f};
                 nativeCL->ClearRenderTargetView(normalRTV, clearBlue, 0, nullptr);
                 nativeCL->ClearDepthStencilView(depthDSV, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0,
                                                 0, nullptr);

                 // 再绑定法线 RT + 深度缓冲
                 nativeCL->OMSetRenderTargets(1, &normalRTV, TRUE, &depthDSV);

                 // 用法线 PSO 绘制场景几何体（同时写入法线和深度）
                 if (ID3D12PipelineState *normPSO = aoMgr.GetNormalPipeline()) {
                     nativeCL->SetPipelineState(normPSO);
                     nativeCL->SetGraphicsRootSignature(aoMgr.GetNormalRootSig());

                     const auto &passCB = m_context->FrameResourceManager->GetPassConstants();
                     // PassConstants 完整布局，着色器自行索引到 gViewProj
                     D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_context->FrameResourceManager->GetPassCBAddress();
                     nativeCL->SetGraphicsRootConstantBufferView(0, cbAddr);

                     auto &gpuMgr = GpuResourceManager::GetInstance();
                     for (const auto &item : m_opaqueQueue) {
                         if (!item.IsValid())
                             continue;
                         const TriangleMesh *mesh =
                             m_context->GeometryResourceManager->GetGeometry<TriangleMesh>(item.geometryHandle);
                         if (!mesh || !mesh->isGpuReady)
                             continue;

                         ID3D12Resource *vb = gpuMgr.GetResource(mesh->vertexBufferHandle);
                         ID3D12Resource *ib = gpuMgr.GetResource(mesh->indexBufferHandle);
                         if (!vb || !ib)
                             continue;

                         D3D12_VERTEX_BUFFER_VIEW vbv = {vb->GetGPUVirtualAddress(),
                                                         (UINT)(mesh->vertexCount * mesh->vertexStride),
                                                         mesh->vertexStride};
                         D3D12_INDEX_BUFFER_VIEW ibv = {
                             ib->GetGPUVirtualAddress(),
                             (UINT)(mesh->indexCount * (mesh->indexFormat == DXGI_FORMAT_R32_UINT ? 4 : 2)),
                             mesh->indexFormat};
                         nativeCL->IASetVertexBuffers(0, 1, &vbv);
                         nativeCL->IASetIndexBuffer(&ibv);
                         nativeCL->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                         nativeCL->SetGraphicsRootShaderResourceView(1, item.instanceBuffer);
                         nativeCL->DrawIndexedInstanced(mesh->indexCount, item.instanceCount, 0, 0, 0);
                     }
                 }

                 // 法线 RT 转回可读状态，供 ComputeAO 采样
                 barrier(normalRes, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

                 // 私有深度缓冲从 DEPTH_WRITE 转 PIXEL_SHADER_RESOURCE，供 ComputeAO 采样法线/深度
                 ID3D12Resource *privateDepthRes = aoMgr.GetPrivateDepthResource();
                 if (privateDepthRes) {
                     barrier(privateDepthRes, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                 }

                 // 执行 SSAO（仅 Compute + Blur，法线/深度已就绪）
                 const auto &passCB = m_context->FrameResourceManager->GetPassConstants();
                 aoMgr.Execute(cmdList.Get(), depthSRV, passCB.Proj);

                 // 私有深度缓冲转回 DEPTH_WRITE，供下一帧 DrawNormals 清除使用
                 if (privateDepthRes) {
                     barrier(privateDepthRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
                 }

                 // 过渡 AO RT 回 PIXEL_SHADER_RESOURCE（ambientRT1 已在 Execute 内部转回 PS）
                 barrier(ambient0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::DynamicAOcclusion, cmdListHandle);

                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::DynamicAOcclusion,
         .alwaysRun = true});
}

void GameWorld::RegisterProbeCaptureSystem() {
    SystemRegistry::Register(
        {.name = "ProbeCaptureSystem",
         .func =
             [this](Registry &, const MessageContext &) {
                 if (!m_probeRenderer || m_activeProbeCount == 0)
                     return;

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 ID3D12DescriptorHeap *heaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 cmdList.Get()->SetDescriptorHeaps(1, heaps);

                 D3D12_GPU_VIRTUAL_ADDRESS lightCBAddr = LightManager::GetInstance().GetLightCBAddress();
                 D3D12_GPU_DESCRIPTOR_HANDLE matBufferSRV = m_context->MaterialMgr->GetMaterialBufferSRV();

                 for (uint32_t i = 0; i < m_activeProbeCount; ++i) {
                     if (m_probeQueues[i].Empty() || m_probeCaptureInfo[i].rtvBaseSlot == UINT32_MAX)
                         continue;
                     const auto &info = m_probeCaptureInfo[i];
                     D3D12_CPU_DESCRIPTOR_HANDLE depthDSV =
                         m_context->DescriptorHeaps->GetCpuHandle(PartitionType::Dsv, info.dsvSlot);

                     XMVECTOR probePos = XMLoadFloat3(&info.position);
                     const auto &mainPass = m_context->FrameResourceManager->GetPassConstants();

                     { // GS_SINGLE_PASS
                         D3D12_GPU_VIRTUAL_ADDRESS captureCBAddr = info.captureCBAddress;
                         if (captureCBAddr == 0)
                             continue;
                         D3D12_CPU_DESCRIPTOR_HANDLE cubemapRTV =
                             m_context->DescriptorHeaps->GetCpuHandle(PartitionType::Rtv, info.rtvBaseSlot);
                         D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart =
                             m_context->DescriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, 0);
                         m_probeRenderer->BeginCapture(cmdList, info.cubemapResource, cubemapRTV, depthDSV,
                                                       info.resolution, info.resolution, captureCBAddr, lightCBAddr,
                                                       matBufferSRV, textureHeapStart);
                         // Draw queued geometry for this face
                         for (const auto &item : m_probeQueues[i]) {
                             if (!item.IsValid())
                                 continue;
                             m_probeRenderer->DrawInstanced(cmdList, item.geometryHandle, item.instanceBuffer,
                                                            item.instanceCount);
                         }
                         m_probeRenderer->EndCapture(cmdList);
                     }
                 }

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

// ============================================================================
// 公告牌（Billboard）系统
// ============================================================================

void GameWorld::LoadBillboardTextures() {
    // ========================================================================
    // 直接加载 treearray.dds 作为 Texture2DArray
    // treearray.dds 是用工具打包好的纹理数组，内部已包含多个切片
    // 着色器通过 Texture2DArray.Sample(float3(uv, sliceIndex)) 采样
    // ========================================================================

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Device *device = m_context->DeviceContext->GetDevice();
    auto &descriptorHeaps = m_context->DescriptorHeaps;
    TextureManager *texMgr = m_context->TextureMgr;

    // Step 1: 解析 DDS
    DDSTextureInfo ddsInfo;
    if (!AssetLoader::GetInstance().LoadTextureFromFile(L"Content/Textures/treearray.dds", ddsInfo)) {
        m_context->Logging->Error("[GameWorld] Failed to load billboard texture treearray.dds");
        return;
    }

    D3D12_RESOURCE_DESC &desc = ddsInfo.desc;
    uint32_t arraySize = static_cast<uint32_t>(desc.DepthOrArraySize);
    m_billboardTotalSlices = arraySize;

    m_context->Logging->Info("[GameWorld] Billboard texture loaded: {}x{}, format={}, mips={}, arraySize={}",
                             desc.Width, desc.Height, static_cast<int>(desc.Format), desc.MipLevels, arraySize);

    // Step 2: 创建 GPU 资源（直接使用 DDS 解析出的 desc，格式/尺寸完全匹配）
    GpuResourceHandle gpuHandle = gpuMgr.CreateTexture2D(device, desc, D3D12_RESOURCE_STATE_COMMON);
    if (!gpuHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] Failed to create billboard Texture2DArray");
        return;
    }

    // Step 3: 分配 SRV
    uint32_t srvIndex = descriptorHeaps->Allocate(PartitionType::Texture);
    m_context->Logging->Info("[SlotDBG] LoadBillboardTextures Allocate srvIndex={}", srvIndex);
    if (srvIndex == UINT32_MAX) {
        m_context->Logging->Error("[GameWorld] Failed to allocate SRV slot for billboard Texture2DArray");
        return;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = desc.MipLevels;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = arraySize;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, srvIndex);
    device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);

    // 注册到 TextureManager
    TextureHandle texHandle = texMgr->RegisterTexture(gpuHandle, srvIndex);

    // Step 4: 上传纹理数据
    uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
    auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
    auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
    auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

    // 屏障: COMMON → COPY_DEST
    auto barrier1 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COMMON,
                                                         D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList.Get()->ResourceBarrier(1, &barrier1);

    UINT64 requiredSize =
        GetRequiredIntermediateSize(gpuMgr.GetResource(gpuHandle), 0, static_cast<UINT>(ddsInfo.subresources.size()));
    GpuResourceHandle uploadHandle =
        gpuMgr.CreateBuffer(device, requiredSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

    UpdateSubresources(cmdList.Get(), gpuMgr.GetResource(gpuHandle), gpuMgr.GetResource(uploadHandle), 0, 0,
                       static_cast<UINT>(ddsInfo.subresources.size()), ddsInfo.subresources.data());

    // 屏障: COPY_DEST → PIXEL_SHADER_RESOURCE
    auto barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COPY_DEST,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList.Get()->ResourceBarrier(1, &barrier2);

    cmdList.Close();
    m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList);
    m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);

    uint64_t releaseSequence = m_context->GetNextSequence();
    gpuMgr.Release(uploadHandle, releaseSequence);
    m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);
    m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, releaseSequence);

    // Step 5: 存储句柄（所有实体共享同一个 Texture2DArray，通过 textureArrayIndex 区分切片）
    for (int i = 0; i < 4; ++i) {
        m_billboardTextureHandles[i] = texHandle;
    }

    // sliceOffsets: 每个逻辑纹理在数组中的起始索引（全部指向同一个数组的不同切片范围）
    // 现在只有一个纹理数组，但为保持兼容性，sliceOffsets[0]=0
    for (int i = 0; i < 4; ++i) {
        m_billboardSliceOffsets[i] = 0;
    }

    m_context->Logging->Info("[GameWorld] Billboard Texture2DArray created: {}x{}, {} slices, SRV index {}", desc.Width,
                             desc.Height, arraySize, srvIndex);
}

void GameWorld::CreateBillboardTrees() {
    if (!m_registry || !m_context) {
        return;
    }

    // 初始化公告牌渲染器
    m_billboardRenderer = std::make_unique<BillboardRenderer>();
    m_billboardRenderer->SetDeviceContext(m_context->DeviceContext);
    m_billboardRenderer->Initialize();

    // 初始化公告牌渲染项构建器
    m_billboardBuilder = std::make_unique<BillboardRenderItemBuilder>(m_context->FrameResourceManager,
                                                                      m_context->TextureMgr, m_context->MaterialMgr);

    // 在场景中随机放置树公告牌实体
    // 平面在 Y=10.0f，公告牌底部与平面齐平（公告牌中心 Y = 平面Y + 高度/2）
    constexpr float PLANE_Y = 30.0f;
    constexpr int BILLBOARD_GRID_HALF = 5; // 50×50 = 2500 棵公告牌树
    constexpr float BILLBOARD_SPACING = 2.0f;

    m_billboardEntities.clear();
    m_billboardEntities.reserve((BILLBOARD_GRID_HALF * 2) * (BILLBOARD_GRID_HALF * 2));

    for (int x = -BILLBOARD_GRID_HALF; x < BILLBOARD_GRID_HALF; ++x) {
        for (int z = -BILLBOARD_GRID_HALF; z < BILLBOARD_GRID_HALF; ++z) {
            // 跳过引用无效纹理
            if (!m_billboardTextureHandles[0].IsValid()) {
                continue;
            }

            auto entity = m_registry->CreateEntity();

            float bx = x * BILLBOARD_SPACING;
            float bz = z * BILLBOARD_SPACING;
            float bw = 2.0f + (x * z) % 3 * 0.4f; // 随机变化宽度
            float bh = 4.0f + (x + z) % 5 * 0.5f; // 随机变化高度

            XMFLOAT3 position(bx, PLANE_Y + bh * 0.5f, bz);
            XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
            XMFLOAT3 scale(1.0f, 1.0f, 1.0f);
            m_registry->AddComponent<TransformComponent>(entity, position, rotation, scale);

            BillboardComponent billboardComp;
            billboardComp.textureHandle = m_billboardTextureHandles[0];
            billboardComp.materialHandle = m_billboardMaterialHandle;
            billboardComp.width = bw;
            billboardComp.height = bh;
            billboardComp.mode = BillboardMode::AxisY;
            billboardComp.minDistance = 0.5f;
            billboardComp.maxDistance = 500.0f;
            billboardComp.switchDistance = 50.0f;

            // textureArrayIndex 循环使用 Texture2DArray 中的切片
            int idx = (x - BILLBOARD_GRID_HALF) * (BILLBOARD_GRID_HALF * 2) + (z - BILLBOARD_GRID_HALF);
            billboardComp.textureArrayIndex = idx % m_billboardTotalSlices;

            m_registry->AddComponent<BillboardComponent>(entity, std::move(billboardComp));

            // 公告牌是静态的，用持久化 Instance Buffer
            // m_registry->AddComponent<StaticComponent>(entity);

            m_billboardEntities.push_back(entity);
        }
    }

    // 注册公告牌渲染系统
    RegisterBillboardRenderSystem();

    m_context->Logging->Info("[GameWorld] {} billboard trees created ({} texture slices)", m_billboardEntities.size(),
                             m_billboardTotalSlices);
}

void GameWorld::RegisterBillboardRenderSystem() {
    SystemRegistry::Register(
        {.name = "BillboardRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 if (m_billboardQueue.Empty()) {
                     return;
                 }

                 // 获取命令列表
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
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 cmdList.Get()->SetDescriptorHeaps(1, descriptorHeaps);

                 // 获取 Pass Constant Buffer 和 Light CB 地址
                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 D3D12_GPU_VIRTUAL_ADDRESS lightCBAddr = LightManager::GetInstance().GetLightCBAddress();
                 D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV = m_context->MaterialMgr->GetMaterialBufferSRV();

                 // 获取纹理数组堆起始 GPU handle（TextureSrv 分区起始）
                 D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart =
                     m_context->DescriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, 0);

                 // 获取公告牌 Texture2DArray 的 SRV（slot 5: t20）
                 D3D12_GPU_DESCRIPTOR_HANDLE billboardTexSRV =
                     m_context->TextureMgr->GetSRV(m_billboardTextureHandles[0]);

                 // 开始公告牌渲染
                 m_billboardRenderer->BeginFrame(cmdList, passCBAddr, lightCBAddr, materialBufferSRV, textureHeapStart,
                                                 billboardTexSRV);

                 // 遍历公告牌队列
                 for (const auto &item : m_billboardQueue.GetItems()) {
                     if (!item.IsValid())
                         continue;
                     m_billboardRenderer->DrawBillboard(cmdList, item);
                 }

                 // 屏障：转换回 PRESENT 状态
                 barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Billboard, cmdListHandle);

                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Billboard,
         .alwaysRun = true});
}

void GameWorld::Update() {
    if (m_backgroundExecutor) {
        m_backgroundExecutor->Tick();
    }
}