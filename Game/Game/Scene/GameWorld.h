#pragma once

#include "ECS/Core/Entity.h"
#include "Math/BoundingVolume.h"
#include "Renderer/ApplicationRenderTargets.h"
#include "Renderer/Material/MaterialHandle.h"
#include "Renderer/Pipeline/LightingRenderer.h"
#include "Renderer/Pipeline/ReflectionProbeRenderer.h"
#include "Renderer/Pipeline/SkinnedRenderer.h"
#include "Renderer/RenderItemBuilder/BillboardRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/OpaqueRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/ProbeBuilder.h"
#include "Renderer/RenderItemBuilder/SkinnedRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/TRenderQueue.h"
#include "Renderer/RenderItemBuilder/TerrainRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/TransparentRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/WaterRenderItemBuilder.h"
#include "Resource/Core/GpuHandlePool.h"
#include "Resource/Manager/SkeletonManager.h"
#include "Resource/Struct/DescriptorHandle.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Struct/TextureHandle.h"
#include "Scheduler/Task.h"
#include <memory>
#include <vector>

namespace DX12Engine {
namespace Boot {
class GameContext;
}
// Async::BackgroundExecutor — 由 Bootstrap 创建，通过 GameContext 访问
namespace Scene {
class SceneConstructor;
}
namespace ECS {
class Registry;
}
namespace Renderer {
class OpaqueRenderer;
class SkyRenderer;
class WaterRenderer;
class ShadowRenderer;
class TerrainRenderer;
} // namespace Renderer
} // namespace DX12Engine

/**
 * @brief 游戏世界管理器
 */
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

    DX12Engine::ECS::Registry *GetRegistry() const { return m_registry; }

    // 注册构建器 System（PreRender 阶段并行执行）
    void RegisterBuilderSystems();

    // 注册骨骼动画推进 System
    void RegisterAnimationAdvancer();

    // 注册蒙皮渲染 System
    void RegisterSkinnedOpaqueRenderSystem();

    // 注册地形常量立即回调（LightManager 模式：Immediate 中分配+上传地形常量）
    void RegisterTerrainImmediateCallback();

    // 注册探针场景数据上传回调（SceneDataUpload 阶段：填充 ProbeCaptureInfo + 分配 CB）
    void RegisterProbeSceneDataCallback();

    // 注册阴影渲染系统
    void RegisterShadowRenderSystem();
    void RegisterPointShadowRenderSystem();
    void RegisterSpotShadowRenderSystem();

    // 注册 SSAO 系统
    void RegisterSsaoSystem();

    // 注册反射探针捕获系统
    void RegisterProbeCaptureSystem();

    // 每帧 Update（清理 BackgroundExecutor 已完成任务）
    void Update();

private:
    void RegisterClearSystem();
    void RegisterSkyboxSystem();
    void RegisterWaterRenderSystem();
    void RegisterTerrainRenderSystem();
    void RegisterOpaqueRenderSystem();
    void RegisterLightingPass();

    // 光照 Pass 渲染器（延迟渲染）
    std::unique_ptr<DX12Engine::Renderer::LightingRenderer> m_lightingRenderer;

    // 注册场景构造响应 System
    void RegisterSceneConstructSystem();

private:
    DX12Engine::Boot::GameContext *m_context = nullptr;
    DX12Engine::ECS::Registry *m_registry = nullptr;

    DX12Engine::Renderer::OpaqueRenderer *m_renderer = nullptr;
    std::unique_ptr<DX12Engine::Renderer::SkyRenderer> m_skyRenderer;
    std::unique_ptr<DX12Engine::Renderer::WaterRenderer> m_waterRenderer;
    std::unique_ptr<DX12Engine::Renderer::ShadowRenderer> m_shadowRenderer;
    std::unique_ptr<DX12Engine::Renderer::TerrainRenderer> m_terrainRenderer;

    // 构建器和渲染队列（统一实例化模式）
    std::unique_ptr<DX12Engine::Renderer::OpaqueRenderItemBuilder> m_opaqueBuilder;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::OpaqueRenderItem> m_opaqueQueue;

    // G-buffer 由 OpaqueRenderer 的 G-buffer 通道完成
    // 视口帧缓冲管理器（G-buffer RT 的统一分配与重建）
    std::unique_ptr<DX12Engine::Renderer::ApplicationRenderTargets> m_appRTs;

    std::unique_ptr<DX12Engine::Renderer::TransparentRenderItemBuilder> m_transparentBuilder;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::TransparentRenderItem> m_transparentQueue;

    std::unique_ptr<DX12Engine::Renderer::TerrainRenderItemBuilder> m_terrainBuilder;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::TerrainRenderItem> m_terrainQueue;

    // 反射探针捕获
    std::unique_ptr<DX12Engine::Renderer::ProbeBuilder> m_probeBuilder;
    std::unique_ptr<DX12Engine::Renderer::ReflectionProbeRenderer> m_probeRenderer;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::OpaqueRenderItem> m_probeQueues[64];
    DX12Engine::Renderer::ProbeCaptureInfo m_probeCaptureInfo[64];
    uint32_t m_activeProbeCount = 0;

    DX12Engine::Resource::TextureHandle m_whiteTextureHandle; // 1x1 纯白纹理，用于反射测试立方体
    uint32_t m_whiteTextureSrvSlot = UINT32_MAX;

    // 后台异步执行器（由 Bootstrap 创建，通过 GameContext 访问）

    // 场景构造器（异步加载 + 构造 ECS 实体）
    std::unique_ptr<DX12Engine::Scene::SceneConstructor> m_sceneConstructor;
    int m_asyncLoadDelay = 0;
    std::string m_asyncScenePath;

    // Soldier 角色
    std::vector<DX12Engine::ECS::Entity> m_soldierEntities;
    DX12Engine::Resource::SkeletonHandle m_soldierSkeletonHandle;
    float m_soldierAngle = 0.0f;
    std::unique_ptr<DX12Engine::Renderer::SkinnedRenderItemBuilder> m_skinnedBuilder;
    std::unique_ptr<DX12Engine::Renderer::SkinnedRenderer> m_skinnedRenderer;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::SkinnedRenderItem> m_skinnedQueue;

    // 水系统
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::WaterRenderItem> m_waterQueue;
    std::unique_ptr<DX12Engine::Renderer::WaterRenderItemBuilder> m_waterBuilder;
};
