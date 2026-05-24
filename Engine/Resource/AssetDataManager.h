#pragma once
#include "Boot/ResourceConfig.h"
#include "Core/CpuHandlePool.h"
#include "Core/DataPool.h"
#include "Core/DataPoolContext.h"
#include "Struct/ResourceHandle.h"
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace DX12Engine {

namespace Boot {
struct ResourceSystemConfig;
}

namespace Resource {

// ========================================================================

/**
 * @brief 资产管理器
 *
 * 核心职责：
 * 1. 句柄管理 (CpuHandlePool) - TLS 优化、Generation 防错
 * 2. 内存管理 (DataPool) - Linear/RingBuffer/FixedSizeBlock
 * 3. 延迟释放 - 3 帧延迟回收
 * 4. 路径映射 - Path -> Handle 的映射表（原 ResourceCache 功能）
 *
 * 状态管理：委托给 HandlePool，使用 ResourceState 枚举
 */
class AssetDataManager {
public:
    static AssetDataManager &GetInstance();

    void Initialize(const Boot::ResourceSystemConfig &config);
    void Shutdown();

    // --- 被动调用接口 (由 TaskBucket 调用) ---

    /**
     * @brief 预分配句柄容量
     * @note 用于测试或已知需要大量句柄的场景
     */
    void Preallocate(uint32_t targetCapacity);

    /**
     * @brief 分配一个资源槽位
     * @note 由 LoadingBucket 在开始加载前调用。返回的 Handle 状态为 Loading。
     */
    CpuResourceHandle AllocateSlot(CpuResourceType type);

    /**
     * @brief 注册已加载的数据
     * @note 由 LoadingBucket (IO线程完成后) 调用。将数据指针绑定到 Handle，状态转为 Ready。
     * @param handle 之前分配的句柄
     * @param dataPtr 指向 DataPool 中数据的指针
     * @param size 数据大小
     */
    void RegisterData(CpuResourceHandle handle, void *dataPtr, size_t size);

    /**
     * @brief 获取资源数据指针
     * @note 由 BindingBucket 或渲染线程调用。如果资源未就绪或无效，返回 nullptr。
     */
    void *GetData(CpuResourceHandle handle) const;

    /**
     * @brief 请求释放资源
     * @note 由 BindingBucket 或逻辑层调用。不立即释放，进入延迟回收队列。
     */
    void Release(CpuResourceHandle handle);

    /**
     * @brief 每帧更新
     * @note 由主线程在帧末调用。处理延迟回收队列，真正释放内存。
     */
    void Update(float deltaTime);

    // ------------------------------------------------------------------
    // 路径映射（新增：原 ResourceCache 功能）
    // ------------------------------------------------------------------

    /**
     * @brief 通过路径获取资源句柄
     * @return 有效的句柄，或 Invalid() 如果不存在
     */
    CpuResourceHandle GetHandle(const std::string &path) const;

    /**
     * @brief 检查资源是否已加载
     */
    bool IsLoaded(const std::string &path) const;

    /**
     * @brief 检查资源是否正在加载
     */
    bool IsLoading(const std::string &path) const;

    /**
     * @brief 获取资源加载状态
     */
    CpuResourceState GetStatus(const std::string &path) const;

    /**
     * @brief 注册路径映射（由异步加载系统调用）
     * @note 加载开始时调用，创建 Handle 并映射路径
     */
    CpuResourceHandle RegisterPath(const std::string &path, CpuResourceType type);

    /**
     * @brief 取消路径映射（加载失败时调用）
     */
    void UnregisterPath(const std::string &path);

    /**
     * @brief 获取所有待加载的路径
     */
    std::vector<std::string> GetPendingPaths() const;

    // ------------------------------------------------------------------
    // 引用计数
    // ------------------------------------------------------------------

    void AddRef(const std::string &path);
    void ReleaseRef(const std::string &path);
    uint32_t GetRefCount(const std::string &path) const;

    // ------------------------------------------------------------------
    // 调试/监控
    // ------------------------------------------------------------------

    uint32_t GetActiveCount() const;
    size_t GetMemoryUsage() const;
    size_t GetAssetCount() const;
    void CleanupUnused();

private:
    AssetDataManager() = default;
    ~AssetDataManager() = default;

    // 禁止拷贝
    AssetDataManager(const AssetDataManager &) = delete;
    AssetDataManager &operator=(const AssetDataManager &) = delete;

    // 初始化状态
    bool m_initialized = false;

    // 句柄池
    CpuHandlePool m_handlePool;

    // 数据池
    std::map<uint8_t, std::unique_ptr<DataPool>> m_dataPools;

    // 延迟回收
    struct PendingRelease {
        CpuResourceHandle handle;
        uint64_t releaseFrame;
    };
    std::vector<PendingRelease> m_pendingReleases;
    mutable std::mutex m_pendingMutex;

    // 路径映射表（原 ResourceCache 核心功能）
    // 注意：状态信息直接通过 HandlePool 查询，这里只存储元数据
    struct AssetInfo {
        CpuResourceHandle handle;
        uint32_t refCount = 0;
    };
    mutable std::unordered_map<std::string, AssetInfo> m_assetMap;
    mutable std::shared_mutex m_assetMutex;

    uint64_t GetCurrentFrame() const;
    Boot::ResourceSystemConfig m_config;
    void InitializeDataPoolsFromConfig(const Boot::ResourceSystemConfig &config);
    DataPool *GetDataPoolForHandle(CpuResourceHandle handle) const;
};

} // namespace Resource

} // namespace DX12Engine
