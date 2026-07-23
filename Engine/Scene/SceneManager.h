#pragma once

#include "Asset/IO/Loader/SceneDescription.h"
#include "ECS/Core/Entity.h"
#include "ECS/World.h"
#include "RenderScene.h"
#include <cstdint>
#include <memory>
#include <string>

namespace DX12Engine {

namespace Boot {
class GameContext;
}

namespace Scene {

// ========================================================================
// 场景生命周期状态
// ========================================================================

enum class SceneState {
    None,      // 无场景
    Loading,   // 正在异步加载
    Active,    // 实体已构造，完全运行中
    Unloading, // 正在卸载
};

// ========================================================================
// 场景切换模式
// ========================================================================

enum class SceneTransition {
    Immediate, // 立即切换（旧场景卸载 → 新场景加载）
    Additive,  // 叠加（不卸载旧场景，新实体追加到世界）
};

// ========================================================================
// 场景事件类型（通过 MessageDispatcher 分发）
// ========================================================================

/// 实体变更事件（通过 MessageDispatcher 异步分发）
struct EntityChangeEvent {
    enum Type { Added, Removed, ComponentChanged, TransformChanged };
    Type type;
    uint64_t entity; // EntityHandle
};

/// 场景生命周期事件（通过 MessageDispatcher 异步分发）
struct SceneLifecycleEvent {
    SceneState oldState;
    SceneState newState;
    std::string sceneName;
};

// ========================================================================
// 场景管理器基类
//
// 设计原则：
//   - World 是 ECS 绝对源头，SceneManager 不再持有 Registry
//   - SceneManager 是场景序列化器 + 环境状态容器
//   - 实体生命周期委托给 World，SceneManager 只管理场景语义
// ========================================================================

class SceneManager {
public:
    SceneManager() = default;
    virtual ~SceneManager();

    SceneManager(const SceneManager &) = delete;
    SceneManager &operator=(const SceneManager &) = delete;

    // ====================================================================
    // 初始化/销毁
    // ====================================================================

    /// 初始化（绑定 World 实例；同时创建 RenderScene 渲染上下文）
    virtual void Initialize(ECS::World *world);

    /// 销毁，清理所有实体
    virtual void Shutdown();

    // ====================================================================
    // World 访问
    // ====================================================================

    /// 获取绑定的 World 实例
    ECS::World *GetWorld() const { return m_world; }

    // ====================================================================
    // 实体管理（委托给 World）
    // ====================================================================

    /// 创建空实体（委托给 World，返回 uint64_t handle）
    uint64_t CreateEntity();

    /// 批量创建空实体
    std::vector<uint64_t> CreateEntities(uint32_t count);

    /// 移除实体（委托给 World）
    void RemoveEntity(uint64_t entity);

    /// 移除所有实体（场景全量切换时使用）
    void RemoveAllEntities();

    /// 获取实体组件（通过 World 访问）
    template <typename T> T *GetComponent(uint64_t entity) {
        return m_world ? m_world->GetComponent<T>(static_cast<ECS::Entity>(entity)) : nullptr;
    }

    // ====================================================================
    // 环境状态
    // ====================================================================

    void SetSkybox(const Resource::SkyboxDesc &skybox);
    void SetEnvironment(const Resource::EnvironmentDesc &env);

    const Resource::SkyboxDesc &GetSkybox() const { return m_skybox; }
    const Resource::EnvironmentDesc &GetEnvironment() const { return m_environment; }

    // ====================================================================
    // 场景生命周期
    // ====================================================================

    SceneState GetState() const { return m_state; }
    const std::string &GetCurrentSceneName() const { return m_sceneName; }

    /// 准备切换场景：清除所有实体 → 更新状态
    void PrepareSceneSwitch(const std::string &newSceneName, SceneTransition transition);

    // ====================================================================
    // 子场景模块管理
    // ====================================================================

    /// 获取 RenderScene 渲染上下文容器
    RenderScene *GetRenderScene() const { return m_renderScene.get(); }

    /// 获取内部 Registry 指针（供 Builder 系统获取 ECS 上下文）
    /// 委托给 World::GetRegistry()
    ECS::Registry *GetRegistry() const { return m_world ? m_world->GetRegistry() : nullptr; }

protected:
    // ====================================================================
    // 内部 System 访问（不对外暴露）
    // ====================================================================

    /// 内部 System 通过此方法访问完整的 ECS 能力
    ECS::Registry *GetRegistryForInternalUse() { return m_world ? m_world->GetRegistry() : nullptr; }

    // 子类可访问的状态
    SceneState m_state = SceneState::None;
    std::string m_sceneName;

    // World 引用（非拥有，由 Bootstrap 创建并传入）
    ECS::World *m_world = nullptr;

    // 环境状态
    Resource::SkyboxDesc m_skybox;
    Resource::EnvironmentDesc m_environment;

    // 渲染上下文容器（直接持有）
    std::unique_ptr<RenderScene> m_renderScene;
};

} // namespace Scene
} // namespace DX12Engine