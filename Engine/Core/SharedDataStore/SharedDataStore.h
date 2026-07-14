// SharedDataStore.h — 事件系统的多线程安全数据中转站
#pragma once
#include "Core/Config/ConfigTypes/ResourceConfig.h"
#include "Core/SharedDataStore/DataPool.h"
#include "Core/SharedDataStore/DataPoolContext.h"
#include "Core/SharedDataStore/DataSlotPool.h"
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

namespace Core {

// ============================================================================
// SharedDataStore — 线程安全的多线程数据中转站
// ============================================================================

class SharedDataStore {
public:
    static SharedDataStore &GetInstance();

    void Initialize(const Boot::ResourceSystemConfig &config);
    void Shutdown();
    void Preallocate(uint32_t targetCapacity) { m_slotPool.Preallocate(targetCapacity); };

    // ------------------------------------------------------------------
    // 槽位管理
    // ------------------------------------------------------------------
    DataSlotHandle AllocateSlot(uint8_t poolId);
    void RegisterData(DataSlotHandle handle, void *dataPtr, size_t size);

    /**
     * @brief 从 DataPool 分配内存后注册数据（分配+拷贝+注册一步完成）
     */
    void *StoreData(DataSlotHandle handle, const void *dataPtr, size_t size);
    void ScheduleRelease(DataSlotHandle handle, uint64_t pendingFence);
    void Reclaim(uint64_t pendingFence);

    // ------------------------------------------------------------------
    // 数据访问
    // ------------------------------------------------------------------
    DataSlotHandle GetHandle(const std::string &path) const;
    DataSlotState GetStatus(const std::string &path) const;
    bool IsLoaded(const std::string &path) const;
    bool IsLoading(const std::string &path) const;
    void *GetData(DataSlotHandle handle) const;

    // ------------------------------------------------------------------
    // 路径管理
    // ------------------------------------------------------------------
    DataSlotHandle RegisterPath(const std::string &path, uint8_t poolId);
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
    uint32_t GetActiveCount() const { return m_slotPool.GetActiveCount(); };
    size_t GetMemoryUsage() const;
    size_t GetAssetCount() const;
    void CleanupUnused();

private:
    struct AssetInfo {
        DataSlotHandle handle;
        uint32_t refCount = 0;
    };

    struct PendingRelease {
        DataSlotHandle handle;
        uint64_t pendingFence = 0;
    };

private:
    SharedDataStore() = default;
    ~SharedDataStore() = default;

    SharedDataStore(const SharedDataStore &) = delete;
    SharedDataStore &operator=(const SharedDataStore &) = delete;

    bool m_initialized = false;

    DataSlotPool m_slotPool;
    std::map<uint8_t, std::unique_ptr<DataPool>> m_dataPools;

    std::vector<PendingRelease> m_pendingReleases;
    mutable std::unordered_map<std::string, AssetInfo> m_assetMap;
    mutable std::unordered_map<std::string, std::any> m_typedDataStore;
    mutable std::shared_mutex m_assetMutex;
    mutable std::mutex m_pendingMutex;

    Boot::ResourceSystemConfig m_config;

    void InitializeDataPoolsFromConfig(const Boot::ResourceSystemConfig &config);
    DataPool *GetDataPoolForHandle(DataSlotHandle handle) const;
    void ForceRelease(DataSlotHandle handle);
};

} // namespace Core
} // namespace DX12Engine
