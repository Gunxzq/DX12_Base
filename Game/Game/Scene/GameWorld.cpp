#include "GameWorld.h"
#include "Background/BackgroundExecutor.h"
#include "Boot/GameContext.h"
#include "ECS/Core/Registry.h"
#include "Logger/Logger.h"
#include "Renderer/Effects/AO/AmbientOcclusionManager.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/Pipeline/OpaqueRenderer.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/RHI/Command/Fence/FenceManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/Pool/DepthStencilPool.h"
#include "Resource/Pool/RenderTargetPool.h"
#include "Resource/Texture/TextureManager.h"
#include "Scheduler/FrameDriver.h"
#include <DirectXMath.h>

using namespace DX12Engine;
using namespace DX12Engine::Boot;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Scheduler;

// ========================================================================
// GameWorld — 核心生命周期
// ========================================================================

GameWorld::GameWorld() = default;
GameWorld::~GameWorld() = default;

void GameWorld::Initialize(GameContext *context, OpaqueRenderer *renderer) {
    m_context = context;

    // 初始化场景管理器
    m_gameSceneMgr.Initialize(context->SceneMgr, context);

    // 初始化渲染管线（构建器、渲染器、队列）
    m_renderPipeline.Initialize(context, renderer);

    // 初始化 GPU 资源（白纹理、组件预触）
    m_gameResources.Initialize(context);

    // 异步加载测试场景
    {
        std::error_code ec;
        std::filesystem::path asyncPath = m_context->ResolvePath(L"Content/Scenes/async_test.scene.json");
        if (std::filesystem::exists(asyncPath, ec)) {
            m_gameSceneMgr.LoadSceneAsync(asyncPath.string());
        } else {
            m_context->Logging->Warn("[GameWorld] async_test.json not found");
        }
    }

    // 注册系统（实现在 GameRenderPipeline.cpp 中）
    m_renderPipeline.RegisterTerrainImmediateCallback();
    m_renderPipeline.RegisterOpaqueRenderSystem();
    m_renderPipeline.RegisterPointShadowRenderSystem();
    m_renderPipeline.RegisterLightingPass();
    m_renderPipeline.RegisterSkyboxSystem();
    m_renderPipeline.RegisterClearSystem();
    m_renderPipeline.RegisterShadowRenderSystem();
    m_renderPipeline.RegisterSpotShadowRenderSystem();
    m_renderPipeline.RegisterSsaoSystem();
    m_renderPipeline.RegisterWaterRenderSystem();
    m_renderPipeline.RegisterTerrainRenderSystem();
    m_renderPipeline.RegisterAnimationAdvancer();
    m_renderPipeline.RegisterSkinnedOpaqueRenderSystem();
    m_renderPipeline.RegisterBuilderSystems();
    m_renderPipeline.RegisterProbeSceneDataCallback();
    m_renderPipeline.RegisterProbeCaptureSystem();

    // 场景构造系统
    m_gameSceneMgr.RegisterSceneConstructSystem();

    // SSAO 白纹理回退
    {
        auto &aoMgr = AmbientOcclusionManager::GetInstance();
        aoMgr.SetEnabled(true);
        auto whiteTex = m_gameResources.GetWhiteTextureHandle();
        if (whiteTex.IsValid())
            aoMgr.SetFallbackWhiteSRV(m_context->TextureMgr->GetSRV(whiteTex));
    }
}

void GameWorld::Clear() {
    // 地面平面已由 JSON 场景管理，不再需要手动清理
}

void GameWorld::OnResize(uint32_t width, uint32_t height) {
    m_renderPipeline.OnResize(width, height);
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

            // 每 60 帧清理池中长时间未使用的 GPU 资源
            if (m_context->FrameDriver && m_context->FrameDriver->GetFrameStats().frameNumber % 60 == 0) {
                Resource::RenderTargetPool::GetInstance().PurgeUnused(
                    m_context->FrameDriver->GetFrameStats().frameNumber, 120);
                Resource::DepthStencilPool::GetInstance().PurgeUnused(
                    m_context->FrameDriver->GetFrameStats().frameNumber, 120);
            }
        }
    }
}
