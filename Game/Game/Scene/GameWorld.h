#pragma once

#include "ECS/Core/Entity.h"
#include "RenderPipeline/GameRenderPipeline.h"
#include "Resources/GameResources.h"
#include "Scene/GameSceneManager.h"
#include <memory>

namespace DX12Engine {
namespace Boot {
class GameContext;
}
namespace ECS {
class Registry;
}
namespace Renderer {
class OpaqueRenderer;
}
} // namespace DX12Engine

/// 游戏世界管理器
///
/// 语义拆分：
///   - GameSceneManager：场景生命周期管理
///   - GameRenderPipeline：渲染管线（构建器、渲染器、队列、系统注册）
///   - GameResources：GPU 资源初始化（白纹理、组件预触）
///   - GameWorld：世界主循环（Update、资源回收、场景驱动）
class GameWorld {
public:
    GameWorld();
    ~GameWorld();

    GameWorld(const GameWorld &) = delete;
    GameWorld &operator=(const GameWorld &) = delete;
    GameWorld(GameWorld &&) noexcept = default;
    GameWorld &operator=(GameWorld &&) noexcept = default;

    void Initialize(DX12Engine::Boot::GameContext *context, DX12Engine::Renderer::OpaqueRenderer *renderer);
    void OnResize(uint32_t width, uint32_t height);
    void Clear();

    DX12Engine::ECS::Registry *GetRegistry() const { return m_gameSceneMgr.GetRegistry(); }

    // 每帧 Update（清理 BackgroundExecutor 已完成任务，驱动场景生命周期）
    void Update();

    // 子模块访问
    GameSceneManager *GetSceneManager() { return &m_gameSceneMgr; }
    GameRenderPipeline *GetRenderPipeline() { return &m_renderPipeline; }
    GameResources *GetResources() { return &m_gameResources; }

    // ====================================================================
    // 系统注册（委托给 GameRenderPipeline，实现在 GameWorld_*.cpp 中）
    // ====================================================================

    void RegisterBuilderSystems() { m_renderPipeline.RegisterBuilderSystems(); }
    void RegisterAnimationAdvancer() { m_renderPipeline.RegisterAnimationAdvancer(); }
    void RegisterSkinnedOpaqueRenderSystem() { m_renderPipeline.RegisterSkinnedOpaqueRenderSystem(); }
    void RegisterTerrainImmediateCallback() { m_renderPipeline.RegisterTerrainImmediateCallback(); }
    void RegisterProbeSceneDataCallback() { m_renderPipeline.RegisterProbeSceneDataCallback(); }
    void RegisterShadowRenderSystem() { m_renderPipeline.RegisterShadowRenderSystem(); }
    void RegisterPointShadowRenderSystem() { m_renderPipeline.RegisterPointShadowRenderSystem(); }
    void RegisterSpotShadowRenderSystem() { m_renderPipeline.RegisterSpotShadowRenderSystem(); }
    void RegisterSsaoSystem() { m_renderPipeline.RegisterSsaoSystem(); }
    void RegisterProbeCaptureSystem() { m_renderPipeline.RegisterProbeCaptureSystem(); }

private:
    DX12Engine::Boot::GameContext *m_context = nullptr;

    // 场景管理器
    GameSceneManager m_gameSceneMgr;

    // 渲染管线（构建器、渲染器、队列、系统注册）
    GameRenderPipeline m_renderPipeline;

    // GPU 资源（白纹理、组件预触）
    GameResources m_gameResources;
};
