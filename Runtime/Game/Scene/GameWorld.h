#pragma once

#include "Async/TerrainLoadTask.h"
#include "ECS/Core/Entity.h"
#include "Math/BoundingVolume.h"
#include "Renderer/RenderItemBuilder/OpaqueRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/TRenderQueue.h"
#include "Renderer/RenderItemBuilder/TransparentRenderItemBuilder.h"
#include "Resource/AssetLoader/Loader/TerrainLoader.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Struct/MaterialHandle.h"
#include "Resource/Struct/ResourceHandle.h"
#include "Resource/Struct/TextureHandle.h"
#include "Scheduler/Task.h"
#include <memory>
#include <vector>

namespace DX12Engine {
namespace Boot {
class GameContext;
}
namespace Async {
class BackgroundExecutor;
}
namespace ECS {
class Registry;
}
namespace Renderer {
class OpaqueRenderer;
class SkyRenderer;
class WaterRenderer;
class ShadowRenderer;
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
    void Clear();

    DX12Engine::ECS::Registry *GetRegistry() const { return m_registry; }
    DX12Engine::ECS::Entity GetTestCube() const { return m_cubeEntity; }

    void LoadTestTexture();
    void LoadWaterTexture();

    void CreateMaterials();

    void CreateTestCube();
    void CreateGroundPlane();
    void CreateSkybox();
    void CreateWater();

    // 每帧 PreRender 阶段：收集实体并构建渲染队列
    void BuildRenderQueue();

    // 注册水常量立即回调（每帧上传水波动画数据）
    void RegisterWaterConstantsCallback();

    // 注册阴影渲染系统
    void RegisterShadowRenderSystem();

    // 异步地形加载（使用 TaskGraph）
    void LoadTerrainAsync();

    // 每帧 Update（清理 BackgroundExecutor 已完成任务）
    void Update();

private:
    void RegisterRotationSystem();
    void RegisterCubeRenderSystem();
    void RegisterSkyboxSystem();
    void RegisterWaterRenderSystem();

    // 注册地形异步加载响应 System
    // 架构：后台线程完成所有 GPU 资源创建 + 上传，主线程只注册句柄 + 创建 ECS 实体
    //
    // 数据流：
    //   LoadTerrainAsync() → BackgroundExecutor::Submit(TerrainLoadTask)
    //     后台线程: CPU加载 → 创建VB/IB (UPLOAD堆) → 创建纹理(DEFAULT堆)
    //              → COPY队列上传 → ResourceTransition → 写入 TerrainReadyState
    //              → PostEvent(TerrainLoaded)
    //
    //   TerrainGPUCreateSystem (主线程，响应 TerrainLoaded):
    //     从 TerrainReadyState 读取 GpuResourceHandle + srvIndex → 注册 GeometryHandle/TextureHandle
    //     → PostEvent(TerrainReady)
    //
    //   TerrainCombineSystem (主线程，响应 TerrainReady):
    //     注册 LODMesh → 创建 ECS 实体
    void RegisterTerrainSystems();

private:
    DX12Engine::Boot::GameContext *m_context = nullptr;
    DX12Engine::ECS::Registry *m_registry = nullptr;

    DX12Engine::Renderer::OpaqueRenderer *m_renderer = nullptr;
    std::unique_ptr<DX12Engine::Renderer::SkyRenderer> m_skyRenderer; // 天空盒渲染器
    std::unique_ptr<DX12Engine::Renderer::WaterRenderer> m_waterRenderer;
    std::unique_ptr<DX12Engine::Renderer::ShadowRenderer> m_shadowRenderer; // 阴影渲染器

    // 新的构建器和渲染队列（由 GameWorld 持有）
    std::unique_ptr<DX12Engine::Renderer::OpaqueRenderItemBuilder> m_opaqueBuilder;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::OpaqueRenderItem> m_opaqueQueue;

    std::unique_ptr<DX12Engine::Renderer::TransparentRenderItemBuilder> m_transparentBuilder;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::TransparentRenderItem> m_transparentQueue;

    DX12Engine::Resource::TextureHandle m_testTextureHandle; // 存储纹理句柄
    DX12Engine::Resource::MaterialHandle m_cubeMaterialHandle;
    DX12Engine::Resource::GpuResourceHandle m_materialBufferHandle; // 材质数组 GPU Buffer 句柄
    DX12Engine::ECS::Entity m_cubeEntity;
    std::vector<DX12Engine::ECS::Entity> m_cubeEntities;    // 多个立方体实体

    // 地面平面
    DX12Engine::ECS::Entity m_groundPlaneEntity;
    DX12Engine::Resource::MaterialHandle m_groundPlaneMaterialHandle;

    // 天空盒数据
    DX12Engine::Resource::TextureHandle m_skyboxTextureHandle;   // 天空盒纹理 SRV
    DX12Engine::Resource::GeometryHandle m_skyboxGeometryHandle; // 天空盒几何体
    D3D12_GPU_VIRTUAL_ADDRESS m_skyboxObjectCBAddress = 0;       // 天空盒单位矩阵 CBV 地址

    // 地形数据
    DX12Engine::Resource::GeometryHandle m_terrainGeometryHandle;
    DX12Engine::Resource::MaterialHandle m_terrainMaterialHandle;
    DX12Engine::Resource::TextureHandle m_terrainTextureHandle = DX12Engine::Resource::TextureHandle::Invalid();
    DX12Engine::ECS::Entity m_terrainEntity;

    // 地形异步加载状态（后台线程写入 GPU 资源，主线程读取后注册句柄）
    DX12Engine::Async::TerrainReadyStatePtr m_terrainReadyState; // 存储后台线程的资源
    DX12Engine::Async::TerrainLoadDataPtr m_terrainLoadData;     // 存储几何体数据
    uint32_t m_terrainRequestId = 0; // ID关联同一个地形加载请求的不同阶段，也就是说明后台线程是关联到那个任务和请求的

    // 水
    DX12Engine::Resource::MaterialHandle m_waterMaterialHandle;
    DX12Engine::Resource::TextureHandle m_waterTextureHandle;
    DX12Engine::ECS::Entity m_waterEntity;
    D3D12_GPU_VIRTUAL_ADDRESS m_waterCBAddress = 0;

    // 后台异步执行器（纯 CPU 线程池，独立于 FrameDriver）
    std::unique_ptr<DX12Engine::Async::BackgroundExecutor> m_backgroundExecutor;
};