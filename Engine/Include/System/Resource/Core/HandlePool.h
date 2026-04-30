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

enum class ResourceState : uint8_t { Empty, Loading, Ready, Error, PendingRelease };

enum class ResourceType : uint8_t { Unknown, Mesh, Texture, Audio, Shader };

class HandlePool {
public:
    static constexpr uint32_t INITIAL_CAPACITY = 4096;

    HandlePool();
    ~HandlePool();

    void Initialize();
    void Shutdown();

    // 【新增】预分配到指定容量，减少扩容次数
    void Preallocate(uint32_t targetCapacity);

    ResourceHandle AllocateSlot(ResourceType type);
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
};

} // namespace Resource
} // namespace System
} // namespace DX12Engine