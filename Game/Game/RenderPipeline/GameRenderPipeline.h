#pragma once

#include "ECS/Core/Entity.h"
#include "Renderer/ApplicationRenderTargets.h"
#include "Renderer/Material/MaterialHandle.h"
#include "Renderer/Pipeline/LightingRenderer.h"
#include "Renderer/Pipeline/ReflectionProbeRenderer.h"
#include "Renderer/Pipeline/ShadowRenderer.h"
#include "Renderer/Pipeline/SkinnedRenderer.h"
#include "Renderer/Pipeline/SkyRenderer.h"
#include "Renderer/Pipeline/TerrainRenderer.h"
#include "Renderer/Pipeline/WaterRenderer.h"
#include "Renderer/RenderItemBuilder/BillboardRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/OpaqueRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/ProbeBuilder.h"
#include "Renderer/RenderItemBuilder/SkinnedRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/TRenderQueue.h"
#include "Renderer/RenderItemBuilder/TerrainRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/TransparentRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/WaterRenderItemBuilder.h"
#include "Resource/Manager/SkeletonManager.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Struct/TextureHandle.h"
#include <DirectXMath.h>
#include <memory>
#include <vector>

namespace DX12Engine {
namespace Boot {
class GameContext;
}
namespace Renderer {
class OpaqueRenderer;
class SkyRenderer;
class WaterRenderer;
class ShadowRenderer;
class TerrainRenderer;
} // namespace Renderer
namespace ECS {
class Registry;
}
} // namespace DX12Engine

/// 游戏渲染管线 —— 组装构建器、渲染器、渲染队列和系统注册
///
/// 职责：
///   - 创建和管理所有构建器（Opaque/Transparent/Terrain/Water/Skinned/Probe）
///   - 创建和管理所有渲染器（Sky/Water/Shadow/Terrain/Skinned/Lighting）
///   - 管理渲染队列和探针捕获数据
///   - 注册所有渲染相关的 System
class GameRenderPipeline {
public:
    GameRenderPipeline() = default;
    ~GameRenderPipeline();

    GameRenderPipeline(const GameRenderPipeline &) = delete;
    GameRenderPipeline &operator=(const GameRenderPipeline &) = delete;

    void Initialize(DX12Engine::Boot::GameContext *context, DX12Engine::Renderer::OpaqueRenderer *renderer);

    void OnResize(uint32_t width, uint32_t height);

    // ====================================================================
    // 构建器访问
    // ====================================================================

    DX12Engine::Renderer::OpaqueRenderItemBuilder *GetOpaqueBuilder() const { return m_opaqueBuilder.get(); }
    DX12Engine::Renderer::TransparentRenderItemBuilder *GetTransparentBuilder() const {
        return m_transparentBuilder.get();
    }
    DX12Engine::Renderer::TerrainRenderItemBuilder *GetTerrainBuilder() const { return m_terrainBuilder.get(); }
    DX12Engine::Renderer::WaterRenderItemBuilder *GetWaterBuilder() const { return m_waterBuilder.get(); }
    DX12Engine::Renderer::SkinnedRenderItemBuilder *GetSkinnedBuilder() const { return m_skinnedBuilder.get(); }
    DX12Engine::Renderer::ProbeBuilder *GetProbeBuilder() const { return m_probeBuilder.get(); }

    // ====================================================================
    // 渲染队列访问
    // ====================================================================

    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::OpaqueRenderItem> &GetOpaqueQueue() {
        return m_opaqueQueue;
    }
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::TransparentRenderItem> &GetTransparentQueue() {
        return m_transparentQueue;
    }
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::TerrainRenderItem> &GetTerrainQueue() {
        return m_terrainQueue;
    }
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::WaterRenderItem> &GetWaterQueue() { return m_waterQueue; }
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::SkinnedRenderItem> &GetSkinnedQueue() {
        return m_skinnedQueue;
    }
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::OpaqueRenderItem> *GetProbeQueues() {
        return m_probeQueues;
    }
    DX12Engine::Renderer::ProbeCaptureInfo *GetProbeCaptureInfo() { return m_probeCaptureInfo; }
    uint32_t &GetActiveProbeCount() { return m_activeProbeCount; }

    // ====================================================================
    // 渲染器访问
    // ====================================================================

    DX12Engine::Renderer::SkyRenderer *GetSkyRenderer() const { return m_skyRenderer.get(); }
    DX12Engine::Renderer::WaterRenderer *GetWaterRenderer() const { return m_waterRenderer.get(); }
    DX12Engine::Renderer::ShadowRenderer *GetShadowRenderer() const { return m_shadowRenderer.get(); }
    DX12Engine::Renderer::TerrainRenderer *GetTerrainRenderer() const { return m_terrainRenderer.get(); }
    DX12Engine::Renderer::SkinnedRenderer *GetSkinnedRenderer() const { return m_skinnedRenderer.get(); }
    DX12Engine::Renderer::LightingRenderer *GetLightingRenderer() const { return m_lightingRenderer.get(); }
    DX12Engine::Renderer::ReflectionProbeRenderer *GetProbeRenderer() const { return m_probeRenderer.get(); }
    DX12Engine::Renderer::ApplicationRenderTargets *GetAppRenderTargets() const { return m_appRTs.get(); }

    // ====================================================================
    // 系统注册
    // ====================================================================

    void RegisterBuilderSystems();
    void RegisterAnimationAdvancer();
    void RegisterSkinnedOpaqueRenderSystem();
    void RegisterTerrainImmediateCallback();
    void RegisterProbeSceneDataCallback();
    void RegisterShadowRenderSystem();
    void RegisterPointShadowRenderSystem();
    void RegisterSpotShadowRenderSystem();
    void RegisterSsaoSystem();
    void RegisterProbeCaptureSystem();

    void RegisterClearSystem();
    void RegisterSkyboxSystem();
    void RegisterWaterRenderSystem();
    void RegisterTerrainRenderSystem();
    void RegisterOpaqueRenderSystem();
    void RegisterLightingPass();

private:
    DX12Engine::Boot::GameContext *m_context = nullptr;

    // 不透明渲染器引用（由外部传入，非本模块创建）
    DX12Engine::Renderer::OpaqueRenderer *m_renderer = nullptr;

    // 动画测试数据（士兵围绕圆心旋转）
    std::vector<DX12Engine::ECS::Entity> m_soldierEntities;
    float m_soldierAngle = 0.0f;

    // 渲染器
    std::unique_ptr<DX12Engine::Renderer::SkyRenderer> m_skyRenderer;
    std::unique_ptr<DX12Engine::Renderer::WaterRenderer> m_waterRenderer;
    std::unique_ptr<DX12Engine::Renderer::ShadowRenderer> m_shadowRenderer;
    std::unique_ptr<DX12Engine::Renderer::TerrainRenderer> m_terrainRenderer;
    std::unique_ptr<DX12Engine::Renderer::LightingRenderer> m_lightingRenderer;
    std::unique_ptr<DX12Engine::Renderer::SkinnedRenderer> m_skinnedRenderer;
    std::unique_ptr<DX12Engine::Renderer::ReflectionProbeRenderer> m_probeRenderer;

    // 构建器
    std::unique_ptr<DX12Engine::Renderer::OpaqueRenderItemBuilder> m_opaqueBuilder;
    std::unique_ptr<DX12Engine::Renderer::TransparentRenderItemBuilder> m_transparentBuilder;
    std::unique_ptr<DX12Engine::Renderer::TerrainRenderItemBuilder> m_terrainBuilder;
    std::unique_ptr<DX12Engine::Renderer::WaterRenderItemBuilder> m_waterBuilder;
    std::unique_ptr<DX12Engine::Renderer::SkinnedRenderItemBuilder> m_skinnedBuilder;
    std::unique_ptr<DX12Engine::Renderer::ProbeBuilder> m_probeBuilder;

    // 渲染队列
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::OpaqueRenderItem> m_opaqueQueue;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::TransparentRenderItem> m_transparentQueue;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::TerrainRenderItem> m_terrainQueue;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::WaterRenderItem> m_waterQueue;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::SkinnedRenderItem> m_skinnedQueue;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::OpaqueRenderItem> m_probeQueues[64];
    DX12Engine::Renderer::ProbeCaptureInfo m_probeCaptureInfo[64];
    uint32_t m_activeProbeCount = 0;

    // 视口帧缓冲
    std::unique_ptr<DX12Engine::Renderer::ApplicationRenderTargets> m_appRTs;
};