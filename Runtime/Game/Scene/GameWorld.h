#pragma once

#include "Async/TerrainLoadTask.h"
#include "ECS/Core/Entity.h"
#include "Math/BoundingVolume.h"
#include "Renderer/Pipeline/ReflectionProbeRenderer.h"
#include "Renderer/RenderItemBuilder/BillboardRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/OpaqueRenderItemBuilder.h"
#include "Renderer/RenderItemBuilder/ProbeBuilder.h"
#include "Renderer/RenderItemBuilder/TRenderQueue.h"
#include "Renderer/RenderItemBuilder/TerrainRenderItemBuilder.h"
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
class BillboardRenderer;
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

    // 压力测试：大量动态物体（不附加 StaticComponent）
    void CreateStressTestScene();

    // 公告牌（Billboard）系统
    void LoadBillboardTextures();
    void CreateBillboardTrees();

    // 注册构建器 System（PreRender 阶段并行执行）
    void RegisterBuilderSystems();

    // 注册水常量立即回调（每帧上传水波动画数据）
    void RegisterWaterConstantsCallback();

    // 注册地形常量立即回调（LightManager 模式：Immediate 中分配+上传地形常量）
    void RegisterTerrainImmediateCallback();

    // 注册探针场景数据上传回调（SceneDataUpload 阶段：填充 ProbeCaptureInfo + 分配 CB）
    void RegisterProbeSceneDataCallback();

    // 注册阴影渲染系统
    void RegisterShadowRenderSystem();

    // 注册反射探针捕获系统
    void RegisterProbeCaptureSystem();

    // 异步地形加载（使用 TaskGraph）
    void LoadTerrainAsync();

    // 每帧 Update（清理 BackgroundExecutor 已完成任务）
    void Update();

private:
    void RegisterRotationSystem();
    void RegisterClearSystem();
    void RegisterCubeRenderSystem();
    void RegisterSkyboxSystem();
    void RegisterWaterRenderSystem();
    void RegisterBillboardRenderSystem();
    void RegisterTerrainRenderSystem();

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
    std::unique_ptr<DX12Engine::Renderer::ShadowRenderer> m_shadowRenderer;   // 阴影渲染器
    std::unique_ptr<DX12Engine::Renderer::TerrainRenderer> m_terrainRenderer; // 地形渲染器

    // 构建器和渲染队列（统一实例化模式）
    std::unique_ptr<DX12Engine::Renderer::OpaqueRenderItemBuilder> m_opaqueBuilder;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::OpaqueRenderItem> m_opaqueQueue;

    std::unique_ptr<DX12Engine::Renderer::TransparentRenderItemBuilder> m_transparentBuilder;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::TransparentRenderItem> m_transparentQueue;

    std::unique_ptr<DX12Engine::Renderer::TerrainRenderItemBuilder> m_terrainBuilder;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::TerrainRenderItem> m_terrainQueue;

    // 反射探针捕获
    std::unique_ptr<DX12Engine::Renderer::ProbeBuilder> m_probeBuilder;
    std::unique_ptr<DX12Engine::Renderer::ReflectionProbeRenderer> m_probeRenderer;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::OpaqueRenderItem> m_probeQueues[64];
    DX12Engine::Renderer::ProbeCaptureInfo m_probeCaptureInfo[64]; // PreRender 填充，Render 消费
    uint32_t m_activeProbeCount = 0;

    DX12Engine::Resource::TextureHandle m_testTextureHandle;
    DX12Engine::Resource::TextureHandle m_whiteTextureHandle; // 1x1 纯白纹理，用于反射测试立方体
    DX12Engine::Resource::MaterialHandle m_cubeMaterialHandle;
    DX12Engine::Resource::MaterialHandle m_reflectionTestMaterialHandle; // 金属材质（反射探针测试用）

    DX12Engine::Resource::GpuResourceHandle m_materialBufferHandle; // 材质数组 GPU Buffer 句柄
    DX12Engine::ECS::Entity m_cubeEntity;
    DX12Engine::ECS::Entity m_reflectionCubeEntity;
    std::vector<DX12Engine::ECS::Entity> m_cubeEntities; // 多个立方体实体

    // 压力测试实体
    std::vector<DX12Engine::ECS::Entity> m_stressEntities;

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
    DX12Engine::Resource::TextureHandle m_terrainAlbedoHandle = DX12Engine::Resource::TextureHandle::Invalid();
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

    // 公告牌（Billboard）系统
    std::unique_ptr<DX12Engine::Renderer::BillboardRenderer> m_billboardRenderer;
    std::unique_ptr<DX12Engine::Renderer::BillboardRenderItemBuilder> m_billboardBuilder;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::BillboardRenderItem> m_billboardQueue;
    DX12Engine::Resource::MaterialHandle m_billboardMaterialHandle;
    DX12Engine::Resource::TextureHandle m_billboardTextureHandles[4]; // 公告牌 Texture2DArray 句柄
    uint32_t m_billboardSliceOffsets[4] = {};                         // 每个逻辑纹理的切片起始偏移
    uint32_t m_billboardTotalSlices = 0;                              // 总切片数
    std::vector<DX12Engine::ECS::Entity> m_billboardEntities;         // 树公告牌实体

    // 后台异步执行器（纯 CPU 线程池，独立于 FrameDriver）
    std::unique_ptr<DX12Engine::Async::BackgroundExecutor> m_backgroundExecutor;
};