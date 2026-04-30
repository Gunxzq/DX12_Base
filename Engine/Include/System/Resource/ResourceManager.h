// ResourceManager.h
#pragma once
#include "System/Resource/Core/DataPool.h"
#include "System/Resource/Core/HandlePool.h"
#include <cstdint>
#include <mutex>
#include <vector>

namespace DX12Engine {
namespace System {
namespace Resource {

class ResourceManager {
public:
    static ResourceManager &GetInstance();

    void Initialize();
    void Shutdown();

    /**
     * @brief 强制清理所有延迟释放的资源（测试专用）
     * @note 绕过帧延迟机制，立即处理 m_pendingReleases 中所有条目。
     *       仅用于测试环境，生产代码不应调用此方法。
     */
    void ForceCleanupForTesting();

    // --- 被动调用接口 (由 TaskBucket 调用) ---

    /**
     * @brief 预分配句柄容量
     * @note 用于测试或已知需要大量句柄的场景，避免运行时循环扩容
     */
    void Preallocate(uint32_t targetCapacity);

    /**
     * @brief 分配一个资源槽位
     * @note 由 LoadingBucket 在开始加载前调用。返回的 Handle 状态为 Loading。
     */
    ResourceHandle AllocateSlot(ResourceType type);

    /**
     * @brief 注册已加载的数据
     * @note 由 LoadingBucket (IO线程完成后) 调用。将数据指针绑定到 Handle，状态转为 Ready。
     * @param handle 之前分配的句柄
     * @param dataPtr 指向 DataPool 中数据的指针
     * @param size 数据大小
     */
    void RegisterData(ResourceHandle handle, void *dataPtr, size_t size);

    /**
     * @brief 获取资源数据指针
     * @note 由 BindingBucket 或渲染线程调用。如果资源未就绪或无效，返回 nullptr。
     */
    void *GetData(ResourceHandle handle) const;

    /**
     * @brief 请求释放资源
     * @note 由 BindingBucket 或逻辑层调用。不立即释放，进入延迟回收队列。
     */
    void Release(ResourceHandle handle);

    /**
     * @brief 每帧更新
     * @note 由主线程在帧末调用。处理延迟回收队列，真正释放内存。
     */
    void Update(float deltaTime);

    // --- 调试/监控接口 ---
    uint32_t GetActiveCount() const;
    size_t GetMemoryUsage() const;

    // --- 测试专用接口 ---
    DataPool &GetDataPool() { return m_dataPool; }
    const DataPool &GetDataPool() const { return m_dataPool; }

private:
    ResourceManager() = default;
    ~ResourceManager() = default;

    // 禁止拷贝
    ResourceManager(const ResourceManager &) = delete;
    ResourceManager &operator=(const ResourceManager &) = delete;

    // 初始化状态标志（用于防止重复初始化导致 TLS 状态残留）
    bool m_initialized = false;

    HandlePool m_handlePool;
    DataPool m_dataPool;

    // 延迟回收机制
    struct PendingRelease {
        ResourceHandle handle;
        uint64_t releaseFrame; // 使用帧计数器而非时间，更精确
    };

    std::vector<PendingRelease> m_pendingReleases;
    mutable std::mutex m_pendingMutex; // 保护 m_pendingReleases 的并发访问

    // 全局帧计数器引用 (假设引擎有全局帧计数)
    uint64_t GetCurrentFrame() const;
};

} // namespace Resource
} // namespace System
} // namespace DX12Engine