#pragma once

#include "ECS/Core/Entity.h"
#include "Math/BoundingVolume.h"
#include "Renderer/ApplicationRenderTargets.h"
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
#include "Resource/AssetLoader/Loader/M3dLoader.h"
#include "Resource/AssetLoader/Loader/TerrainLoader.h"
#include "Resource/Manager/SkeletonManager.h"
#include "Resource/Struct/DescriptorHandle.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Renderer/Material/MaterialHandle.h"
#include "Resource/Core/GpuHandlePool.h"
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
    void OnResize(uint32_t width, uint32_t height);
    void Clear();

    DX12Engine::ECS::Registry *GetRegistry() const { return m_registry; }
    DX12Engine::ECS::Entity GetTestCube() const { return m_cubeEntity; }

    void LoadTestTexture();
    void LoadWaterTexture();
    void LoadBrickTextures();

    void CreateMaterials();

    void CreateTestCube();
    void CreateTestCylinder();
    void CreateTestTorus(); // 程序化环形圆环（SSAO 测试用：自带凹陷）
    void CreateGroundPlane();
    void CreateSkybox();
    void CreateWater();

    // 压力测试：大量动态物体（不附加 StaticComponent）
    void CreateStressTestScene();

    // 注册骨骼动画推进 System
    void RegisterAnimationAdvancer();

    // 注册蒙皮渲染 System
    void RegisterSkinnedOpaqueRenderSystem();

    // 验证 M3d 解析器
    void TestM3dLoader();

    // 加载 soldier 角色（完整管线验证）
    void LoadSoldierCharacter();

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
    void RegisterPointShadowRenderSystem();
    void RegisterSpotShadowRenderSystem();

    // 注册 SSAO 系统
    void RegisterSsaoSystem();

    // 注册反射探针捕获系统
    void RegisterProbeCaptureSystem();

    // 异步地形加载（使用 TaskGraph）
    void LoadTerrainAsync();

    // 每帧 Update（清理 BackgroundExecutor 已完成任务）
    void Update();

private:
    void RegisterClearSystem();
    void RegisterSkyboxSystem();
    void RegisterWaterRenderSystem();
    void RegisterBillboardRenderSystem();
    void RegisterTerrainRenderSystem();
    void RegisterOpaqueRenderSystem();
    void RegisterLightingPass();

    // 光照 Pass 渲染器（延迟渲染）
    std::unique_ptr<DX12Engine::Renderer::LightingRenderer> m_lightingRenderer;

    // 注册地形异步加载响应 System
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
    DX12Engine::Renderer::ProbeCaptureInfo m_probeCaptureInfo[64]; // PreRender 填充，Render 消费
    uint32_t m_activeProbeCount = 0;

    DX12Engine::Resource::TextureHandle m_testTextureHandle;
    uint32_t m_testTextureSrvSlot = UINT32_MAX;               // CBV_SRV_UAV 堆槽位索引
    DX12Engine::Resource::TextureHandle m_whiteTextureHandle; // 1x1 纯白纹理，用于反射测试立方体
    uint32_t m_whiteTextureSrvSlot = UINT32_MAX;              // CBV_SRV_UAV 堆槽位索引
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

    // 水
    DX12Engine::Resource::MaterialHandle m_waterMaterialHandle;
    DX12Engine::Resource::TextureHandle m_waterTextureHandle;
    uint32_t m_waterTextureSrvSlot = UINT32_MAX;

    // 砖块纹理（法线贴图测试）
    DX12Engine::Resource::TextureHandle m_brickTextureHandle;
    uint32_t m_brickTextureSrvSlot = UINT32_MAX;
    uint32_t m_brickNormalSrvSlot = UINT32_MAX;
    uint32_t m_brickOcclusionSrvSlot = UINT32_MAX;         // bricks2_OCC.dds
    uint32_t m_brickMetallicRoughnessSrvSlot = UINT32_MAX; // bricks2_SPEC.dds
    uint32_t m_brickHeightSrvSlot = UINT32_MAX;            // bricks2_DISP.dds
    DX12Engine::Resource::MaterialHandle m_brickMaterialHandle;
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

    // Soldier 角色
    std::vector<DX12Engine::ECS::Entity> m_soldierEntities;
    DX12Engine::Resource::SkeletonHandle m_soldierSkeletonHandle;
    float m_soldierAngle = 0.0f; // 圆周运动角度
    std::unique_ptr<DX12Engine::Renderer::SkinnedRenderItemBuilder> m_skinnedBuilder;
    std::unique_ptr<DX12Engine::Renderer::SkinnedRenderer> m_skinnedRenderer;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::SkinnedRenderItem> m_skinnedQueue;
};