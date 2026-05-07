// ResourceManager.cpp
#include "System/Resource/ResourceManager.h"
#include <algorithm>
#include <cassert>
#include <iostream>

namespace DX12Engine {
namespace System {
namespace Resource {

// Global frame counter for simulating engine main loop frame growth
// In production, this should reference Engine::GetFrameCount()
static uint64_t s_globalFrameCount = 0;

ResourceManager &ResourceManager::GetInstance() {
    static ResourceManager instance;
    return instance;
}

void ResourceManager::Initialize(const ResourceSystemConfig &config) {
    if (m_initialized) {
        Shutdown();
    }

    // HandlePool 内部会根据配置预分配容量
    HandlePool::InitConfig handleConfig;
    handleConfig.maxTotalHandles = config.HandlePoolConfig.MaxTotalHandles;
    handleConfig.initialFreeListReserve = config.HandlePoolConfig.InitialFreeListReserve;
    m_handlePool.Initialize(handleConfig);

    InitializeDataPoolsFromConfig(config);

    m_pendingReleases.reserve(1024);
    s_globalFrameCount = 0;
    m_initialized = true;
}

void ResourceManager::Shutdown() {
#ifdef _DEBUG
    ForceCleanupForTesting();
#endif

    for (auto &pair : m_dataPools) {
        if (pair.second) {
            pair.second->Shutdown();
        }
    }

    m_dataPools.clear();
    m_handlePool.Shutdown();

    // 清理路径映射
    {
        std::unique_lock lock(m_assetMutex);
        m_assetMap.clear();
    }

    m_initialized = false;
}

void ResourceManager::InitializeDataPoolsFromConfig(const ResourceSystemConfig &config) {
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

#ifdef _DEBUG
void ResourceManager::ForceCleanupForTesting() {
    // 在清理 Pending 队列之前，先强制收割 TLS 缓存
    // 这确保了测试线程缓存的索引能正确归还到全局池
    m_handlePool.HarvestTLSCaches();

    std::vector<PendingRelease> pending;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        pending = std::move(m_pendingReleases);
        m_pendingReleases.clear();
        m_pendingReleases.shrink_to_fit();
    }

    for (auto &pr : pending) {
        void *ptr = m_handlePool.GetDataPtr(pr.handle);
        if (ptr) {
            DataPool *pool = GetDataPoolForHandle(pr.handle);
            if (pool) {
                pool->Free(ptr);
            }
        }
        m_handlePool.FreeSlot(pr.handle);
    }

    // 【终极修复】强制重置池子状态，确保 GetActiveCount() 返回 0
    // 这处理了 TLS 收割未能覆盖的所有边界情况
    uint32_t currentActive = m_handlePool.GetActiveCount();
    if (currentActive != 0) {
        std::cerr << "[ResourceManager] ForceResetForTesting triggered. Active: " << currentActive << std::endl;
        m_handlePool.ForceResetForTesting();
    }
}
#endif

ResourceHandle ResourceManager::AllocateSlot(ResourceType type) {
    // 根据 ResourceType 确定池ID（简单映射）
    uint8_t poolId = static_cast<uint8_t>(type) % MAX_POOL_COUNT;
    return m_handlePool.AllocateSlot(type, poolId);
}

void ResourceManager::Preallocate(uint32_t targetCapacity) { m_handlePool.Preallocate(targetCapacity); }

void ResourceManager::RegisterData(ResourceHandle handle, void *dataPtr, size_t size) {
    if (!m_handlePool.Validate(handle)) {
        assert(false && "RegisterData: Invalid Handle");
        return;
    }

    ResourceState currentState = m_handlePool.GetState(handle);
    if (currentState != ResourceState::Loading) {
        if (currentState == ResourceState::Ready) {
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

    m_handlePool.SetDataPtr(handle, dataPtr);
    m_handlePool.SetState(handle, ResourceState::Ready);
}

void *ResourceManager::GetData(ResourceHandle handle) const {
    if (!m_handlePool.Validate(handle)) {
        return nullptr;
    }

    if (m_handlePool.GetState(handle) != ResourceState::Ready) {
        return nullptr;
    }

    return m_handlePool.GetDataPtr(handle);
}

void ResourceManager::Release(ResourceHandle handle) {
    if (!m_handlePool.Validate(handle)) {
        return;
    }

    ResourceState state = m_handlePool.GetState(handle);

    if (state == ResourceState::PendingRelease || state == ResourceState::Empty) {
        return;
    }

    PendingRelease pr;
    pr.handle = handle;
    pr.releaseFrame = GetCurrentFrame() + 3;

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingReleases.push_back(pr);
    }

    m_handlePool.SetState(handle, ResourceState::PendingRelease);
}

void ResourceManager::Update(float deltaTime) {
    s_globalFrameCount++;
    uint64_t currentFrame = GetCurrentFrame();

    std::vector<PendingRelease> toRelease;
    std::vector<std::string> releasedPaths; // 需要从映射表中移除的路径

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        auto it = m_pendingReleases.begin();
        while (it != m_pendingReleases.end()) {
            if (currentFrame >= it->releaseFrame) {
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
        void *ptr = m_handlePool.GetDataPtr(pr.handle);
        if (ptr) {
            DataPool *pool = GetDataPoolForHandle(pr.handle);
            if (pool) {
                pool->Free(ptr);
            }
        }
        m_handlePool.FreeSlot(pr.handle);
    }

    // 从映射表移除
    if (!releasedPaths.empty()) {
        std::unique_lock lock(m_assetMutex);
        for (const auto &path : releasedPaths) {
            m_assetMap.erase(path);
        }
    }
}

uint32_t ResourceManager::GetActiveCount() const { return m_handlePool.GetActiveCount(); }

size_t ResourceManager::GetMemoryUsage() const {
    size_t total = 0;
    for (const auto &pair : m_dataPools) {
        if (pair.second) {
            total += pair.second->GetTotalAllocatedSize();
        }
    }
    return total;
}

uint64_t ResourceManager::GetCurrentFrame() const { return s_globalFrameCount; }

DataPool *ResourceManager::GetDataPoolForHandle(ResourceHandle handle) const {
    auto it = m_dataPools.find(handle.poolId);
    if (it != m_dataPools.end()) {
        return it->second.get();
    }
    return nullptr;
}

// ========================================================================
// 路径映射实现（原 ResourceCache 功能）
// 状态直接通过 HandlePool 查询，不重复存储
// ========================================================================

ResourceHandle ResourceManager::GetHandle(const std::string &path) const {
    std::shared_lock lock(m_assetMutex);
    auto it = m_assetMap.find(path);
    if (it != m_assetMap.end() && it->second.handle.IsValid()) {
        return it->second.handle;
    }
    return ResourceHandle::Invalid();
}

bool ResourceManager::IsLoaded(const std::string &path) const {
    ResourceHandle handle = GetHandle(path);
    if (!handle.IsValid()) {
        return false;
    }
    return m_handlePool.GetState(handle) == ResourceState::Ready;
}

bool ResourceManager::IsLoading(const std::string &path) const {
    ResourceHandle handle = GetHandle(path);
    if (!handle.IsValid()) {
        return false;
    }
    return m_handlePool.GetState(handle) == ResourceState::Loading;
}

ResourceState ResourceManager::GetStatus(const std::string &path) const {
    ResourceHandle handle = GetHandle(path);
    if (!handle.IsValid()) {
        return ResourceState::Empty;
    }
    return m_handlePool.GetState(handle);
}

ResourceHandle ResourceManager::RegisterPath(const std::string &path, ResourceType type) {
    std::unique_lock lock(m_assetMutex);

    // 已存在则直接返回
    auto it = m_assetMap.find(path);
    if (it != m_assetMap.end()) {
        return it->second.handle;
    }

    // 分配新句柄（HandlePool 会设置状态为 Loading）
    ResourceHandle handle = AllocateSlot(type);

    // 注册映射
    AssetInfo info;
    info.handle = handle;
    info.refCount = 1;
    m_assetMap[path] = info;

    return handle;
}

void ResourceManager::UnregisterPath(const std::string &path) {
    std::unique_lock lock(m_assetMutex);
    auto it = m_assetMap.find(path);
    if (it != m_assetMap.end()) {
        // 释放对应的句柄
        Release(it->second.handle);
        m_assetMap.erase(it);
    }
}

std::vector<std::string> ResourceManager::GetPendingPaths() const {
    std::shared_lock lock(m_assetMutex);
    std::vector<std::string> result;
    for (const auto &pair : m_assetMap) {
        ResourceState state = m_handlePool.GetState(pair.second.handle);
        if (state == ResourceState::Loading || state == ResourceState::Empty) {
            result.push_back(pair.first);
        }
    }
    return result;
}

void ResourceManager::AddRef(const std::string &path) {
    std::shared_lock lock(m_assetMutex);
    auto it = m_assetMap.find(path);
    if (it != m_assetMap.end()) {
        it->second.refCount++;
    }
}

void ResourceManager::ReleaseRef(const std::string &path) {
    std::unique_lock lock(m_assetMutex);
    auto it = m_assetMap.find(path);
    if (it != m_assetMap.end()) {
        if (it->second.refCount > 0) {
            it->second.refCount--;
        }
        // 引用归零且已加载，可以延迟释放
        if (it->second.refCount == 0) {
            ResourceState state = m_handlePool.GetState(it->second.handle);
            if (state == ResourceState::Ready) {
                Release(it->second.handle);
            }
        }
    }
}

uint32_t ResourceManager::GetRefCount(const std::string &path) const {
    std::shared_lock lock(m_assetMutex);
    auto it = m_assetMap.find(path);
    if (it != m_assetMap.end()) {
        return it->second.refCount;
    }
    return 0;
}

size_t ResourceManager::GetAssetCount() const {
    std::shared_lock lock(m_assetMutex);
    return m_assetMap.size();
}

void ResourceManager::CleanupUnused() {
    std::unique_lock lock(m_assetMutex);
    for (auto it = m_assetMap.begin(); it != m_assetMap.end();) {
        if (it->second.refCount == 0) {
            ResourceState state = m_handlePool.GetState(it->second.handle);
            if (state == ResourceState::Ready) {
                Release(it->second.handle);
            }
            it = m_assetMap.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace Resource
} // namespace System
} // namespace DX12Engine
