#include "GameWorld.h"
#include "Asset/IO/Loader/SceneLoader.h"
#include "Background/BackgroundExecutor.h"
#include "Boot/GameContext.h"
#include "Common/ThrowHelper.h"
#include "Common/d3dUtil.h"
#include "ECS/Core/Registry.h"
#include "Logger/Logger.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/Effects/AO/AmbientOcclusionManager.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/Material/MaterialManager.h"
#include "Renderer/Material/MaterialResource.h"
#include "Renderer/Pipeline/BillboardRenderer.h"
#include "Renderer/Pipeline/OpaqueRenderer.h"
#include "Renderer/Pipeline/ShadowRenderer.h"
#include "Renderer/Pipeline/SkyRenderer.h"
#include "Renderer/Pipeline/TerrainRenderer.h"
#include "Renderer/Pipeline/WaterRenderer.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/RHI/Command/Fence/FenceManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/RenderItemBuilder/OpaqueRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/TerrainRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/TransparentRenderItemBuilder.h"
#include "Renderer/Scene/LightManager/LightManager.h"
#include "Renderer/Scene/TerrainManager/TerrainManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Pool/DepthStencilPool.h"
#include "Resource/Pool/RenderTargetPool.h"
#include "Resource/Texture/TextureManager.h"
#include "Scene/SceneConstructor.h"
#include "Scheduler/FrameDriver.h"
#include <DirectXMath.h>
#include <wrl/client.h>

using namespace DirectX;
using namespace DX12Engine;
using namespace DX12Engine::Async;
using namespace DX12Engine::Boot;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::Scheduler;
using namespace DX12Engine::Math;

// ========================================================================
// GameWorld — 核心生命周期
// ========================================================================

GameWorld::GameWorld() = default;
GameWorld::~GameWorld() = default;

void GameWorld::Initialize(GameContext *context, OpaqueRenderer *renderer) {
    m_context = context;
    m_registry = context->Registry;
    m_renderer = renderer;

    m_opaqueBuilder = std::make_unique<OpaqueRenderItemBuilder>(m_context->FrameResourceManager, m_context->MaterialMgr,
                                                                m_context->TextureMgr);

    m_transparentBuilder = std::make_unique<TransparentRenderItemBuilder>(
        m_context->FrameResourceManager, m_context->MaterialMgr, m_context->TextureMgr, m_context->CameraMgr);

    m_waterRenderer = std::make_unique<WaterRenderer>();
    m_waterRenderer->SetDeviceContext(m_context->DeviceContext);
    m_waterRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_waterRenderer->SetMaterialManager(m_context->MaterialMgr);
    m_waterRenderer->Initialize();

    m_shadowRenderer = std::make_unique<ShadowRenderer>();
    m_shadowRenderer->SetDeviceContext(m_context->DeviceContext);
    m_shadowRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_shadowRenderer->Initialize();

    m_terrainRenderer = std::make_unique<TerrainRenderer>();
    m_terrainRenderer->SetDeviceContext(m_context->DeviceContext);
    m_terrainRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_terrainRenderer->SetMaterialManager(m_context->MaterialMgr);
    m_terrainRenderer->Initialize();

    TerrainManager::GetInstance().Initialize(m_context->DeviceContext->GetDevice());

    m_terrainBuilder =
        std::make_unique<TerrainRenderItemBuilder>(m_context->FrameResourceManager, m_context->TextureMgr);

    m_probeBuilder =
        std::make_unique<ProbeBuilder>(m_context->FrameResourceManager, m_context->MaterialMgr, m_context->TextureMgr);
    m_probeRenderer = std::make_unique<ReflectionProbeRenderer>();
    m_probeRenderer->SetDeviceContext(m_context->DeviceContext);
    m_probeRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_probeRenderer->SetMaterialManager(m_context->MaterialMgr);
    m_probeRenderer->Initialize();
    // LoadWaterTexture(); // 已迁移

    // 创建 1x1 纯白纹理
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

        GpuResourceHandle whiteTexHandle =
            gpuMgr.CreateTexture2D(device, whiteDesc, L"WhiteTexture", D3D12_RESOURCE_STATE_COMMON);
        if (whiteTexHandle.IsValid()) {
            uint32_t whiteSrvSlot = m_context->DescriptorHeaps->Allocate(PartitionType::Texture);
            if (whiteSrvSlot != UINT32_MAX) {
                uint32_t whitePixel = 0xFFFFFFFFu;
                D3D12_SUBRESOURCE_DATA subData = {};
                subData.pData = &whitePixel;
                subData.RowPitch = 4;
                subData.SlicePitch = 4;

                uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                auto allocHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle);
                auto cmdHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdHandle);

                UINT64 uploadSize = GetRequiredIntermediateSize(gpuMgr.GetResource(whiteTexHandle), 0, 1);
                GpuResourceHandle uploadBuf =
                    gpuMgr.CreateBuffer(device, uploadSize, L"WhiteTexture_Upload", D3D12_HEAP_TYPE_UPLOAD,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);

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

    // LoadSoldierCharacter(); // 已迁移：soldier 应由 AssetTool 导出为 .dxmesh 后通过 JSON 加载

    // LoadBillboardTextures(); // 公告牌由 JSON 场景后需接入
    // CreateBillboardTrees();

    // ====================================================================
    // SceneConstructor 异步加载测试
    // ====================================================================
    {
        std::error_code ec;
        std::filesystem::path asyncPath = m_context->ResolvePath(L"Content/map/city/city.scene.json");
        if (std::filesystem::exists(asyncPath, ec)) {
            m_asyncScenePath = asyncPath.string();
            m_asyncLoadDelay = 1; // 下一帧立即触发
            m_context->Logging->Info("[GameWorld] Async scene load scheduled");
        } else {
            m_context->Logging->Warn("[GameWorld] async_test.json not found");
        }
    }

    RegisterTerrainImmediateCallback();

    m_skinnedBuilder = std::make_unique<SkinnedRenderItemBuilder>(m_context->FrameResourceManager,
                                                                  m_context->MaterialMgr, m_context->SkeletonMgr);

    m_skinnedRenderer = std::make_unique<SkinnedRenderer>();
    m_skinnedRenderer->SetDeviceContext(m_context->DeviceContext);
    m_skinnedRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_skinnedRenderer->SetMaterialManager(m_context->MaterialMgr);
    m_skinnedRenderer->Initialize();

    // 预触 entt 组件存储池——确保 Worker 线程首次 view<> 时不与主线程竞态
    // 覆盖所有 Builder 系统可能访问的组件类型
    m_registry->view<MeshComponent>();
    m_registry->view<TransformComponent>();
    m_registry->view<OpaqueTag>();
    m_registry->view<TransparentTag>();
    m_registry->view<SkinnedTag>();
    m_registry->view<SkinnedComponent>();
    m_registry->view<TerrainComponent>();
    m_registry->view<BillboardComponent>();
    m_registry->view<WaterComponent>();

    // 水构建器
    m_waterBuilder = std::make_unique<WaterRenderItemBuilder>(m_context->FrameResourceManager, m_context->MaterialMgr,
                                                              m_context->CameraMgr);

    // 天空盒渲染器（由 SkyboxManager 提供数据，与硬编码 CreateSkybox 解耦）
    m_skyRenderer = std::make_unique<SkyRenderer>();
    m_skyRenderer->SetDeviceContext(m_context->DeviceContext);
    m_skyRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_skyRenderer->Initialize();

    // G-buffer 写入由 OpaqueRenderer 的 G-buffer 通道完成
    // 初始化延迟光照渲染器
    m_lightingRenderer = std::make_unique<DX12Engine::Renderer::LightingRenderer>();
    m_lightingRenderer->SetDeviceContext(m_context->DeviceContext);
    m_lightingRenderer->Initialize();

    // 初始化视口帧缓冲（G-buffer RT）
    m_appRTs = std::make_unique<DX12Engine::Renderer::ApplicationRenderTargets>();
    {
        auto *device = m_context->DeviceContext->GetDevice();
        auto *heaps = m_context->DescriptorHeaps;
        uint32_t w = m_context->DeviceContext->GetViewport().Width;
        uint32_t h = m_context->DeviceContext->GetViewport().Height;
        m_appRTs->Initialize(device, heaps, w, h);
    }

    RegisterOpaqueRenderSystem();
    RegisterPointShadowRenderSystem();
    RegisterLightingPass();
    RegisterSceneConstructSystem();
    // 天空盒（主渲染最后阶段，提交到 PostProcess）
    RegisterSkyboxSystem();
    // [GBuffer 调试] 注释其他渲染 Pass，避免干扰
    RegisterClearSystem();
    RegisterShadowRenderSystem();
    RegisterSpotShadowRenderSystem();
    RegisterSsaoSystem();
    {
        auto &aoMgr = AmbientOcclusionManager::GetInstance();
        aoMgr.SetEnabled(true);
        if (m_whiteTextureHandle.IsValid())
            aoMgr.SetFallbackWhiteSRV(m_context->TextureMgr->GetSRV(m_whiteTextureHandle));
    }

    RegisterTerrainRenderSystem();
    RegisterAnimationAdvancer();
    RegisterSkinnedOpaqueRenderSystem();
    RegisterBuilderSystems();
}

void GameWorld::Clear() {
    if (!m_registry)
        return;

    auto DestroyEntityWithCleanup = [this](ECS::Entity entity) {
        if (entity == INVALID_ENTITY)
            return;
        m_registry->DestroyEntity(entity);
    };

    // 地面平面已由 JSON 场景管理，不再需要手动清理
}

void GameWorld::OnResize(uint32_t width, uint32_t height) {
    if (m_appRTs) {
        m_appRTs->OnResize(width, height);
        OutputDebugStringW(L"[INFO] G-buffer RTs resized\n");
    }
}

void GameWorld::Update() {

    if (m_context->BackgroundExecutor) {
        m_context->BackgroundExecutor->Tick();
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
        }
    }

    // 延迟触发异步场景加载（让引擎先跑几帧，实体"后出现"）
    if (m_asyncLoadDelay > 0) {
        m_asyncLoadDelay--;
        if (m_asyncLoadDelay == 0 && !m_asyncScenePath.empty()) {
            try {
                m_sceneConstructor = std::make_unique<Scene::SceneConstructor>();
                auto desc = Resource::SceneLoader::LoadFromFile(std::filesystem::path(m_asyncScenePath));
                m_sceneConstructor->LoadScene(desc, m_context, [ctx = m_context](bool ok) {
                    ctx->Logging->Info("[SceneConstructor] Async scene test {}", ok ? "PASSED" : "FAILED");
                });
            } catch (const std::exception &e) {
                m_context->Logging->Error("[SceneConstructor] Failed: {}", e.what());
            }
        }
    }
}
