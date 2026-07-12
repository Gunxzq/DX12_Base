#include "Boot/GameContext.h"
#include "ECS/Core/Registry.h"
#include "Framework/SystemBuilder.h"
#include "Framework/SystemRegistry.h"
#include "GameWorld.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "Renderer/Pipeline/ReflectionProbeRenderer.h"
#include "Renderer/RenderItemBuilder/BillboardRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/OpaqueRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/ProbeBuilder.h"
#include "Renderer/RenderItemBuilder/SkinnedRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/TerrainRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/TransparentRenderItemBuilder.h"
#include "Renderer/Scene/LightManager/LightManager.h"
#include "Renderer/Scene/ReflectionProbeManager/ReflectionProbeManager.h"
#include "Renderer/Scene/TerrainManager/TerrainManager.h"
#include "Resource/Pool/RenderTargetPool.h"
#include "Scheduler/FrameDriver.h"
#include <DirectXMath.h>
#include <cmath>

using namespace DirectX;
using namespace DX12Engine;
using namespace DX12Engine::Boot;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::Scheduler;

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
// GameWorld — Builder & 回调注册
// ========================================================================

void GameWorld::RegisterBuilderSystems() {
    // =========================================================================
    // 上传阶段（串行）：计数 → 预留 → 分发 FrameWriter
    // =========================================================================

    REGISTER_SYSTEM(BuilderUpload, PreRender, Worker)
        .Func([this](Registry &reg, const MessageContext &) {
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
        .Func([this](Registry &reg, const MessageContext &) { m_opaqueBuilder->BuildTyped(reg, m_opaqueQueue); })
        .AlwaysRun()
        .DependsOn("BuilderUpload")
        .Build();

    if (m_skinnedBuilder) {
        REGISTER_SYSTEM(BuildSkinned, PreRender, Worker)
            .Func([this](Registry &reg, const MessageContext &) { m_skinnedBuilder->BuildTyped(reg, m_skinnedQueue); })
            .AlwaysRun()
            .DependsOn("BuilderUpload")
            .Build();
    }

    REGISTER_SYSTEM(BuildTransparent, PreRender, Worker)
        .Func([this](Registry &reg, const MessageContext &) {
            m_transparentBuilder->BuildTyped(reg, m_transparentQueue);
        })
        .AlwaysRun()
        .DependsOn("BuilderUpload")
        .Build();

    if (m_terrainBuilder) {
        REGISTER_SYSTEM(BuildTerrain, PreRender, Worker)
            .Func([this](Registry &reg, const MessageContext &) { m_terrainBuilder->BuildTyped(reg, m_terrainQueue); })
            .AlwaysRun()
            .DependsOn("BuilderUpload")
            .Build();
    }

    if (m_probeBuilder && m_context->ReflectionProbeMgr) {
        REGISTER_SYSTEM(BuildProbes, PreRender, Worker)
            .Func([this](Registry &reg, const MessageContext &) {
                uint32_t probeCount = m_context->ReflectionProbeMgr->GetActiveProbeCount();
                m_activeProbeCount = probeCount;
                if (probeCount > 0) {
                    m_probeBuilder->Build(m_probeCaptureInfo, m_activeProbeCount, reg, m_probeQueues);
                }
            })
            .AlwaysRun()
            .DependsOn("BuilderUpload")
            .Build();
    }

    // 水构建器
    REGISTER_SYSTEM(BuildWater, PreRender, Worker)
        .Func([this](Registry &reg, const MessageContext &) { m_waterBuilder->BuildTyped(reg, m_waterQueue); })
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

void GameWorld::RegisterAnimationAdvancer() {
    SystemRegistry::Register(
        {.name = "AnimationAdvancer",
         .func =
             [this](Registry &registry, const MessageContext &) {
                 auto *skeletonMgr = m_context->SkeletonMgr;
                 auto *frameResMgr = m_context->FrameResourceManager;
                 if (!skeletonMgr || !frameResMgr)
                     return;

                 float deltaTime = m_context->MainTimer->GetDeltaTime();

                 auto view = registry.view<ECS::SkinnedComponent>();
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
                         auto *xform = registry.TryGetComponent<ECS::TransformComponent>(entity);
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

void GameWorld::RegisterTerrainImmediateCallback() {
    if (!m_context || !m_context->FrameDriver || !m_registry)
        return;

    m_context->FrameDriver->RegisterImmediateCallback(
        [this]() {
            auto &terrainMgr = TerrainManager::GetInstance();

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
