#pragma once

#include "ECS/Core/Entity.h"
#include "Math/BoundingVolume.h"
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
}
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

    void CreateTestCube();
    void LoadTestTexture(); // 新增：加载测试纹理
    // void CreateTerrain();   // 新增：创建地形（后续实现）

    void RegisterRotationSystem();
    void RegisterCubeRenderSystem();

private:
    DX12Engine::Boot::GameContext *m_context = nullptr;
    DX12Engine::ECS::Registry *m_registry = nullptr;
    DX12Engine::Renderer::OpaqueRenderer *m_renderer = nullptr;
    DX12Engine::Resource::TextureHandle m_testTextureHandle; // 存储纹理句柄
    DX12Engine::ECS::Entity m_cubeEntity;
};