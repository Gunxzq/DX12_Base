// HandlePool.h
#pragma once
#include "System/Resource/ResourceHandle.h"
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <vector>

namespace DX12Engine {
namespace System {
namespace Resource {

struct ThreadLocalCache;

enum class ResourceState : uint8_t { Empty, Loading, Ready, Error, PendingRelease };

enum class ResourceType : uint8_t { Unknown, Mesh, Texture, Audio, Shader };

class HandlePool {

    friend struct ThreadLocalCache;

public:
    static constexpr uint32_t INITIAL_CAPACITY = 4096;

    struct InitConfig {
        uint32_t maxTotalHandles = 0;        // 0 = 使用默认 INITIAL_CAPACITY
        uint32_t initialFreeListReserve = 0; // 0 = 使用默认
    };

    HandlePool();
    ~HandlePool();

    void Initialize(const InitConfig &config = {});
    void Shutdown();

    // 显式收割当前线程的 TLS 缓存
    // 由 ResourceManager::ForceCleanupForTesting() 调用
#ifdef _DEBUG
    void HarvestTLSCaches();

    // 强制重置池子状态（仅用于测试）
    // 绕过所有检查，将所有槽位标记为空闲态，强制让 GetActiveCount() 返回 0
    void ForceResetForTesting();
#endif

    // 预分配到指定容量，减少扩容次数
    void Preallocate(uint32_t targetCapacity);

    ResourceHandle AllocateSlot(ResourceType type, uint8_t poolId = 0);
    void FreeSlot(ResourceHandle handle);

    void SetState(ResourceHandle handle, ResourceState state);
    ResourceState GetState(ResourceHandle handle) const;

    void SetDataPtr(ResourceHandle handle, void *ptr);
    void *GetDataPtr(ResourceHandle handle) const;

    bool Validate(ResourceHandle handle) const;

    uint32_t GetActiveCount() const;

private:
    mutable std::mutex m_mutex;

    // 使用 unique_ptr 管理原子数组，避免 vector::resize() 拷贝 atomic 的问题
    std::vector<ResourceType> m_types;
    std::unique_ptr<std::atomic<ResourceState>[]> m_states;
    std::unique_ptr<std::atomic<uint32_t>[]> m_generations; // 【修复】原子数组，防止 ABA 竞态
    std::unique_ptr<std::atomic<void *>[]> m_dataPtrs;

    std::vector<uint32_t> m_freeIndices;
    uint32_t m_capacity = 0;

    // 初始化状态标志（防止 Shutdown 后其他线程继续访问已释放的数组）
    bool m_initialized = false;

    void ExpandCapacity();
    void Preallocate_Locked(uint32_t targetCapacity);
};

} // namespace Resource
} // namespace System
} // namespace DX12Engine