#pragma once

#include "ECS/Core/Entity.h"
#include "Renderer/RHI/PassConstants.h"
#include "Scheduler/Task.h"
#include <array>
#include <memory>
#include <vector>
#include <wrl/client.h>

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
 *
 * 职责：
 * 1. 管理游戏场景中的所有实体（创建、销毁、查询）
 * 2. 注册和管理游戏世界相关的 ECS 系统
 * 3. 处理场景加载/卸载
 */
class GameWorld {
public:
    GameWorld();
    ~GameWorld();

    // 禁止拷贝
    GameWorld(const GameWorld &) = delete;
    GameWorld &operator=(const GameWorld &) = delete;

    // 允许移动
    GameWorld(GameWorld &&) noexcept = default;
    GameWorld &operator=(GameWorld &&) noexcept = default;

    void Initialize(DX12Engine::Boot::GameContext *context, DX12Engine::Renderer::OpaqueRenderer *renderer);

    void Clear();

    DX12Engine::ECS::Registry *GetRegistry() const { return m_registry; }

    // ========================================================================
    // 场景对象创建
    // ========================================================================

    void CreateTestCube();

    DX12Engine::ECS::Entity GetTestCube() const { return m_cubeEntity; }

    void InitializePassConstantBuffers();

    void RegisterSystems();

private:
    DX12Engine::Boot::GameContext *m_context = nullptr;
    DX12Engine::ECS::Registry *m_registry = nullptr;
    DX12Engine::Renderer::OpaqueRenderer *m_renderer = nullptr;

    // 测试立方体实体
    DX12Engine::ECS::Entity m_cubeEntity;

    // 系统注册 ID（用于注销）
    // std::vector<uint32_t> m_systemIds;

private:
    D3D12_GPU_VIRTUAL_ADDRESS GetCurrentPassCBAddress() const;
    struct PassCBResource {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        void *mappedData = nullptr;
    };
    std::array<PassCBResource, 3> m_passCBResources;

public:
    const PassCBResource &GetPassCBResource(uint32_t frameIndex) const { return m_passCBResources[frameIndex]; }
};
