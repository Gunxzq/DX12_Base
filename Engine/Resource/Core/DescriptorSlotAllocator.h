#pragma once
#include "Resource/Struct/Descriptor.h"
#include <cstdint>
#include <vector>

namespace DX12Engine::Resource {

struct DescriptorSlotAllocatorConfig;

class DescriptorSlotAllocator {
public:
    DescriptorSlotAllocator() = default;
    ~DescriptorSlotAllocator() = default;

    DescriptorSlotAllocator(const DescriptorSlotAllocator &) = delete;
    DescriptorSlotAllocator &operator=(const DescriptorSlotAllocator &) = delete;

    void Initialize(const DescriptorSlotAllocatorConfig &config);
    void Shutdown();
    void Reset();

    // 描述符槽管理
    uint32_t Allocate();
    void Free(uint32_t index, uint64_t fenceValue);
    void Reclaim(uint64_t completedFence);

    void Reserve(uint32_t targetCapacity);

    // 调试
    uint32_t GetAllocatedCount() const { return m_allocatedCount; }
    uint32_t GetCapacity() const { return m_capacity; }
    bool IsInitialized() const { return m_initialized; }

private:
    void Expand(uint32_t newCapacity);
    uint32_t AllocateLinear();
    uint32_t AllocateFromFreeList();

private:
    struct PendingFree {
        uint32_t index;
        uint64_t fenceValue;
    };

private:
    DescriptorSlotAllocatorConfig m_config; // 配置参数
    std::vector<uint32_t> m_freeIndices;    // 可用索引列表
    std::vector<PendingFree> m_pendingFree; // 待释放索引列表
    uint32_t m_nextIndex = 0;               // 下一个可用索引
    uint32_t m_capacity = 0;                // 当前容量
    uint32_t m_allocatedCount = 0;          // 已分配索引数量
    bool m_initialized = false;             // 是否初始化
};

} // namespace DX12Engine::Resource