#pragma once

#include "Core/Entity.h"
#include "Core/Registry.h"
#include <memory>
#include <vector>

namespace DX12Engine {
namespace ECS {

/// World — ECS 源头
///
/// 设计原则：
///   - 所有实体必须属于某个 World，不存在游离于 World 之外的实体
///   - World 本身不分区，不感知任何逻辑分组
///   - 逻辑分区由 Manager 的"视角"提供（SceneManager、PreviewManager 等）
///   - 内部 System 通过 GetRegistry() 访问完整 ECS 能力（view/group/ctx）
///   - 外部消费者通过 World 的受控 API 操作实体
class World {
public:
    World() = default;
    ~World();

    World(const World &) = delete;
    World &operator=(const World &) = delete;

    // ====================================================================
    // 初始化/销毁
    // ====================================================================

    void Initialize();
    void Shutdown();

    // ====================================================================
    // 实体生命周期（唯一入口）
    // ====================================================================

    /// 创建空实体，返回 Entity handle
    Entity CreateEntity();

    /// 批量创建空实体
    std::vector<Entity> CreateEntities(uint32_t count);

    /// 销毁实体
    void DestroyEntity(Entity entity);

    /// 销毁所有实体
    void RemoveAllEntities();

    /// 检查实体是否有效（未被销毁）
    bool IsValid(Entity entity) const;

    // ====================================================================
    // 组件访问（受控）
    // ====================================================================

    template <typename T> T *GetComponent(Entity entity) { return m_registry->TryGetComponent<T>(entity); }

    template <typename T> const T *GetComponent(Entity entity) const { return m_registry->TryGetComponent<T>(entity); }

    template <typename T, typename... Args> T &AddComponent(Entity entity, Args &&...args) {
        return m_registry->AddComponent<T>(entity, std::forward<Args>(args)...);
    }

    template <typename T> void RemoveComponent(Entity entity) { m_registry->RemoveComponent<T>(entity); }

    template <typename T> bool HasComponent(Entity entity) const { return m_registry->HasComponent<T>(entity); }

    // ====================================================================
    // 内部 System 访问
    // ====================================================================

    /// 内部 System 通过此方法访问完整的 ECS 能力（view/group/ctx 等）
    Registry *GetRegistry() { return m_registry.get(); }
    const Registry *GetRegistry() const { return m_registry.get(); }

private:
    std::unique_ptr<Registry> m_registry;
};

} // namespace ECS
} // namespace DX12Engine