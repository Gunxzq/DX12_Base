#pragma once
#include "Common/DX12API.h"
#include "Entity.h"
#include <entt/entt.hpp>
#include <stdexcept>

namespace DX12Engine {
namespace ECS {

/**
 * @brief ECS 注册表封装 (Facade Pattern)
 *
 * L2 数据层核心：纯净的 EnTT 封装，提供简洁接口
 *
 * 职责：
 * 1. 管理实体生命周期 (Entity Management)
 * 2. 管理组件数据 (Component CRUD)
 * 3. 提供视图查询 (View Query)
 * 4. 管理全局上下文 (Context/Singletons)
 *
 * 设计原则：
 * - 不包含任何具体业务逻辑
 * - 所有操作均为通用 ECS 操作
 * - 上层 (L3/L4) 不应直接接触 EnTT
 * - **不处理多缓冲**：多缓冲逻辑由 L4 层自行管理
 *
 * @note 此类是线程不安全的，多线程访问需要外部同步
 */
class DX12ECS_API Registry {
public:
    Registry() = default;
    ~Registry() = default;

    // 禁止拷贝，允许移动
    Registry(const Registry &) = delete;
    Registry &operator=(const Registry &) = delete;
    Registry(Registry &&) noexcept = default;
    Registry &operator=(Registry &&) = default;

    // =======================================================================
    // 1. 实体管理 (Entity Management)
    // =======================================================================

    /// 创建新实体
    Entity CreateEntity() { return m_registry.create(); }

    /// 销毁实体（延迟删除，通过墓碑机制）
    void DestroyEntity(Entity e) {
        if (m_registry.valid(e)) {
            m_registry.destroy(e);
        }
    }

    /// 检查实体是否有效
    bool IsValid(Entity e) const { return m_registry.valid(e); }

    /// 获取所有有效实体
    auto AllEntities() const { return m_registry.view<entt::type_list<>>(); }

    // =======================================================================
    // 2. 组件管理 (Component Management)
    // =======================================================================

    /**
     * @brief 添加/替换组件
     * @tparam T 组件类型
     * @param e 实体 ID
     * @param args 构造函数参数
     * @return 组件引用
     */
    template <typename T, typename... Args> decltype(auto) AddComponent(Entity e, Args &&...args) {
        ValidateEntity(e);
        return m_registry.emplace<T>(e, std::forward<Args>(args)...);
    }

    /**
     * @brief 移除组件
     * @tparam T 组件类型
     * @param e 实体 ID
     */
    template <typename T> void RemoveComponent(Entity e) {
        ValidateEntity(e);
        m_registry.remove<T>(e);
    }

    /**
     * @brief 获取组件引用（不存在则抛出异常）
     * @tparam T 组件类型
     */
    template <typename T> T &GetComponent(Entity e) {
        ValidateEntity(e);
        return m_registry.get<T>(e);
    }

    /// Const 版本
    template <typename T> const T &GetComponent(Entity e) const {
        ValidateEntity(e);
        return m_registry.get<T>(e);
    }

    /**
     * @brief 尝试获取组件（不存在返回 nullptr）
     * @tparam T 组件类型
     */
    template <typename T> T *TryGetComponent(Entity e) {
        if (!IsValid(e))
            return nullptr;
        return m_registry.try_get<T>(e);
    }

    /// Const 版本
    template <typename T> const T *TryGetComponent(Entity e) const {
        if (!IsValid(e))
            return nullptr;
        return m_registry.try_get<T>(e);
    }

    /// 检查实体是否拥有某组件
    template <typename T> bool HasComponent(Entity e) const { return IsValid(e) && m_registry.all_of<T>(e); }

    // =======================================================================
    // 3. 视图查询 (View Query)
    // =======================================================================

    /**
     * @brief 获取实体视图（核心查询接口）
     *
     * 用于遍历拥有所有指定 Components 的实体
     *
     * @tparam Components 组件类型列表
     * @return EnTT view 对象
     *
     * 使用示例：
     * @code
     * // 遍历所有同时拥有 Transform 和 Health 的实体
     * for (auto ent : reg.view<Transform, Health>()) {
     *     auto& transform = reg.GetComponent<Transform>(ent);
     *     auto& health = reg.GetComponent<Health>(ent);
     *     // 处理逻辑...
     * }
     * @endcode
     *
     * @note 返回的是 EnTT 的 basic_view，我们不做二次包装以保证性能
     *       如果未来需要更换 ECS 引擎，只需修改此文件
     */
    template <typename... Components> auto view() { return m_registry.view<Components...>(); }

    /// Const 版本
    template <typename... Components> auto view() const { return m_registry.view<Components...>(); }

    // =======================================================================
    // 4. 上下文管理 (Context / Singletons)
    // =======================================================================

    /**
     * @brief 设置/创建全局上下文对象
     * @tparam T 上下文类型
     * @param args 构造函数参数
     * @return 引用
     */
    template <typename T, typename... Args> T &SetContext(Args &&...args) {
        return m_registry.ctx().emplace<T>(std::forward<Args>(args)...);
    }

    /// 获取全局上下文（不存在则抛出异常）
    template <typename T> T &GetContext() {
        if (!m_registry.ctx().contains<T>()) {
            throw std::runtime_error("Context not found: " + std::string(typeid(T).name()));
        }
        return m_registry.ctx().get<T>();
    }

    /// Const 版本
    template <typename T> const T &GetContext() const {
        if (!m_registry.ctx().contains<T>()) {
            throw std::runtime_error("Context not found: " + std::string(typeid(T).name()));
        }
        return m_registry.ctx().get<T>();
    }

    /// 替换上下文（如果存在则替换，不存在则创建）
    template <typename T, typename... Args> T &CtxReplace(Args &&...args) {
        return m_registry.ctx().replace<T>(std::forward<Args>(args)...);
    }

    /// 查找上下文（不存在返回 nullptr）
    template <typename T> T *CtxFind() { return m_registry.ctx().contains<T>() ? &m_registry.ctx().get<T>() : nullptr; }

    /// Const 版本
    template <typename T> const T *CtxFind() const {
        return m_registry.ctx().contains<T>() ? &m_registry.ctx().get<T>() : nullptr;
    }

    /// 检查上下文是否存在
    template <typename T> bool HasContext() const { return m_registry.ctx().contains<T>(); }

    // =======================================================================
    // 5. 墓碑与清理
    // =======================================================================

    /// 清理已销毁实体的墓碑
    void SweepTombstones() { m_registry.compact(); }

private:
    void ValidateEntity(Entity e) const {
        if (!m_registry.valid(e)) {
            throw std::invalid_argument("Invalid Entity");
        }
    }

    entt::registry m_registry;
};

} // namespace ECS

} // namespace DX12Engine
