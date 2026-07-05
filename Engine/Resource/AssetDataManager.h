#pragma once
#include "Core/Config/ConfigTypes/ResourceConfig.h"
#include "Core/CpuHandlePool.h"
#include "Core/DataPool.h"
#include "Core/DataPoolContext.h"
#include "Struct/ResourceHandle.h"
#include <any>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace DX12Engine {

namespace Boot {
struct ResourceSystemConfig;
}

namespace Resource {

class AssetDataManager {
public:
    static AssetDataManager &GetInstance();

    void Initialize(const Boot::ResourceSystemConfig &config);
    void Shutdown();
    void Preallocate(uint32_t targetCapacity) { m_handlePool.Preallocate(targetCapacity); };

    // ------------------------------------------------------------------
    // 资产数据管理
    // ------------------------------------------------------------------
    CpuResourceHandle AllocateSlot(CpuResourceType type, uint8_t poolId);
    void RegisterData(CpuResourceHandle handle, void *dataPtr, size_t size);
    void ScheduleRelease(CpuResourceHandle handle, uint64_t pendingFence);
    void Reclaim(uint64_t pendingFence);
    // ------------------------------------------------------------------
    // 资产数据访问
    // ------------------------------------------------------------------
    CpuResourceHandle GetHandle(const std::string &path) const;
    CpuResourceState GetStatus(const std::string &path) const;
    bool IsLoaded(const std::string &path) const;
    bool IsLoading(const std::string &path) const;
    void *GetData(CpuResourceHandle handle) const;

    // ------------------------------------------------------------------
    // 资产路径管理
    // ------------------------------------------------------------------

    CpuResourceHandle RegisterPath(const std::string &path, CpuResourceType type, uint8_t poolId);
    void UnregisterPath(const std::string &path);
    std::vector<std::string> GetPendingPaths() const;

    // ------------------------------------------------------------------
    // 引用计数
    // ------------------------------------------------------------------

    void AddRef(const std::string &path);
    void ReleaseRef(const std::string &path);
    uint32_t GetRefCount(const std::string &path) const;

    // ------------------------------------------------------------------
    // 事件系统数据传递（线程安全，用于跨 System/lambda 传递大对象）
    // ------------------------------------------------------------------

    template <typename T> void StoreTypedData(const std::string &key, std::shared_ptr<T> data) {
        std::unique_lock lock(m_assetMutex);
        m_typedDataStore[key] = std::move(data);
    }

    template <typename T> std::shared_ptr<T> GetTypedData(const std::string &key) const {
        std::shared_lock lock(m_assetMutex);
        auto it = m_typedDataStore.find(key);
        if (it != m_typedDataStore.end()) {
            return std::any_cast<std::shared_ptr<T>>(it->second);
        }
        return nullptr;
    }

    void RemoveTypedData(const std::string &key) {
        std::unique_lock lock(m_assetMutex);
        m_typedDataStore.erase(key);
    }

    // ------------------------------------------------------------------
    // 调试/监控
    // ------------------------------------------------------------------

    uint32_t GetActiveCount() const { return m_handlePool.GetActiveCount(); };
    size_t GetMemoryUsage() const;
    size_t GetAssetCount() const;
    void CleanupUnused();

private:
    struct AssetInfo {
        CpuResourceHandle handle;
        uint32_t refCount = 0;
    };

    struct PendingRelease {
        CpuResourceHandle handle;
        uint64_t pendingFence = 0;
    };

private:
    AssetDataManager() = default;
    ~AssetDataManager() = default;

    // 禁止拷贝
    AssetDataManager(const AssetDataManager &) = delete;
    AssetDataManager &operator=(const AssetDataManager &) = delete;

    // 初始化状态
    bool m_initialized = false;

    CpuHandlePool m_handlePool;
    std::map<uint8_t, std::unique_ptr<DataPool>> m_dataPools;

    std::vector<PendingRelease> m_pendingReleases;
    mutable std::unordered_map<std::string, AssetInfo> m_assetMap; // 资产数据映射表
    mutable std::unordered_map<std::string, std::any> m_typedDataStore; // 事件系统数据存储
    mutable std::shared_mutex m_assetMutex;
    mutable std::mutex m_pendingMutex;

    Boot::ResourceSystemConfig m_config;

    void InitializeDataPoolsFromConfig(const Boot::ResourceSystemConfig &config);

    DataPool *GetDataPoolForHandle(CpuResourceHandle handle) const;

    void ForceRelease(CpuResourceHandle handle);
};

} // namespace Resource

} // namespace DX12Engine
