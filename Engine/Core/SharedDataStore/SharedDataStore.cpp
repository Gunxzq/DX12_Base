#include "SharedDataStore.h"
#include <algorithm>
#include <cassert>
#include <iostream>

using namespace DX12Engine::Boot;

namespace DX12Engine::Core {

SharedDataStore &SharedDataStore::GetInstance() {
    static SharedDataStore instance;
    return instance;
}

/**
 * @brief 初始化资产数据管理器
 * @param config
 * @date 2026-05-24
 */
void SharedDataStore::Initialize(const ResourceSystemConfig &config) {
    if (m_initialized) {
        Shutdown();
    }

    DataSlotPool::InitConfig handleConfig;
    handleConfig.maxTotalHandles = config.HandlePoolConfig.MaxTotalHandles;
    handleConfig.initialFreeListReserve = config.HandlePoolConfig.InitialFreeListReserve;

    m_slotPool.Initialize(handleConfig);

    // 初始化数据池
    InitializeDataPoolsFromConfig(config);

    // 预分配
    m_pendingReleases.reserve(1024);

    m_initialized = true;
}

/**
 * @brief 关闭资产数据管理器
 * @date 2026-05-24
 */
void SharedDataStore::Shutdown() {

    for (auto &pair : m_dataPools) {
        if (pair.second) {
            // 关闭数据池
            pair.second->Shutdown();
        }
    }

    m_dataPools.clear();
    m_slotPool.Shutdown();

    // 清理路径映射
    {
        std::unique_lock lock(m_assetMutex);
        m_assetMap.clear();
    }

    m_initialized = false;
}

/**
 * @brief 初始化数据池
 * @param config
 * @date 2026-05-24
 */
void SharedDataStore::InitializeDataPoolsFromConfig(const ResourceSystemConfig &config) {
    // 遍历配置中的每个数据池
    for (const auto &poolCfg : config.MemoryPools) {
        // 检查 Tag 是否有效 (0-15)
        if (poolCfg.HandleTag > 15) {
            assert(false && "Invalid HandleTag in config");
            continue;
        }

        // 创建一个新的 DataPool 实例
        auto pool = std::make_unique<DataPool>();

        // 设置池ID（用于TLS索引）
        pool->SetPoolID(poolCfg.HandleTag);

        // 计算大小并初始化
        size_t sizeBytes = 0;
        size_t blockSize = 0;

        if (poolCfg.Strategy == MemoryStrategy::Linear || poolCfg.Strategy == MemoryStrategy::RingBuffer) {
            // 线性或环形缓冲：总大小 = 大小MB * 1024 * 1024
            sizeBytes = poolCfg.SizeMB * 1024 * 1024;
        } else if (poolCfg.Strategy == MemoryStrategy::FixedSizeBlock) {
            // 固定块：总大小 = 块大小 * 数量
            blockSize = poolCfg.BlockSize;
            sizeBytes = blockSize * poolCfg.Count;
        }

        // 初始化 DataPool，传入策略参数
        pool->Initialize(poolCfg.Name, sizeBytes, poolCfg.Alignment, poolCfg.Strategy, blockSize);

        // 存入路由表
        m_dataPools[poolCfg.HandleTag] = std::move(pool);
    }
}

DataSlotHandle SharedDataStore::AllocateSlot(uint8_t poolId) { return m_slotPool.AllocateSlot(poolId); };

/**
 * @brief 注册资产数据
 * @param handle 资产句柄
 * @param dataPtr 数据指针
 * @param size 数据大小
 * @date 2026-05-24
 */
void SharedDataStore::RegisterData(DataSlotHandle handle, void *dataPtr, size_t size) {
    if (!m_slotPool.Validate(handle)) {
        assert(false && "RegisterData: Invalid Handle");
        return;
    }

    DataSlotState currentState = m_slotPool.GetState(handle);
    if (currentState != DataSlotState::Loading) {
        if (currentState == DataSlotState::Ready) {
            return;
        }
        assert(false && "RegisterData: Handle is not in Loading state");
        return;
    }

#ifdef _DEBUG
    if (dataPtr) {
        DataPool *pool = GetDataPoolForHandle(handle);
        if (pool) {
            bool inRange = pool->Contains(dataPtr);
            assert(inRange && "RegisterData: Pointer is not managed by the correct DataPool!");
        }
    }
#endif

    // 设置句柄数据指针
    m_slotPool.SetDataPtr(handle, dataPtr);

    // 设置句柄状态为已加载
    m_slotPool.SetState(handle, DataSlotState::Ready);
}

void *SharedDataStore::StoreData(DataSlotHandle handle, const void *dataPtr, size_t size) {
    if (!m_slotPool.Validate(handle)) {
        return nullptr;
    }

    DataPool *pool = GetDataPoolForHandle(handle);
    if (!pool) {
        return nullptr;
    }

    void *dst = pool->Allocate(size, 16);
    if (!dst) {
        return nullptr;
    }

    memcpy(dst, dataPtr, size);
    RegisterData(handle, dst, size);
    return dst;
}

/**
 * @brief 获取资产数据指针
 * @brief 获取资产数据指针
 * @param handle 资产句柄
 * @return void*
 * @date 2026-05-24
 */
void *SharedDataStore::GetData(DataSlotHandle handle) const {
    if (!m_slotPool.Validate(handle)) {
        return nullptr;
    }

    if (m_slotPool.GetState(handle) != DataSlotState::Ready) {
        return nullptr;
    }

    return m_slotPool.GetDataPtr(handle);
}

/**
 * @brief 设置资产资产数据为待释放
 * @param handle 资产句柄
 * @param pendingFence 待办Fence
 * @date 2026-05-24
 */
void SharedDataStore::ScheduleRelease(DataSlotHandle handle, uint64_t pendingFence) {
    if (!m_slotPool.Validate(handle)) {
        return;
    }

    DataSlotState state = m_slotPool.GetState(handle);

    if (state == DataSlotState::PendingRelease || state == DataSlotState::Empty) {
        return;
    }

    PendingRelease pr;
    pr.handle = handle;
    pr.pendingFence = pendingFence;

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingReleases.push_back(pr);
    }

    m_slotPool.SetState(handle, DataSlotState::PendingRelease);
}

/**
 * @brief 强制释放资产数据
 * @param handle 资产句柄
 * @date 2026-05-24
 */
void SharedDataStore::ForceRelease(DataSlotHandle handle) {
    if (!m_slotPool.Validate(handle))
        return;

    void *ptr = m_slotPool.GetDataPtr(handle);
    if (ptr) {
        DataPool *pool = GetDataPoolForHandle(handle);
        if (pool)
            pool->Free(ptr);
    }
    m_slotPool.FreeSlot(handle);
}

/**
 * @brief 回收资产数据
 * @param completedFence GPU已完成Fence
 * @date 2026-05-24
 */
void SharedDataStore::Reclaim(uint64_t completedFence) {

    std::vector<PendingRelease> toRelease;
    std::vector<std::string> releasedPaths; // 需要从映射表中移除的路径

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        auto it = m_pendingReleases.begin();
        while (it != m_pendingReleases.end()) {
            if (completedFence >= it->pendingFence) {
                toRelease.push_back(*it);
                it = m_pendingReleases.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 收集待释放的路径
    {
        std::shared_lock lock(m_assetMutex);
        for (const auto &pr : toRelease) {
            for (const auto &pair : m_assetMap) {
                if (pair.second.handle == pr.handle) {
                    releasedPaths.push_back(pair.first);
                    break;
                }
            }
        }
    }

    // 执行释放
    for (auto &pr : toRelease) {
        void *ptr = m_slotPool.GetDataPtr(pr.handle);
        if (ptr) {
            DataPool *pool = GetDataPoolForHandle(pr.handle);
            if (pool) {
                pool->Free(ptr);
            }
        }
        m_slotPool.FreeSlot(pr.handle);
    }

    // 从映射表移除
    if (!releasedPaths.empty()) {
        std::unique_lock lock(m_assetMutex);
        for (const auto &path : releasedPaths) {
            m_assetMap.erase(path);
        }
    }
}

/**
 * @brief 获取资产数据内存占用
 * @return size_t
 * @date 2026-05-24
 */
size_t SharedDataStore::GetMemoryUsage() const {
    size_t total = 0;
    for (const auto &pair : m_dataPools) {
        if (pair.second) {
            total += pair.second->GetTotalAllocatedSize();
        }
    }
    return total;
}

/**
 * @brief 获取资产数据池
 * @param handle 资产句柄
 * @return DataPool*
 * @date 2026-05-24
 */
DataPool *SharedDataStore::GetDataPoolForHandle(DataSlotHandle handle) const {
    auto it = m_dataPools.find(handle.poolId);
    if (it != m_dataPools.end()) {
        return it->second.get();
    }
    return nullptr;
}

/**
 * @brief 获取资产句柄
 * @param path 资产路径
 * @return DataSlotHandle
 * @date 2026-05-24
 */
DataSlotHandle SharedDataStore::GetHandle(const std::string &path) const {
    std::shared_lock lock(m_assetMutex);
    auto it = m_assetMap.find(path);
    if (it != m_assetMap.end() && it->second.handle.IsValid()) {
        return it->second.handle;
    }
    return DataSlotHandle::Invalid();
}

/**
 * @brief 检查资产数据是否已加载
 * @param path 资产路径
 * @return bool
 * @date 2026-05-24
 */
bool SharedDataStore::IsLoaded(const std::string &path) const {
    DataSlotHandle handle = GetHandle(path);
    if (!handle.IsValid()) {
        return false;
    }
    return m_slotPool.GetState(handle) == DataSlotState::Ready;
}

/**
 * @brief 检查资产数据是否正在加载
 * @param path 资产路径
 * @return bool
 * @date 2026-05-24
 */
bool SharedDataStore::IsLoading(const std::string &path) const {
    DataSlotHandle handle = GetHandle(path);
    if (!handle.IsValid()) {
        return false;
    }
    return m_slotPool.GetState(handle) == DataSlotState::Loading;
}

/**
 * @brief 获取资产数据状态
 * @param path 资产路径
 * @return DataSlotState
 * @date 2026-05-24
 */
DataSlotState SharedDataStore::GetStatus(const std::string &path) const {
    DataSlotHandle handle = GetHandle(path);
    if (!handle.IsValid()) {
        return DataSlotState::Empty;
    }
    return m_slotPool.GetState(handle);
}

/**
 * @brief 注册资产路径
 * @param path 资产路径
 * @param type 资产类型
 * @param poolId 数据池ID
 * @return DataSlotHandle
 * @date 2026-05-24
 */
DataSlotHandle SharedDataStore::RegisterPath(const std::string &path, uint8_t poolId) {
    std::unique_lock lock(m_assetMutex);

    // 已存在则直接返回
    auto it = m_assetMap.find(path);
    if (it != m_assetMap.end()) {
        return it->second.handle;
    }

    // 分配新句柄（HandlePool 会设置状态为 Loading）
    DataSlotHandle handle = m_slotPool.AllocateSlot(poolId);

    // 注册映射
    AssetInfo info;
    info.handle = handle;
    info.refCount = 1;
    m_assetMap[path] = info;

    return handle;
}

/**
 * @brief 注销资产路径
 * @param path 资产路径
 * @date 2026-05-24
 */
void SharedDataStore::UnregisterPath(const std::string &path) {
    std::unique_lock lock(m_assetMutex);
    auto it = m_assetMap.find(path);
    if (it != m_assetMap.end()) {
        // 释放对应的句柄
        ForceRelease(it->second.handle);
        m_assetMap.erase(it);
    }
}

/**
 * @brief 获取待加载资产路径
 * @return std::vector<std::string>
 * @date 2026-05-24
 */
std::vector<std::string> SharedDataStore::GetPendingPaths() const {
    std::shared_lock lock(m_assetMutex);
    std::vector<std::string> result;
    for (const auto &pair : m_assetMap) {
        DataSlotState state = m_slotPool.GetState(pair.second.handle);
        if (state == DataSlotState::Loading || state == DataSlotState::Empty) {
            result.push_back(pair.first);
        }
    }
    return result;
}

/**
 * @brief 增加资产路径引用计数
 * @param path 资产路径
 * @date 2026-05-24
 */
void SharedDataStore::AddRef(const std::string &path) {
    std::shared_lock lock(m_assetMutex);
    auto it = m_assetMap.find(path);
    if (it != m_assetMap.end()) {
        it->second.refCount++;
    }
}

/**
 * @brief 释放资产路径引用计数
 * @param path 资产路径
 * @date 2026-05-24
 */
void SharedDataStore::ReleaseRef(const std::string &path) {
    std::unique_lock lock(m_assetMutex);
    auto it = m_assetMap.find(path);
    if (it != m_assetMap.end()) {
        if (it->second.refCount > 0) {
            it->second.refCount--;
        }
        // 引用归零且已加载，可以延迟释放
        if (it->second.refCount == 0) {
            DataSlotState state = m_slotPool.GetState(it->second.handle);
            if (state == DataSlotState::Ready) {
                ForceRelease(it->second.handle);
            }
        }
    }
}

/**
 * @brief 获取资产路径引用计数
 * @param path 资产路径
 * @return uint32_t
 * @date 2026-05-24
 */
uint32_t SharedDataStore::GetRefCount(const std::string &path) const {
    std::shared_lock lock(m_assetMutex);
    auto it = m_assetMap.find(path);
    if (it != m_assetMap.end()) {
        return it->second.refCount;
    }
    return 0;
}

/**
 * @brief 获取已加载资产数据数量
 * @return size_t
 * @date 2026-05-24
 */
size_t SharedDataStore::GetAssetCount() const {
    std::shared_lock lock(m_assetMutex);
    return m_assetMap.size();
}

/**
 * @brief 清理未使用的资产数据
 * @date 2026-05-24
 */
void SharedDataStore::CleanupUnused() {
    std::unique_lock lock(m_assetMutex);
    for (auto it = m_assetMap.begin(); it != m_assetMap.end();) {
        if (it->second.refCount == 0) {
            DataSlotState state = m_slotPool.GetState(it->second.handle);
            if (state == DataSlotState::Ready) {
                ForceRelease(it->second.handle);
            }
            it = m_assetMap.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace DX12Engine::Core
