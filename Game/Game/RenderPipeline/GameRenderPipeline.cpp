#include "GameRenderPipeline.h"
#include "Boot/GameContext.h"
#include "ECS/Core/Components.h"
#include "ECS/Core/Registry.h"
#include "Framework/SystemBuilder.h"
#include "Framework/SystemRegistry.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/Effects/AO/AmbientOcclusionManager.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "Renderer/Material/MaterialManager.h"
#include "Renderer/Material/MaterialResource.h"
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
#include "Renderer/Scene/ReflectionProbeManager/ReflectionProbeManager.h"
#include "Renderer/Scene/SkyboxManager.h"
#include "Renderer/Scene/TerrainManager/TerrainManager.h"
#include "Renderer/Scene/WaterManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Pool/DepthStencilPool.h"
#include "Resource/Pool/RenderTargetPool.h"
#include "Resource/Texture/TextureManager.h"
#include "Scene/RenderScene.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneConstructor.h"
#include "Scheduler/FrameDriver.h"
#include <DirectXMath.h>
#include <cmath>

using namespace DX12Engine;
using namespace DX12Engine::Async;
using namespace DX12Engine::Boot;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::Scheduler;

// ========================================================================
// 初始化
// ========================================================================

GameRenderPipeline::~GameRenderPipeline() = default;

void GameRenderPipeline::Initialize(Boot::GameContext *context, OpaqueRenderer *renderer) {
    m_context = context;
    m_renderer = renderer;

    m_opaqueBuilder = std::make_unique<OpaqueRenderItemBuilder>(
        m_context->FrameResourceManager, m_context->MaterialMgr, m_context->TextureMgr);

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

    m_terrainBuilder = std::make_unique<TerrainRenderItemBuilder>(
        m_context->FrameResourceManager, m_context->TextureMgr);

    m_probeBuilder = std::make_unique<ProbeBuilder>(
        m_context->FrameResourceManager, m_context->MaterialMgr, m_context->TextureMgr);
    m_probeRenderer = std::make_unique<ReflectionProbeRenderer>();
    m_probeRenderer->SetDeviceContext(m_context->DeviceContext);
    m_probeRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_probeRenderer->SetMaterialManager(m_context->MaterialMgr);
    m_probeRenderer->Initialize();

    m_skinnedBuilder = std::make_unique<SkinnedRenderItemBuilder>(
        m_context->FrameResourceManager, m_context->MaterialMgr, m_context->SkeletonMgr);

    m_skinnedRenderer = std::make_unique<SkinnedRenderer>();
    m_skinnedRenderer->SetDeviceContext(m_context->DeviceContext);
    m_skinnedRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_skinnedRenderer->SetMaterialManager(m_context->MaterialMgr);
    m_skinnedRenderer->Initialize();

    // 水构建器
    m_waterBuilder = std::make_unique<WaterRenderItemBuilder>(
        m_context->FrameResourceManager, m_context->MaterialMgr, m_context->CameraMgr);

    // 天空盒渲染器
    m_skyRenderer = std::make_unique<SkyRenderer>();
    m_skyRenderer->SetDeviceContext(m_context->DeviceContext);
    m_skyRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_skyRenderer->Initialize();

    // 延迟光照渲染器
    m_lightingRenderer = std::make_unique<LightingRenderer>();
    m_lightingRenderer->SetDeviceContext(m_context->DeviceContext);
    m_lightingRenderer->Initialize();

    // 视口帧缓冲（G-buffer RT）
    m_appRTs = std::make_unique<ApplicationRenderTargets>();
    {
        auto *device = m_context->DeviceContext->GetDevice();
        auto *heaps = m_context->DescriptorHeaps;
        uint32_t w = m_context->DeviceContext->GetViewport().Width;
        uint32_t h = m_context->DeviceContext->GetViewport().Height;
        m_appRTs->Initialize(device, heaps, w, h);
    }
}

void GameRenderPipeline::OnResize(uint32_t width, uint32_t height) {
    if (m_appRTs) {
        m_appRTs->OnResize(width, height);
    }
}

// ========================================================================
// ProbeHelpers — 辅助函数
// ========================================================================
namespace ProbeHelpers {
void FillCaptureCB(const DirectX::XMFLOAT3 &probePos, D3D12_GPU_VIRTUAL_ADDRESS &outCBAddress,
                   FrameResourceManager &frameMgr) {
    XMVECTOR pos = XMLoadFloat3(&probePos);
    static const float s_fd[18] = {1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1};
    static const float s_fu[18] = {0, 1, 0, 0, 1, 0, 0, 0, -1, 0, 0, 1, 0, 1, 0, 0, 1, 0};
    ProbeCaptureCB captureCB = {};
    for (uint32_t f = 0; f < 6; ++f) {
        XMVECTOR dir = XMVectorSet(s_fd[f * 3], s_fd[f * 3 + 1], s_fd[f * 3 + 2], 0);
        XMVECTOR up = XMVectorSet(s_fu[f * 3], s_fu[f * 3 + 1], s_fu[f * 3 + 2], 0);
        XMMATRIX view = XMMatrixLookAtLH(pos, pos + dir, up);
        XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, 0.1f, 1000.0f);
        XMMATRIX viewProj = view * proj;
        XMStoreFloat4x4(&captureCB.faceViewProj[f], viewProj);
    }
    captureCB.probePosition = probePos;
    outCBAddress = frameMgr.Allocate("ProbeCaptureCB", &captureCB, sizeof(ProbeCaptureCB));
}
} // namespace ProbeHelpers

// ========================================================================
// Builder 注册
// ========================================================================

void GameRenderPipeline::RegisterBuilderSystems() {
    // =========================================================================
    // 上传阶段（串行）：计数 → 预留 → 分发 FrameWriter
    // =========================================================================

    REGISTER_SYSTEM(BuilderUpload, PreRender, Worker)
        .Func([this](const MessageContext &) {
            const Frustum &frustum = m_context->CullingSystem->GetFrustum();
            auto camPos = m_context->CameraMgr->GetMainCamera().Position;

            // 设置帧数据到各构建器
            m_opaqueBuilder->SetFrustum(&frustum);
            m_opaqueBuilder->SetCameraPos(camPos);
            m_opaqueBuilder->SetLODSystem(m_context->LODSystem);

            if (m_skinnedBuilder) {
                m_skinnedBuilder->SetFrustum(&frustum);
                m_skinnedBuilder->SetCameraPos(camPos);
                m_skinnedBuilder->SetLODSystem(m_context->LODSystem);
            }

            m_transparentBuilder->SetFrustum(&frustum);
            m_transparentBuilder->SetCameraPos(camPos);
            m_transparentBuilder->SetLODSystem(m_context->LODSystem);

            if (m_terrainBuilder)
                m_terrainBuilder->SetFrustum(&frustum);
            if (m_probeBuilder && m_activeProbeCount > 0) {
                m_probeBuilder->SetFrustum(&frustum);
                m_probeBuilder->SetCameraPos(camPos);
                m_probeBuilder->SetLODSystem(m_context->LODSystem);
            }

            // 水构建器
            if (m_waterBuilder) {
                m_waterBuilder->SetFrustum(&frustum);
                m_waterBuilder->SetCameraPos(camPos);
            }
        })
        .AlwaysRun()
        .Build();

    // =========================================================================
    // 构建阶段（并行）：各构建器独立 System，Worker 线程并发执行
    // =========================================================================

    REGISTER_SYSTEM(BuildOpaque, PreRender, Worker)
        .Func([this](const MessageContext &) {
            auto *reg = m_context->SceneMgr->GetRegistry();
            if (reg) m_opaqueBuilder->BuildTyped(*reg, m_opaqueQueue);
        })
        .AlwaysRun()
        .DependsOn("BuilderUpload")
        .Build();

    if (m_skinnedBuilder) {
        REGISTER_SYSTEM(BuildSkinned, PreRender, Worker)
            .Func([this](const MessageContext &) {
                auto *reg = m_context->SceneMgr->GetRegistry();
                if (reg) m_skinnedBuilder->BuildTyped(*reg, m_skinnedQueue);
            })
            .AlwaysRun()
            .DependsOn("BuilderUpload")
            .Build();
    }

    REGISTER_SYSTEM(BuildTransparent, PreRender, Worker)
        .Func([this](const MessageContext &) {
            auto *reg = m_context->SceneMgr->GetRegistry();
            if (reg) m_transparentBuilder->BuildTyped(*reg, m_transparentQueue);
        })
        .AlwaysRun()
        .DependsOn("BuilderUpload")
        .Build();

    if (m_terrainBuilder) {
        REGISTER_SYSTEM(BuildTerrain, PreRender, Worker)
            .Func([this](const MessageContext &) {
                auto *reg = m_context->SceneMgr->GetRegistry();
                if (reg) m_terrainBuilder->BuildTyped(*reg, m_terrainQueue);
            })
            .AlwaysRun()
            .DependsOn("BuilderUpload")
            .Build();
    }

    if (m_probeBuilder) {
        REGISTER_SYSTEM(BuildProbes, PreRender, Worker)
            .Func([this](const MessageContext &) {
                auto *reg = m_context->SceneMgr->GetRegistry();
                if (!reg) return;
                auto *rs = reg ? m_context->SceneMgr->GetRenderScene() : nullptr;
                auto *probeMgr = rs ? rs->GetReflectionProbeManager() : nullptr;
                uint32_t probeCount = probeMgr ? probeMgr->GetActiveProbeCount() : 0;
                m_activeProbeCount = probeCount;
                if (probeCount > 0) {
                    m_probeBuilder->Build(m_probeCaptureInfo, m_activeProbeCount, *reg, m_probeQueues);
                }
            })
            .AlwaysRun()
            .DependsOn("BuilderUpload")
            .Build();
    }

    // 水构建器
    REGISTER_SYSTEM(BuildWater, PreRender, Worker)
        .Func([this](const MessageContext &) {
            auto *reg = m_context->SceneMgr->GetRegistry();
            if (reg) m_waterBuilder->BuildTyped(*reg, m_waterQueue);
        })
        .AlwaysRun()
        .DependsOn("BuilderUpload")
        .Build();

    // =========================================================================
    // FrameSync 回调：统一上传所有临时数据到 GPU RingBuffer
    // =========================================================================
    if (m_context && m_context->FrameDriver) {
        m_context->FrameDriver->RegisterFrameSyncCallback(
            [this]() {
                auto *frameRes = m_context->FrameResourceManager;
                if (!frameRes)
                    return;

                // ── Opaque ──
                {
                    auto &batches = m_opaqueBuilder->GetPendingBatches();
                    auto &queue = m_opaqueQueue;
                    for (auto &batch : batches) {
                        if (batch.queueIndex >= queue.Size() || batch.instances.empty())
                            continue;
                        D3D12_GPU_VIRTUAL_ADDRESS addr =
                            frameRes->Allocate("Instance", batch.instances.data(),
                                               static_cast<uint32_t>(batch.instances.size() * sizeof(InstanceData)));
                        queue[batch.queueIndex].instanceBuffer = addr;
                    }
                    batches.clear();
                }

                // ── Skinned ──
                if (m_skinnedBuilder) {
                    auto &batches = m_skinnedBuilder->GetPendingBatches();
                    auto &queue = m_skinnedQueue;
                    for (auto &batch : batches) {
                        if (batch.queueIndex >= queue.Size() || batch.instances.empty())
                            continue;
                        D3D12_GPU_VIRTUAL_ADDRESS addr =
                            frameRes->Allocate("Instance", batch.instances.data(),
                                               static_cast<uint32_t>(batch.instances.size() * sizeof(InstanceData)));
                        queue[batch.queueIndex].instanceBuffer = addr;
                    }
                    batches.clear();
                }

                // ── Transparent ObjectConstants ──
                {
                    auto &batches = m_transparentBuilder->GetPendingBatches();
                    auto &queue = m_transparentQueue;
                    for (uint32_t i = 0; i < queue.Size(); ++i) {
                        auto &item = queue[i];
                        if (item.tempSlot != UINT32_MAX && item.tempSlot < batches.size()) {
                            item.objectCBAddress =
                                frameRes->Allocate("ObjectCB", &batches[item.tempSlot].object, sizeof(ObjectConstants));
                        }
                    }
                    batches.clear();
                }
            },
            "FrameSync_UploadInstanceData");
    }
}

// ========================================================================
// 动画推进
// ========================================================================

void GameRenderPipeline::RegisterAnimationAdvancer() {
    SystemRegistry::Register(
        {.name = "AnimationAdvancer",
         .func =
             [this](const MessageContext &) {
                 auto *skeletonMgr = m_context->SkeletonMgr;
                 auto *frameResMgr = m_context->FrameResourceManager;
                 if (!skeletonMgr || !frameResMgr)
                     return;

                 float deltaTime = m_context->MainTimer->GetDeltaTime();

                 auto *registry = m_context->SceneMgr->GetRegistry();
                 if (!registry)
                     return;

                 auto view = registry->view<ECS::SkinnedComponent>();
                 if (view.empty())
                     return;

                 std::vector<DirectX::XMFLOAT4X4> boneTransforms;

                 for (auto entity : view) {
                     auto &skin = view.get<ECS::SkinnedComponent>(entity);
                     if (!skin.skeletonHandle.IsValid() || skin.currentClip.empty())
                         continue;

                     uint32_t boneCount = skeletonMgr->GetBoneCount(skin.skeletonHandle);
                     if (boneCount == 0)
                         continue;

                     float clipDuration = skeletonMgr->GetClipDuration(skin.skeletonHandle, skin.currentClip);
                     if (clipDuration > 0.0f) {
                         skin.timePos += deltaTime;
                         while (skin.timePos >= clipDuration)
                             skin.timePos -= clipDuration;
                     }

                     if (boneTransforms.size() < boneCount)
                         boneTransforms.resize(boneCount);

                     bool ok = skeletonMgr->ComputeFinalTransforms(skin.skeletonHandle, skin.currentClip, skin.timePos,
                                                                   boneTransforms);
                     if (!ok) {
                         for (uint32_t i = 0; i < boneCount; ++i) {
                             XMStoreFloat4x4(&boneTransforms[i], XMMatrixIdentity());
                         }
                     }

                     uint32_t uploadSize = boneCount * sizeof(DirectX::XMFLOAT4X4);
                     skin.boneBufferAddress = frameResMgr->Allocate("Skinning", boneTransforms.data(), uploadSize);
                 }

                 if (!m_soldierEntities.empty()) {
                     m_soldierAngle += deltaTime * 0.5f;
                     float radius = 8.0f;
                     float cx = 0.0f, cz = 0.0f;
                     float x = cx + radius * cosf(m_soldierAngle);
                     float z = cz + radius * sinf(m_soldierAngle);
                     for (auto entity : m_soldierEntities) {
                         auto *xform = registry->TryGetComponent<ECS::TransformComponent>(entity);
                         if (xform) {
                             xform->position = XMFLOAT3(x, 30.0f, z);
                         }
                     }
                 }
             },
         .phase = TaskPhase::LateUpdate,
         .threadType = ThreadType::Any,
         .priority = TaskPriority::Normal,
         .alwaysRun = true});
}

// ========================================================================
// 地形立即回调
// ========================================================================

void GameRenderPipeline::RegisterTerrainImmediateCallback() {
    if (!m_context || !m_context->FrameDriver)
        return;

    auto *registry = m_context->SceneMgr->GetRegistry();
    if (!registry)
        return;

    m_context->FrameDriver->RegisterImmediateCallback(
        [this, registry]() {
            auto &terrainMgr = TerrainManager::GetInstance();

            std::vector<TerrainConstants> pendingConstants;
            pendingConstants.reserve(16);

            auto view = registry->view<TerrainComponent>();
            for (auto entity : view) {
                auto *terrainComp = registry->TryGetComponent<TerrainComponent>(entity);
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
                constants.HeightMapIndex = 0;
                constants.AlbedoMapIndex = 1;
                constants.NormalMapIndex = 2;

                pendingConstants.push_back(constants);
            }

            if (!pendingConstants.empty()) {
                terrainMgr.SetPendingConstants(std::move(pendingConstants));
                terrainMgr.UpdateAndUpload(m_context->GetNextFence());
            }
        },
        "TerrainImmediate");
}

// ========================================================================
// 探针场景数据回调
// ========================================================================

void GameRenderPipeline::RegisterProbeSceneDataCallback() {
    auto *rs = m_context ? m_context->SceneMgr->GetRenderScene() : nullptr;
    auto *probeMgr = rs ? rs->GetReflectionProbeManager() : nullptr;
    if (!m_context || !m_context->FrameDriver || !probeMgr)
        return;

    m_context->FrameDriver->RegisterSceneDataCallback([this, probeMgr]() {
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

// ========================================================================
// 清除 System
// ========================================================================

void GameRenderPipeline::RegisterClearSystem() {
    SystemRegistry::Register(
        {.name = "ClearRenderSystem",
         .func =
             [this](const MessageContext &ctx) {
                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 auto backBuffer = m_context->GetBackBuffer();

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

                 auto rtvHandle = m_context->DeviceContext->GetCurrentBackBufferView();
                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 const float clearColor[] = {0.0f, 0.2f, 0.4f, 1.0f};
                 cmdList.Get()->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
                 cmdList.Get()->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                                      1.0f, 0, 0, nullptr);

                 D3D12_RESOURCE_BARRIER endBarrier = {};
                 endBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 endBarrier.Transition.pResource = backBuffer;
                 endBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 endBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 endBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &endBarrier);

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

// ========================================================================
// 天空盒渲染 System
// ========================================================================

void GameRenderPipeline::RegisterSkyboxSystem() {
    SystemRegistry::Register(
        {.name = "SkyboxRenderSystem",
         .func =
             [this](const MessageContext &ctx) {
                 auto &skyMgr = SkyboxManager::GetInstance();
                 if (!m_skyRenderer || !skyMgr.IsValid()) {
                     return;
                 }

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 auto backBuffer = m_context->GetBackBuffer();
                 auto rtvHandle = m_context->DeviceContext->GetCurrentBackBufferView();
                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();
                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();

                 D3D12_RESOURCE_BARRIER barrier = {};
                 barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 barrier.Transition.pResource = backBuffer;
                 barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                 barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 const auto &viewport = m_context->DeviceContext->GetViewport();
                 const auto &scissorRect = m_context->DeviceContext->GetScissorRect();
                 cmdList.Get()->RSSetViewports(1, &viewport);
                 cmdList.Get()->RSSetScissorRects(1, &scissorRect);
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 ID3D12DescriptorHeap *descriptorHeaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 cmdList.Get()->SetDescriptorHeaps(1, descriptorHeaps);

                 // 从 SkyboxManager 直接读取数据
                 m_skyRenderer->BeginFrame(cmdList, passCBAddr, skyMgr.GetCubeSRV());
                 m_skyRenderer->DrawSky(cmdList, skyMgr.GetGeometry(), skyMgr.GetObjectCBAddress());
                 m_skyRenderer->EndFrame();

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

// ========================================================================
// 水体渲染 System
// ========================================================================

void GameRenderPipeline::RegisterWaterRenderSystem() {
    SystemRegistry::Register(
        {.name = "WaterRenderSystem",
         .func =
             [this](const MessageContext &ctx) {
                 if (m_waterQueue.Empty()) {
                     return;
                 }

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

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

                 const auto &viewport = m_context->DeviceContext->GetViewport();
                 const auto &scissorRect = m_context->DeviceContext->GetScissorRect();
                 cmdList.Get()->RSSetViewports(1, &viewport);
                 cmdList.Get()->RSSetScissorRects(1, &scissorRect);
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 ID3D12DescriptorHeap *descriptorHeaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 cmdList.Get()->SetDescriptorHeaps(1, descriptorHeaps);

                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 D3D12_GPU_VIRTUAL_ADDRESS lightCBAddr = LightManager::GetInstance().GetLightCBAddress();
                 D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV = m_context->MaterialMgr->GetMaterialBufferSRV();

                 m_waterRenderer->BeginFrame(cmdList, passCBAddr, lightCBAddr, materialBufferSRV,
                                             WaterManager::GetInstance().GetWaterCBAddress());

                 // 消费 WaterRenderItem 队列（由 WaterRenderItemBuilder 在 PreRender 阶段构建）
                 auto &waterMgr = WaterManager::GetInstance();
                 D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV = waterMgr.GetEnvironmentMap();

                 // 按深度排序（远到近）
                 auto &items = m_waterQueue.GetItems();
                 std::vector<size_t> indices(items.size());
                 for (size_t i = 0; i < indices.size(); ++i)
                     indices[i] = i;
                 std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
                     return items[a].depth > items[b].depth; // 远→近
                 });

                 for (size_t idx : indices) {
                     const auto &item = items[idx];
                     if (!item.IsValid())
                         continue;
                     m_waterRenderer->DrawWater(cmdList, item.geometryHandle, item.worldMatrix, item.objectCBAddress,
                                                envMapSRV);
                 }
                 m_waterRenderer->EndFrame();

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

// ========================================================================
// 地形渲染 System
// ========================================================================

void GameRenderPipeline::RegisterTerrainRenderSystem() {
    SystemRegistry::Register(
        {.name = "TerrainRenderSystem",
         .func =
             [this](const MessageContext &ctx) {
                 if (m_terrainQueue.Empty() || !m_appRTs) {
                     return;
                 }

                 uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                 auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                 auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
                 auto *nativeCL = cmd.Get();

                 // 屏障：G-buffer RTs COMMON → RENDER_TARGET
                 auto &rtPool = RenderTargetPool::GetInstance();
                 auto barrierRT = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_COMMON,
                                                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                     nativeCL->ResourceBarrier(1, &b);
                 };
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferAlbedo()));
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferNormal()));
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferMaterial()));
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferWorldPos()));

                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();
                 const auto &vp = m_context->DeviceContext->GetViewport();
                 const auto &sr = m_context->DeviceContext->GetScissorRect();
                 nativeCL->RSSetViewports(1, &vp);
                 nativeCL->RSSetScissorRects(1, &sr);

                 ID3D12DescriptorHeap *heaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 nativeCL->SetDescriptorHeaps(1, heaps);

                 // 绑定 G-buffer RTs
                 D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferAlbedo()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferNormal()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferMaterial()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferWorldPos()),
                 };
                 nativeCL->OMSetRenderTargets(4, rtvs, FALSE, &dsvHandle);

                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 m_terrainRenderer->BeginFrameGBuffer(cmd, passCBAddr);

                 for (const auto &item : m_terrainQueue.GetItems()) {
                     if (!item.IsValid())
                         continue;
                     m_terrainRenderer->DrawTerrainGBuffer(cmd, item);
                 }

                 // 屏障：G-buffer RTs RENDER_TARGET → COMMON（为下帧/后续 Pass 准备）
                 auto barrierBack = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                   D3D12_RESOURCE_STATE_COMMON);
                     nativeCL->ResourceBarrier(1, &b);
                 };
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferAlbedo()));
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferNormal()));
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferMaterial()));
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferWorldPos()));

                 cmd.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Opaque, cmdH);
                 uint64_t seq = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Opaque,
         .alwaysRun = true});
}

// ========================================================================
// Opaque G-buffer 渲染 System
// ========================================================================

void GameRenderPipeline::RegisterOpaqueRenderSystem() {
    SystemRegistry::Register(
        {.name = "OpaqueRenderSystem",
         .func =
             [this](const MessageContext &) {
                 if (!m_renderer || m_opaqueQueue.Empty())
                     return;

                 // G-buffer RT 必须已分配
                 if (!m_appRTs || !m_appRTs->IsInitialized())
                     return;

                 auto &rtPool = RenderTargetPool::GetInstance();

                 uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                 auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                 auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);

                 D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferAlbedo()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferNormal()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferMaterial()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferWorldPos()),
                 };

                 // 资源屏障：COMMON → RENDER_TARGET
                 auto barrierRT = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_COMMON,
                                                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                     cmd.Get()->ResourceBarrier(1, &b);
                 };
                 barrierRT(m_appRTs->GetGBufferAlbedoResource());
                 barrierRT(m_appRTs->GetGBufferNormalResource());
                 barrierRT(m_appRTs->GetGBufferMaterialResource());
                 barrierRT(m_appRTs->GetGBufferWorldPosResource());

                 // 设置视口
                 const auto &vp = m_context->DeviceContext->GetViewport();
                 const auto &sr = m_context->DeviceContext->GetScissorRect();
                 cmd.Get()->RSSetViewports(1, &vp);
                 cmd.Get()->RSSetScissorRects(1, &sr);

                 // 设置描述符堆
                 ID3D12DescriptorHeap *heaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 cmd.Get()->SetDescriptorHeaps(1, heaps);

                 // 绑定 4 个 G-buffer RT + 主深度缓冲
                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();
                 cmd.Get()->OMSetRenderTargets(4, rtvs, FALSE, &dsvHandle);

                 // 清除 RT 和深度缓冲
                 const float clearColor[4] = {0, 0, 0, 0};
                 cmd.Get()->ClearRenderTargetView(rtvs[0], clearColor, 0, nullptr);
                 cmd.Get()->ClearRenderTargetView(rtvs[1], clearColor, 0, nullptr);
                 cmd.Get()->ClearRenderTargetView(rtvs[2], clearColor, 0, nullptr);
                 cmd.Get()->ClearRenderTargetView(rtvs[3], clearColor, 0, nullptr);
                 cmd.Get()->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

                 // G-buffer 渲染（使用 OpaqueRenderer 的 G-buffer 通道）
                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 D3D12_GPU_DESCRIPTOR_HANDLE matSRV = m_context->MaterialMgr->GetMaterialBufferSRV();
                 D3D12_GPU_DESCRIPTOR_HANDLE texHeapStart =
                     m_context->DescriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, 0);

                 m_renderer->BeginFrameGBuffer(cmd, passCBAddr, matSRV, texHeapStart);

                 for (const auto &item : m_opaqueQueue) {
                     if (!item.IsValid())
                         continue;
                     m_renderer->DrawInstancedGBuffer(cmd, item.geometryHandle, item.instanceBuffer, item.instanceCount,
                                                      item.startIndex, item.startVertex, item.indexCount);
                 }

                 m_renderer->EndFrameGBuffer();

                 // 屏障：RENDER_TARGET → COMMON（为下帧准备）
                 auto barrierBack = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                   D3D12_RESOURCE_STATE_COMMON);
                     cmd.Get()->ResourceBarrier(1, &b);
                 };
                 barrierBack(m_appRTs->GetGBufferAlbedoResource());
                 barrierBack(m_appRTs->GetGBufferNormalResource());
                 barrierBack(m_appRTs->GetGBufferMaterialResource());
                 barrierBack(m_appRTs->GetGBufferWorldPosResource());

                 cmd.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Opaque, cmdH);

                 uint64_t seq = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Opaque,
         .alwaysRun = true});
}

// ========================================================================
// 延迟光照 System
// ========================================================================

void GameRenderPipeline::RegisterLightingPass() {
    SystemRegistry::Register(
        {.name = "LightingPass",
         .func =
             [this](const MessageContext &) {
                 if (!m_lightingRenderer)
                     return;

                 uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                 auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                 auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
                 auto backBuffer = m_context->GetBackBuffer();

                 // 屏障：G-buffer RTs COMMON → PIXEL_SHADER_RESOURCE
                 auto &rtPool = RenderTargetPool::GetInstance();
                 auto barrierToSRV = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_COMMON,
                                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                     cmd.Get()->ResourceBarrier(1, &b);
                 };
                 barrierToSRV(m_appRTs->GetGBufferAlbedoResource());
                 barrierToSRV(m_appRTs->GetGBufferNormalResource());
                 barrierToSRV(m_appRTs->GetGBufferMaterialResource());
                 barrierToSRV(m_appRTs->GetGBufferWorldPosResource());

                 // 屏障：交换链 PRESENT → RENDER_TARGET
                 D3D12_RESOURCE_BARRIER bbBarrier = {};
                 bbBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 bbBarrier.Transition.pResource = backBuffer;
                 bbBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                 bbBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 bbBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmd.Get()->ResourceBarrier(1, &bbBarrier);

                 // 视口和 RT
                 const auto &vp = m_context->DeviceContext->GetViewport();
                 const auto &sr = m_context->DeviceContext->GetScissorRect();
                 cmd.Get()->RSSetViewports(1, &vp);
                 cmd.Get()->RSSetScissorRects(1, &sr);
                 auto rtv = m_context->DeviceContext->GetCurrentBackBufferView();
                 cmd.Get()->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

                 // 设置描述符堆
                 ID3D12DescriptorHeap *heaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 cmd.Get()->SetDescriptorHeaps(1, heaps);

                 // 渲染
                 D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV = SkyboxManager::GetInstance().GetCubeSRV();
                 auto *rs = m_context->SceneMgr->GetRenderScene();
                 auto *probeMgr = rs ? rs->GetReflectionProbeManager() : nullptr;
                 D3D12_GPU_DESCRIPTOR_HANDLE cubemapArraySRV = probeMgr ? probeMgr->GetProbeCubemapArraySRV() : D3D12_GPU_DESCRIPTOR_HANDLE{};

                 auto &lightMgr = LightManager::GetInstance();
                 D3D12_GPU_DESCRIPTOR_HANDLE shadowDataSRV = lightMgr.GetShadowDataSRV();
                 D3D12_GPU_DESCRIPTOR_HANDLE shadowMapSRV = lightMgr.GetShadowMapSRV();

                 m_lightingRenderer->BeginFrame(cmd, m_context->FrameResourceManager->GetPassCBAddress(),
                                                LightManager::GetInstance().GetLightCBAddress(),
                                                m_appRTs->GetGBufferAlbedoSRV(), m_appRTs->GetGBufferNormalSRV(),
                                                m_appRTs->GetGBufferMaterialSRV(), m_appRTs->GetGBufferWorldPosSRV(),
                                                AmbientOcclusionManager::GetInstance().GetAmbientMapSRV(), envMapSRV,
                                                cubemapArraySRV, shadowDataSRV, shadowMapSRV);
                 m_lightingRenderer->Draw(cmd);
                 m_lightingRenderer->EndFrame();

                 // 屏障：G-buffer RTs PIXEL_SHADER_RESOURCE → COMMON
                 auto barrierToCommon = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                                   D3D12_RESOURCE_STATE_COMMON);
                     cmd.Get()->ResourceBarrier(1, &b);
                 };
                 barrierToCommon(m_appRTs->GetGBufferAlbedoResource());
                 barrierToCommon(m_appRTs->GetGBufferNormalResource());
                 barrierToCommon(m_appRTs->GetGBufferMaterialResource());
                 barrierToCommon(m_appRTs->GetGBufferWorldPosResource());

                 // 屏障：交换链 RENDER_TARGET → PRESENT
                 bbBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 bbBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 cmd.Get()->ResourceBarrier(1, &bbBarrier);

                 cmd.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Lighting, cmdH);
                 uint64_t seq = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Lighting,
         .alwaysRun = true});
}

// ========================================================================
// Skinned Opaque 渲染 System
// ========================================================================

void GameRenderPipeline::RegisterSkinnedOpaqueRenderSystem() {
    SystemRegistry::Register(
        {.name = "SkinnedOpaqueRenderSystem",
         .func =
             [this](const MessageContext &ctx) {
                 if (!m_skinnedRenderer || m_skinnedQueue.Empty() || !m_appRTs)
                     return;

                 uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                 auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                 auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
                 auto *nativeCL = cmd.Get();

                 // 屏障：G-buffer RTs COMMON → RENDER_TARGET
                 auto &rtPool = RenderTargetPool::GetInstance();
                 auto barrierRT = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_COMMON,
                                                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                     nativeCL->ResourceBarrier(1, &b);
                 };
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferAlbedo()));
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferNormal()));
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferMaterial()));
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferWorldPos()));

                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();

                 // 设置视口 + 描述符堆
                 const auto &vp = m_context->DeviceContext->GetViewport();
                 const auto &sr = m_context->DeviceContext->GetScissorRect();
                 nativeCL->RSSetViewports(1, &vp);
                 nativeCL->RSSetScissorRects(1, &sr);
                 ID3D12DescriptorHeap *heaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 nativeCL->SetDescriptorHeaps(1, heaps);

                 // 绑定 G-buffer RTs
                 D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferAlbedo()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferNormal()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferMaterial()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferWorldPos()),
                 };
                 nativeCL->OMSetRenderTargets(4, rtvs, FALSE, &dsvHandle);

                 // G-buffer 渲染
                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 D3D12_GPU_DESCRIPTOR_HANDLE matSRV = m_context->MaterialMgr->GetMaterialBufferSRV();
                 D3D12_GPU_DESCRIPTOR_HANDLE texHeapStart =
                     m_context->DescriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, 0);

                 m_skinnedRenderer->BeginFrameGBuffer(cmd, passCBAddr, matSRV, texHeapStart);
                 m_skinnedRenderer->DrawGBuffer(cmd, m_skinnedQueue);
                 m_skinnedRenderer->EndFrameGBuffer();

                 // 屏障：G-buffer RTs RENDER_TARGET → COMMON（为下帧/后续 Pass 准备）
                 auto barrierBack = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                   D3D12_RESOURCE_STATE_COMMON);
                     nativeCL->ResourceBarrier(1, &b);
                 };
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferAlbedo()));
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferNormal()));
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferMaterial()));
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferWorldPos()));

                 cmd.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Opaque, cmdH);

                 uint64_t seq = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Opaque,
         .alwaysRun = true});
}

// ========================================================================
// 方向光阴影 System
// ========================================================================

void GameRenderPipeline::RegisterShadowRenderSystem() {
    SystemRegistry::Register(
        {.name = "ShadowRenderSystem",
         .func =
             [this](const MessageContext &ctx) {
                 auto &lightMgr = LightManager::GetInstance();
                 if (!lightMgr.HasShadow(LightType::Directional) || m_opaqueQueue.Empty())
                     return;

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 uint32_t dirCount = lightMgr.GetLightCount(LightType::Directional);
                 for (uint32_t i = 0; i < dirCount; ++i) {
                     const Light *light = lightMgr.GetLight(LightType::Directional, i);
                     if (!light || light->CastShadow <= 0.5f)
                         continue;

                     const auto &shadowRes = lightMgr.GetDirShadowResource();
                     ID3D12Resource *depthTexture = DepthStencilPool::GetInstance().GetResource(shadowRes.handle);
                     if (!depthTexture || shadowRes.cbvAddress == 0)
                         continue;

                     D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
                         m_context->DescriptorHeaps->GetCpuHandle(PartitionType::Dsv, shadowRes.dsvSlot);

                     m_shadowRenderer->BeginOffscreen(cmdList, shadowRes.cbvAddress, dsvHandle, shadowRes.resolution,
                                                      shadowRes.resolution);

                     for (const auto &item : m_opaqueQueue) {
                         if (!item.IsValid())
                             continue;
                         m_shadowRenderer->DrawInstanced(cmdList, item.geometryHandle, item.instanceBuffer,
                                                         item.instanceCount);
                     }

                     m_shadowRenderer->EndOffscreen(cmdList);
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

// ========================================================================
// 点光源阴影 System
// ========================================================================

void GameRenderPipeline::RegisterPointShadowRenderSystem() {
    SystemRegistry::Register(
        {.name = "PointShadowRenderSystem",
         .func =
             [this](const MessageContext &ctx) {
                 auto &lightMgr = LightManager::GetInstance();
                 if (!lightMgr.HasShadow(LightType::Point) || m_opaqueQueue.Empty())
                     return;

                 uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                 auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                 auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);

                 cmd.Get()->SetGraphicsRootSignature(m_shadowRenderer->GetRootSignature());

                 m_shadowRenderer->SetInPass(true);

                 uint32_t ptCount = lightMgr.GetLightCount(LightType::Point);
                 for (uint32_t lightIdx = 0; lightIdx < ptCount; ++lightIdx) {
                     const Light *light = lightMgr.GetLight(LightType::Point, lightIdx);
                     if (!light || light->CastShadow <= 0.5f)
                         continue;

                     const auto &shadowRes = lightMgr.GetPointShadowResource(lightIdx);
                     if (!shadowRes.isValid)
                         continue;

                     auto &dsPool = DepthStencilPool::GetInstance();
                     uint32_t res = shadowRes.resolution;

                     // 全数组 DSV 清除一次（6 slice 共享纹理）
                     D3D12_CPU_DESCRIPTOR_HANDLE arrayDsvHandle = dsPool.GetDsvHandle(shadowRes.arrayHandle);
                     cmd.Get()->ClearDepthStencilView(arrayDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

                     D3D12_VIEWPORT vp = {0, 0, (float)res, (float)res, 0, 1};
                     D3D12_RECT scissor = {0, 0, (LONG)res, (LONG)res};
                     cmd.Get()->RSSetViewports(1, &vp);
                     cmd.Get()->RSSetScissorRects(1, &scissor);

                     if (ID3D12PipelineState *gsPSO = m_shadowRenderer->GetPointGSPSO()) {
                         // GS 路径：一次 DrawCall 展开 6 面
                         cmd.Get()->SetPipelineState(gsPSO);
                         cmd.Get()->OMSetRenderTargets(0, nullptr, FALSE, &arrayDsvHandle);
                         cmd.Get()->SetGraphicsRootConstantBufferView(1, shadowRes.cbvAddress);

                         for (const auto &item : m_opaqueQueue) {
                             if (!item.IsValid())
                                 continue;
                             if (item.indexCount > 0) {
                                 m_shadowRenderer->DrawIndexedInstancedSubmesh(
                                     cmd, item.geometryHandle, item.instanceBuffer, item.instanceCount, item.startIndex,
                                     item.startVertex, item.indexCount);
                             } else {
                                 m_shadowRenderer->DrawInstanced(cmd, item.geometryHandle, item.instanceBuffer,
                                                                 item.instanceCount);
                             }
                         }
                     } else {
                         // 6 遍渲染（GS 不可用时的回退）
                         for (int face = 0; face < 6; ++face) {
                             D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
                                 m_context->DescriptorHeaps->GetCpuHandle(PartitionType::Dsv, shadowRes.dsvSlots[face]);

                             cmd.Get()->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);

                             cmd.Get()->SetPipelineState(m_shadowRenderer->GetPointInstancedPSO());
                             cmd.Get()->SetGraphicsRootConstantBufferView(1, shadowRes.cbvAddress);
                             cmd.Get()->SetGraphicsRoot32BitConstant(3, face, 0); // gShadowLightIndex

                             for (const auto &item : m_opaqueQueue) {
                                 if (!item.IsValid())
                                     continue;
                                 if (item.indexCount > 0) {
                                     m_shadowRenderer->DrawIndexedInstancedSubmesh(
                                         cmd, item.geometryHandle, item.instanceBuffer, item.instanceCount,
                                         item.startIndex, item.startVertex, item.indexCount);
                                 } else {
                                     m_shadowRenderer->DrawInstanced(cmd, item.geometryHandle, item.instanceBuffer,
                                                                     item.instanceCount);
                                 }
                             }
                         }
                     }
                 }

                 m_shadowRenderer->SetInPass(false);

                 cmd.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::PrePass, cmdH);

                 uint64_t seq = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::PrePass,
         .alwaysRun = true});
}

// ========================================================================
// 聚光灯阴影 System
// ========================================================================

void GameRenderPipeline::RegisterSpotShadowRenderSystem() {
    SystemRegistry::Register(
        {.name = "SpotShadowRenderSystem",
         .func =
             [this](const MessageContext &ctx) {
                 auto &lightMgr = LightManager::GetInstance();

                 if (!lightMgr.HasShadow(LightType::Spot) || m_opaqueQueue.Empty())
                     return;

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 // 使用聚光灯专用 PSO（SpotShadowVS_Instanced 读取 gSpotLightViewProj）
                 cmdList.Get()->SetGraphicsRootSignature(m_shadowRenderer->GetRootSignature());

                 uint32_t spCount = lightMgr.GetLightCount(LightType::Spot);
                 for (uint32_t i = 0; i < spCount; ++i) {
                     const Light *light = lightMgr.GetLight(LightType::Spot, i);
                     if (!light || light->CastShadow <= 0.5f)
                         continue;

                     const auto &shadowRes = lightMgr.GetSpotShadowResource(i);
                     if (!shadowRes.isValid || shadowRes.cbvAddress == 0)
                         continue;

                     D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
                         m_context->DescriptorHeaps->GetCpuHandle(PartitionType::Dsv, shadowRes.dsvSlot);

                     ID3D12Resource *depthTexture = DepthStencilPool::GetInstance().GetResource(shadowRes.handle);
                     if (!depthTexture)
                         continue;

                     m_shadowRenderer->BeginOffscreen(cmdList, shadowRes.cbvAddress, dsvHandle, shadowRes.resolution,
                                                      shadowRes.resolution);
                     // BeginOffscreen 设了方向光 PSO，覆盖回聚光灯 PSO
                     cmdList.Get()->SetPipelineState(m_shadowRenderer->GetSpotPSO());

                     for (const auto &item : m_opaqueQueue) {
                         if (!item.IsValid())
                             continue;
                         m_shadowRenderer->DrawInstanced(cmdList, item.geometryHandle, item.instanceBuffer,
                                                         item.instanceCount);
                     }

                     m_shadowRenderer->EndOffscreen(cmdList);
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

// ========================================================================
// SSAO System
// ========================================================================

void GameRenderPipeline::RegisterSsaoSystem() {
    SystemRegistry::Register(
        {.name = "SsaoSystem",
         .func =
             [this](const MessageContext &ctx) {
                 auto &aoMgr = AmbientOcclusionManager::GetInstance();
                 if (!aoMgr.IsInitialized() || !m_appRTs)
                     return;

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);
                 auto *nativeCL = cmdList.Get();

                 // 从 G-buffer 读取法线 + 主深度缓冲
                 D3D12_GPU_DESCRIPTOR_HANDLE depthSRV = m_context->DeviceContext->GetDepthSRV();
                 D3D12_GPU_DESCRIPTOR_HANDLE normalSRV = m_appRTs->GetGBufferNormalSRV();
                 ID3D12Resource *depthRes = m_context->DeviceContext->GetSwapChainManager().GetDepthStencilBuffer();

                 auto *ambient0 = aoMgr.GetAmbientResource0();
                 auto *ambient1 = aoMgr.GetAmbientResource1();
                 auto barrier = [&](ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
                     if (!res)
                         return;
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, from, to);
                     nativeCL->ResourceBarrier(1, &b);
                 };

                 // 屏障：主深度 DEPTH_WRITE → PIXEL_SHADER_RESOURCE
                 barrier(depthRes, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                 // 屏障：G-buffer 法线 COMMON → PIXEL_SHADER_RESOURCE
                 barrier(m_appRTs->GetGBufferNormalResource(), D3D12_RESOURCE_STATE_COMMON,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                 // 屏障：AO 环境贴图 COMMON → RENDER_TARGET（RenderTargetPool 初始状态为 COMMON）
                 barrier(ambient0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
                 barrier(ambient1, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);

                 // 设置视口（新命令列表需要）
                 const auto &vp = m_context->DeviceContext->GetViewport();
                 const auto &sr = m_context->DeviceContext->GetScissorRect();
                 nativeCL->RSSetViewports(1, &vp);
                 nativeCL->RSSetScissorRects(1, &sr);

                 const auto &passCB = m_context->FrameResourceManager->GetPassConstants();
                 aoMgr.Execute(nativeCL, depthSRV, normalSRV, passCB.View, passCB.Proj);

                 // 屏障：AO 环境贴图 RENDER_TARGET → COMMON（回到 RenderTargetPool 初始状态）
                 barrier(ambient0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
                 barrier(ambient1, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
                 // 屏障：G-buffer 法线 PIXEL_SHADER_RESOURCE → COMMON
                 barrier(m_appRTs->GetGBufferNormalResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_COMMON);
                 // 屏障：主深度 PIXEL_SHADER_RESOURCE → DEPTH_WRITE
                 barrier(depthRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

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

// ========================================================================
// 探针捕获 System
// ========================================================================

void GameRenderPipeline::RegisterProbeCaptureSystem() {
    SystemRegistry::Register(
        {.name = "ProbeCaptureSystem",
         .func =
             [this](const MessageContext &) {
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

                     D3D12_GPU_VIRTUAL_ADDRESS captureCBAddr = info.captureCBAddress;
                     if (captureCBAddr == 0)
                         continue;
                     D3D12_CPU_DESCRIPTOR_HANDLE cubemapRTV =
                         m_context->DescriptorHeaps->GetCpuHandle(PartitionType::Rtv, info.rtvBaseSlot);
                     D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart =
                         m_context->DescriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, 0);
                     m_probeRenderer->BeginCapture(cmdList, info.cubemapResource, cubemapRTV, depthDSV, info.resolution,
                                                   info.resolution, captureCBAddr, lightCBAddr, matBufferSRV,
                                                   textureHeapStart);
                     for (const auto &item : m_probeQueues[i]) {
                         if (!item.IsValid())
                             continue;
                         m_probeRenderer->DrawInstanced(cmdList, item.geometryHandle, item.instanceBuffer,
                                                        item.instanceCount);
                     }
                     m_probeRenderer->EndCapture(cmdList);
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