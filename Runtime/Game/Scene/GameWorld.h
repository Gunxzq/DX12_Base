#pragma once

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
namespace ECS {
class Registry;
}
namespace Renderer {
class OpaqueRenderer;
class SkyRenderer;
class WaterRenderer;
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

    void CreateTerrain();
    void CreateTestCube();
    void CreateSkybox();
    void CreateWater();

    // 每帧 PreRender 阶段：收集实体并构建渲染队列
    void BuildRenderQueue();

    // 注册水常量立即回调（每帧上传水波动画数据）
    void RegisterWaterConstantsCallback();

private:
    void RegisterRotationSystem();
    void RegisterCubeRenderSystem();
    void RegisterSkyboxSystem();
    void RegisterWaterRenderSystem();

    void CreateTerrainMesh();
    void UploadTerrainGeometry();
    void CreateTerrainMaterial();
    void CreateTerrainEntity();

private:
    DX12Engine::Boot::GameContext *m_context = nullptr;
    DX12Engine::ECS::Registry *m_registry = nullptr;

    DX12Engine::Renderer::OpaqueRenderer *m_renderer = nullptr;
    std::unique_ptr<DX12Engine::Renderer::SkyRenderer> m_skyRenderer; // 天空盒渲染器
    std::unique_ptr<DX12Engine::Renderer::WaterRenderer> m_waterRenderer;

    // 新的构建器和渲染队列（由 GameWorld 持有）
    std::unique_ptr<DX12Engine::Renderer::OpaqueRenderItemBuilder> m_opaqueBuilder;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::OpaqueRenderItem> m_opaqueQueue;

    std::unique_ptr<DX12Engine::Renderer::TransparentRenderItemBuilder> m_transparentBuilder;
    DX12Engine::Renderer::TRenderQueue<DX12Engine::Renderer::TransparentRenderItem> m_transparentQueue;

    DX12Engine::Resource::TextureHandle m_testTextureHandle; // 存储纹理句柄
    DX12Engine::Resource::MaterialHandle m_cubeMaterialHandle;
    DX12Engine::Resource::GpuResourceHandle m_materialBufferHandle; // 材质数组 GPU Buffer 句柄
    DX12Engine::ECS::Entity m_cubeEntity;

    // 天空盒数据
    DX12Engine::Resource::TextureHandle m_skyboxTextureHandle;   // 天空盒纹理 SRV
    DX12Engine::Resource::GeometryHandle m_skyboxGeometryHandle; // 天空盒几何体
    D3D12_GPU_VIRTUAL_ADDRESS m_skyboxObjectCBAddress = 0;       // 天空盒单位矩阵 CBV 地址

    // 地形数据
    DX12Engine::Resource::TerrainMeshData m_terrainMeshData;
    DX12Engine::Resource::GeometryHandle m_terrainGeometryHandle;
    DX12Engine::Resource::MaterialHandle m_terrainMaterialHandle;
    DX12Engine::Resource::TextureHandle m_terrainTextureHandle;
    DX12Engine::ECS::Entity m_terrainEntity;

    // 水
    DX12Engine::Resource::MaterialHandle m_waterMaterialHandle;
    DX12Engine::Resource::TextureHandle m_waterTextureHandle;
    DX12Engine::ECS::Entity m_waterEntity;
    D3D12_GPU_VIRTUAL_ADDRESS m_waterCBAddress = 0;
};