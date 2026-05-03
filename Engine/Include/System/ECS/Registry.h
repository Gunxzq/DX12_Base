#pragma once
#include "Entity.h"
#include <entt/entt.hpp>
#include <stdexcept>
#include <vector>
#include <type_traits>

namespace DX12::ECS {

// ========================================================================
// 缓冲策略标记 (Buffer Strategy Tags)
// ========================================================================

/// 单缓冲策略（默认）：直接存储，无额外开销
struct single_buffer_t {};
inline constexpr single_buffer_t single_buffer{};

/// 双缓冲策略：适合 CPU 写 / GPU 读的场景（如 Transform）
struct double_buffer_t {};
inline constexpr double_buffer_t double_buffer{};

/// 三缓冲策略：适合高并发读写，避免管线停顿
struct triple_buffer_t {};
inline constexpr triple_buffer_t triple_buffer{};

/// N 缓冲策略：通用多缓冲，N 在编译期确定
template<size_t N>
struct n_buffer_t {};

template<size_t N>
inline constexpr n_buffer_t<N> n_buffer{};

// ========================================================================
// 组件特性模板 (Component Traits Hook)
// ========================================================================

/**
 * @brief 组件特性模板 - L4 层扩展钩子
 *
 * L4 应用层通过特化此模板来声明组件的缓冲策略。
 * 默认情况下所有组件使用单缓冲（零开销）。
 *
 * 使用示例（在 L4 层定义组件时）：
 * @code
 * // 1. 定义组件数据（纯 POD）
 * struct RenderTransform {
 *     alignas(16) float matrix[16];
 * };
 *
 * // 2. 特化组件特性，声明双缓冲策略
 * namespace DX12::ECS {
 *     template<>
 *     struct component_traits<RenderTransform> {
 *         using buffer_policy = double_buffer_t;
 *     };
 * }
 * @endcode
 */
template<typename T>
struct component_traits {
    using buffer_policy = single_buffer_t;
};

// ========================================================================
// 缓冲策略检测工具
// ========================================================================

namespace detail {
    template<typename T>
    using buffer_policy_t = typename component_traits<T>::buffer_policy;

    template<typename T>
    inline constexpr bool is_double_buffered_v =
        std::is_same_v<buffer_policy_t<T>, double_buffer_t>;

    template<typename T>
    inline constexpr bool is_triple_buffered_v =
        std::is_same_v<buffer_policy_t<T>, triple_buffer_t>;

    template<typename T, size_t N>
    inline constexpr bool is_n_buffered_v =
        std::is_same_v<buffer_policy_t<T>, n_buffer_t<N>>;

    template<typename T>
    inline constexpr bool is_multi_buffered_v =
        is_double_buffered_v<T> || is_triple_buffered_v<T> ||
        []<size_t... Is>(std::index_sequence<Is...>) {
            return (is_n_buffered_v<T, Is + 2> || ...);
        }(std::make_index_sequence<6>{}); // 支持 2-8 缓冲
}

// ========================================================================
// 多缓冲存储包装器 (Multi-Buffer Storage Wrapper)
// ========================================================================

/**
 * @brief 双缓冲存储实现
 * @tparam T 组件类型
 */
template<typename T>
class DoubleBufferStorage {
public:
    static_assert(std::is_copy_assignable_v<T>, "Double-buffered component must be copyable");

    explicit DoubleBufferStorage(size_t capacity = 1024) {
        m_front.reserve(capacity);
        m_back.reserve(capacity);
    }

    /// 获取当前写入缓冲区（Back）的引用
    T& get_for_write(size_t index) { return m_back[index]; }
    const T& get_for_write(size_t index) const { return m_back[index]; }

    /// 获取当前读取缓冲区（Front）的引用
    T& get_for_read(size_t index) { return m_front[index]; }
    const T& get_for_read(size_t index) const { return m_front[index]; }

    /// 添加元素到写入缓冲区
    template<typename... Args>
    T& emplace_back(Args&&... args) {
        m_back.emplace_back(std::forward<Args>(args)...);
        // 同步扩展 front 以保持索引对齐
        if (m_front.size() < m_back.size()) {
            m_front.emplace_back(m_back.back());
        }
        return m_back.back();
    }

    /// 交换前后缓冲区（O(1) 指针交换）
    void swap() {
        m_front.swap(m_back);
    }

    /// 将写入缓冲区的数据复制到读取缓冲区（用于部分更新场景）
    void sync() {
        m_front = m_back;
    }

    size_t size() const { return m_back.size(); }
    void clear() { m_front.clear(); m_back.clear(); }
    void reserve(size_t n) { m_front.reserve(n); m_back.reserve(n); }

private:
    std::vector<T> m_front;  ///< 读取缓冲区（Render Thread 读）
    std::vector<T> m_back;   ///< 写入缓冲区（Logic Thread 写）
};

/**
 * @brief 三缓冲存储实现
 * @tparam T 组件类型
 */
template<typename T>
class TripleBufferStorage {
public:
    static_assert(std::is_copy_assignable_v<T>, "Triple-buffered component must be copyable");

    explicit TripleBufferStorage(size_t capacity = 1024) {
        for (auto& buf : m_buffers) buf.reserve(capacity);
    }

    /// 获取当前写入缓冲区的引用
    T& get_for_write(size_t index) { return m_buffers[m_write_idx][index]; }

    /// 获取当前读取缓冲区的引用
    T& get_for_read(size_t index) { return m_buffers[m_read_idx][index]; }
    const T& get_for_read(size_t index) const { return m_buffers[m_read_idx][index]; }

    template<typename... Args>
    T& emplace_back(Args&&... args) {
        auto& result = m_buffers[m_write_idx].emplace_back(std::forward<Args>(args)...);
        // 同步扩展其他缓冲区
        for (size_t i = 0; i < 3; ++i) {
            if (i != m_write_idx && m_buffers[i].size() < m_buffers[m_write_idx].size()) {
                m_buffers[i].emplace_back(result);
            }
        }
        return result;
    }

    /// 旋转缓冲区索引 (write->read, read->pending, pending->write)
    void rotate() {
        // 新读取索引 = 旧写入索引
        // 新写入索引 = 第三个索引
        size_t new_read = m_write_idx;
        size_t new_write = 3 - m_read_idx - m_write_idx;
        m_read_idx = new_read;
        m_write_idx = new_write;
    }

    size_t size() const { return m_buffers[m_write_idx].size(); }
    void clear() { for (auto& buf : m_buffers) buf.clear(); }

private:
    std::vector<T> m_buffers[3];
    size_t m_read_idx = 0;   ///< 当前读取缓冲区索引
    size_t m_write_idx = 1;  ///< 当前写入缓冲区索引
};

/**
 * @brief ECS 注册表封装 (Facade Pattern)
 *
 * L2 数据层核心：屏蔽 EnTT 底层，提供简洁接口
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
 *
 * @note 此类是线程不安全的，多线程访问需要外部同步
 */
class Registry {
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
    template <typename T, typename... Args> T &AddComponent(Entity e, Args &&...args) {
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

    // ========================================================================
    // 6. 多缓冲管理 (Multi-Buffer Management) - L4 钩子支持
    // ========================================================================

    /**
     * @brief 交换所有双缓冲组件的前后缓冲区
     *
     * 调用时机：帧结束时（Frame End），由 L3 调度层触发
     * 复杂度：O(N)，N 为双缓冲组件类型数量（编译期确定）
     */
    void SwapDoubleBuffers() {
        // 实际实现需要配合自定义存储，这里提供接口框架
        // L4 层通过特化 component_traits 来标记哪些组件需要双缓冲
        OnSwapDoubleBuffers();
    }

    /**
     * @brief 旋转所有三缓冲组件的缓冲区
     *
     * 调用时机：帧结束时（Frame End），由 L3 调度层触发
     */
    void RotateTripleBuffers() {
        OnRotateTripleBuffers();
    }

    /**
     * @brief 同步所有多缓冲组件（将写入缓冲区的数据复制到读取缓冲区）
     *
     * 用于非交换模式下的显式同步
     */
    void SyncMultiBuffers() {
        OnSyncMultiBuffers();
    }

    /**
     * @brief 获取组件的读取缓冲区视图（用于渲染线程）
     * @tparam T 组件类型（必须是多缓冲组件）
     * @return 读取缓冲区的引用
     *
     * 使用示例：
     * @code
     * // L4 层在渲染线程中读取 Transform
     * auto& read_transforms = registry.GetReadBuffer<RenderTransform>();
     * for (auto& tf : read_transforms) {
     *     // 提交到 GPU...
     * }
     * @endcode
     */
    template<typename T>
    auto* GetReadBuffer() {
        static_assert(detail::is_multi_buffered_v<T>,
                      "GetReadBuffer() only available for multi-buffered components");
        return OnGetReadBuffer<T>();
    }

    template<typename T>
    const auto* GetReadBuffer() const {
        static_assert(detail::is_multi_buffered_v<T>,
                      "GetReadBuffer() only available for multi-buffered components");
        return OnGetReadBuffer<T>();
    }

    /**
     * @brief 获取组件的写入缓冲区视图（用于逻辑线程）
     * @tparam T 组件类型（必须是多缓冲组件）
     * @return 写入缓冲区的引用
     */
    template<typename T>
    auto* GetWriteBuffer() {
        static_assert(detail::is_multi_buffered_v<T>,
                      "GetWriteBuffer() only available for multi-buffered components");
        return OnGetWriteBuffer<T>();
    }

private:
    void ValidateEntity(Entity e) const {
        if (!m_registry.valid(e)) {
            throw std::invalid_argument("Invalid Entity");
        }
    }

    // 多缓冲事件钩子（可由派生类或混入类实现）
    virtual void OnSwapDoubleBuffers() {}
    virtual void OnRotateTripleBuffers() {}
    virtual void OnSyncMultiBuffers() {}

    template<typename T>
    void* OnGetReadBuffer() { return nullptr; }

    template<typename T>
    void* OnGetWriteBuffer() { return nullptr; }

    entt::registry m_registry;
};

} // namespace DX12::ECS

// ========================================================================
// 便捷宏定义 (L4 层使用)
// ========================================================================

/**
 * @brief 声明组件使用双缓冲策略
 * @param ComponentType 组件类型名
 *
 * 使用位置：L4 层组件定义的头文件中，全局命名空间
 *
 * 示例：
 * @code
 * // RenderTransform.h
 * #pragma once
 * #include "ECS/Registry.h"
 *
 * struct RenderTransform {
 *     alignas(16) float matrix[16];
 * };
 *
 * DECLARE_DOUBLE_BUFFERED(RenderTransform);
 * @endcode
 */
#define DECLARE_DOUBLE_BUFFERED(ComponentType)                      \
    namespace DX12::ECS {                                           \
        template<>                                                  \
        struct component_traits<ComponentType> {                    \
            using buffer_policy = DX12::ECS::double_buffer_t;       \
        };                                                          \
    }

/**
 * @brief 声明组件使用三缓冲策略
 * @param ComponentType 组件类型名
 */
#define DECLARE_TRIPLE_BUFFERED(ComponentType)                      \
    namespace DX12::ECS {                                           \
        template<>                                                  \
        struct component_traits<ComponentType> {                    \
            using buffer_policy = DX12::ECS::triple_buffer_t;       \
        };                                                          \
    }

/**
 * @brief 声明组件使用 N 缓冲策略
 * @param ComponentType 组件类型名
 * @param N 缓冲区数量 (2-8)
 */
#define DECLARE_N_BUFFERED(ComponentType, N)                        \
    namespace DX12::ECS {                                           \
        template<>                                                  \
        struct component_traits<ComponentType> {                    \
            using buffer_policy = DX12::ECS::n_buffer_t<N>;         \
        };                                                          \
    }

/**
 * @brief 声明组件使用单缓冲策略（显式声明，可选）
 * @param ComponentType 组件类型名
 */
#define DECLARE_SINGLE_BUFFERED(ComponentType)                      \
    namespace DX12::ECS {                                           \
        template<>                                                  \
        struct component_traits<ComponentType> {                    \
            using buffer_policy = DX12::ECS::single_buffer_t;       \
        };                                                          \
    }
