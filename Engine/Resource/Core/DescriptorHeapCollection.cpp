#include "DescriptorHeapCollection.h"
#include "DescriptorSlotAllocator.h"
#include <cassert>

namespace DX12Engine::Resource {

/**
 * @brief 将PartitionType映射到D3D12_DESCRIPTOR_HEAP_TYPE
 */
static D3D12_DESCRIPTOR_HEAP_TYPE PartitionToD3D12Type(PartitionType partition) {
    switch (partition) {
    case PartitionType::Texture:
    case PartitionType::Buffer:
    case PartitionType::Shadow:
    case PartitionType::Cubemap:
    case PartitionType::PostFx:
        return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    case PartitionType::Rtv:
        return D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    case PartitionType::Dsv:
        return D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    case PartitionType::Sampler:
        return D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    default:
        return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    }
}

/**
 * @brief 初始化DescriptorHeapCollection
 * @param device 初始化时使用的D3D12设备
 * @param configs 描述符堆配置
 * @date 2026-05-24
 */
void DescriptorHeapCollection::Initialize(ID3D12Device *device, const std::vector<DescriptorHeapConfig> &configs) {
    if (m_initialized) {
        Shutdown();
    }

    m_device = device;

    for (const auto &config : configs) {
        D3D12_DESCRIPTOR_HEAP_TYPE d3d12Type = config.type;

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = d3d12Type;
        heapDesc.NumDescriptors = config.initialSize;
        heapDesc.Flags =
            config.shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        heapDesc.NodeMask = 0;

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap));
        if (FAILED(hr)) {
            continue;
        }

        DescriptorSlotAllocatorConfig allocatorConfig;
        allocatorConfig.initialCapacity = config.initialSize;
        allocatorConfig.maxCapacity = config.maxSize;
        allocatorConfig.flags = config.slotFlags;

        auto allocator = std::make_unique<DescriptorSlotAllocator>();
        allocator->Initialize(allocatorConfig);

        uint32_t descriptorSize = device->GetDescriptorHandleIncrementSize(d3d12Type);

        HeapEntry entry;
        entry.heap = heap;
        entry.allocator = std::move(allocator);
        entry.d3d12Type = d3d12Type;
        entry.descriptorSize = descriptorSize;

        m_heaps[config.type] = std::move(entry);
    }

    m_initialized = true;
}

/**
 * @brief 关闭DescriptorHeapCollection
 * @date 2026-05-24
 */
/**
 * @brief 在指定物理堆中创建逻辑分区
 * @param heapType 物理堆类型（如 CbvSrvUav）
 * @param partition 分区类型（如 TextureSrv）
 * @param baseOffset 分区在物理堆中的起始槽位
 * @param size 分区大小（槽位数）
 * @date 2026-06-28
 */
void DescriptorHeapCollection::AddPartition(D3D12_DESCRIPTOR_HEAP_TYPE heapType, PartitionType partition, uint32_t baseOffset,
                                            uint32_t size) {
    auto it = m_heaps.find(heapType);
    if (it == m_heaps.end())
        return;

    // 从父堆分配器中预占用分区槽位，避免与父堆的其他分配冲突
    uint32_t reservedStart = it->second.allocator->AllocateConsecutive(size);
    if (reservedStart == UINT32_MAX || reservedStart != baseOffset)
        return;
    DescriptorSlotAllocatorConfig allocatorConfig;
    allocatorConfig.initialCapacity = size;
    allocatorConfig.maxCapacity = size;
    allocatorConfig.flags = DescriptorSlotFlags::LinearAlloc | DescriptorSlotFlags::EnableExpand;

    auto allocator = std::make_unique<DescriptorSlotAllocator>();
    allocator->Initialize(allocatorConfig);

    PartitionEntry entry;
    entry.heapType = heapType;
    entry.allocator = std::move(allocator);
    entry.baseOffset = baseOffset;
    entry.size = size;

    m_partitions[partition] = std::move(entry);
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeapCollection::GetPartitionGpuHandle(PartitionType partition,
                                                                            uint32_t index) const {
    auto pit = m_partitions.find(partition);
    if (pit == m_partitions.end())
        return {};

    auto hit = m_heaps.find(pit->second.heapType);
    if (hit == m_heaps.end())
        return {};

    D3D12_GPU_DESCRIPTOR_HANDLE handle = hit->second.heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<uint64_t>(pit->second.baseOffset + index) * hit->second.descriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeapCollection::GetPartitionCpuHandle(PartitionType partition,
                                                                            uint32_t index) const {
    auto pit = m_partitions.find(partition);
    if (pit == m_partitions.end())
        return {};

    auto hit = m_heaps.find(pit->second.heapType);
    if (hit == m_heaps.end())
        return {};

    D3D12_CPU_DESCRIPTOR_HANDLE handle = hit->second.heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<uint64_t>(pit->second.baseOffset + index) * hit->second.descriptorSize;
    return handle;
}

uint32_t DescriptorHeapCollection::GetPartitionBaseOffset(PartitionType partition) const {
    auto it = m_partitions.find(partition);
    if (it == m_partitions.end())
        return 0;
    return it->second.baseOffset;
}

DescriptorHeapCollection::PartitionEntry &DescriptorHeapCollection::GetPartitionEntry(PartitionType type) {
    static PartitionEntry invalidEntry;
    auto it = m_partitions.find(type);
    if (it == m_partitions.end())
        return invalidEntry;
    return it->second;
}

uint32_t DescriptorHeapCollection::Allocate(PartitionType partition) {
    auto it = m_partitions.find(partition);
    if (it != m_partitions.end())
        return it->second.allocator->Allocate();
    // Fallback: 未创建分区的类型直接分配在物理堆上（如 Rtv/Dsv/Sampler）
    auto hit = m_heaps.find(PartitionToD3D12Type(partition));
    if (hit == m_heaps.end())
        return UINT32_MAX;
    return hit->second.allocator->Allocate();
}

uint32_t DescriptorHeapCollection::AllocateConsecutive(PartitionType partition, uint32_t count) {
    auto it = m_partitions.find(partition);
    if (it != m_partitions.end())
        return it->second.allocator->AllocateConsecutive(count);
    auto hit = m_heaps.find(PartitionToD3D12Type(partition));
    if (hit == m_heaps.end())
        return UINT32_MAX;
    return hit->second.allocator->AllocateConsecutive(count);
}

void DescriptorHeapCollection::Free(PartitionType partition, uint32_t index, uint64_t fenceValue) {
    auto it = m_partitions.find(partition);
    if (it != m_partitions.end()) {
        it->second.allocator->Free(index, fenceValue);
        return;
    }
    auto hit = m_heaps.find(PartitionToD3D12Type(partition));
    if (hit != m_heaps.end())
        hit->second.allocator->Free(index, fenceValue);
}

void DescriptorHeapCollection::Reclaim(PartitionType partition, uint64_t completedFence) {
    auto it = m_partitions.find(partition);
    if (it == m_partitions.end())
        return;
    it->second.allocator->Reclaim(completedFence);
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeapCollection::GetGpuHandle(PartitionType partition, uint32_t index) const {
    // 如果是已创建的分区，用分区句柄（带 baseOffset）
    auto pit = m_partitions.find(partition);
    if (pit != m_partitions.end())
        return GetPartitionGpuHandle(partition, index);
    // 否则回退到物理堆
    auto hit = m_heaps.find(PartitionToD3D12Type(partition));
    if (hit == m_heaps.end())
        return {};
    D3D12_GPU_DESCRIPTOR_HANDLE handle = hit->second.heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<uint64_t>(index) * hit->second.descriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeapCollection::GetCpuHandle(PartitionType partition, uint32_t index) const {
    auto pit = m_partitions.find(partition);
    if (pit != m_partitions.end())
        return GetPartitionCpuHandle(partition, index);
    auto hit = m_heaps.find(PartitionToD3D12Type(partition));
    if (hit == m_heaps.end())
        return {};
    D3D12_CPU_DESCRIPTOR_HANDLE handle = hit->second.heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<uint64_t>(index) * hit->second.descriptorSize;
    return handle;
}

void DescriptorHeapCollection::Shutdown() {
    if (!m_initialized) {
        return;
    }

    for (auto &pair : m_partitions) {
        if (pair.second.allocator)
            pair.second.allocator->Shutdown();
    }
    m_partitions.clear();

    for (auto &pair : m_heaps) {
        if (pair.second.allocator) {
            pair.second.allocator->Shutdown();
        }
        pair.second.heap.Reset();
    }

    m_heaps.clear();
    m_device = nullptr;
    m_initialized = false;
}

/**
 * @brief 获取DescriptorHeap
 * @param type 获取DescriptorHeap的DescriptorHeap类型
 * @return ID3D12DescriptorHeap*
 * @date 2026-05-24
 */
ID3D12DescriptorHeap *DescriptorHeapCollection::GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE type) const {
    auto it = m_heaps.find(type);
    if (it == m_heaps.end()) {
        return nullptr;
    }
    return it->second.heap.Get();
}

/**
 * @brief 获取DescriptorHeap的DescriptorSize
 * @param type 获取DescriptorSize的DescriptorHeap类型
 * @return uint32_t
 * @date 2026-05-24
 */
uint32_t DescriptorHeapCollection::GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE type) const {
    auto it = m_heaps.find(type);
    if (it == m_heaps.end()) {
        return 0;
    }
    return it->second.descriptorSize;
}

/**
 * @brief 分配DescriptorHeap的Descriptor
 * @param type 分配的DescriptorHeap类型
 * @return uint32_t
 * @date 2026-05-24
 */
uint32_t DescriptorHeapCollection::Allocate(D3D12_DESCRIPTOR_HEAP_TYPE type) {
    auto it = m_heaps.find(type);
    if (it == m_heaps.end()) {
        return UINT32_MAX;
    }
    return it->second.allocator->Allocate();
}

/**
 * @brief 分配 count 个连续描述符槽，返回起始索引
 * @param type 分配的DescriptorHeap类型
 * @param count 需要的连续槽位数量
 * @return uint32_t 起始索引，失败返回 UINT32_MAX
 * @date 2026-06-07
 */
uint32_t DescriptorHeapCollection::AllocateConsecutive(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count) {
    auto it = m_heaps.find(type);
    if (it == m_heaps.end()) {
        return UINT32_MAX;
    }
    return it->second.allocator->AllocateConsecutive(count);
}

/**
 * @brief 释放DescriptorHeap的Descriptor
 * @param type
 * @param index 释放的Descriptor索引
 * @param fenceValue 释放的Fence值，用于同步操作
 * @date 2026-05-24
 */
void DescriptorHeapCollection::Free(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t index, uint64_t fenceValue) {
    auto it = m_heaps.find(type);
    if (it == m_heaps.end()) {
        return;
    }
    it->second.allocator->Free(index, fenceValue);
}

/**
 * @brief 回收DescriptorHeap的Descriptor
 * @param type
 * @param completedFence
 * @date 2026-05-24
 */
void DescriptorHeapCollection::Reclaim(D3D12_DESCRIPTOR_HEAP_TYPE type, uint64_t completedFence) {
    auto it = m_heaps.find(type);
    if (it == m_heaps.end()) {
        return;
    }
    it->second.allocator->Reclaim(completedFence);
}

/**
 * @brief 获取DescriptorHeap的CPU描述符句柄
 * @param type 获取CPU描述符句柄的DescriptorHeap类型
 * @param index 获取CPU描述符句柄的Descriptor索引
 * @return D3D12_CPU_DESCRIPTOR_HANDLE
 * @date 2026-05-24
 */
D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeapCollection::GetCpuHandle(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t index) const {
    auto it = m_heaps.find(type);
    if (it == m_heaps.end()) {
        return {};
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = it->second.heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<uint64_t>(index) * it->second.descriptorSize;
    return handle;
}

/**
 * @brief 获取DescriptorHeap的GPU描述符句柄
 * @param type 获取GPU描述符句柄的DescriptorHeap类型
 * @param index 获取GPU描述符句柄的Descriptor索引
 * @return D3D12_GPU_DESCRIPTOR_HANDLE
 * @date 2026-05-24
 */
D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeapCollection::GetGpuHandle(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t index) const {
    auto it = m_heaps.find(type);
    if (it == m_heaps.end()) {
        return {};
    }

    if ((it->second.heap->GetDesc().Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0) {
        return {};
    }

    D3D12_GPU_DESCRIPTOR_HANDLE handle = it->second.heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<uint64_t>(index) * it->second.descriptorSize;
    return handle;
}

/**
 * @brief 获取DescriptorHeap的Descriptor数量
 * @param type 获取Descriptor数量的DescriptorHeap类型
 * @return uint32_t
 * @date 2026-05-24
 */
uint32_t DescriptorHeapCollection::GetHeapSize(D3D12_DESCRIPTOR_HEAP_TYPE type) const {
    auto it = m_heaps.find(type);
    if (it == m_heaps.end()) {
        return 0;
    }
    return it->second.allocator->GetCapacity();
}

/**
 * @brief 获取DescriptorHeap的已分配Descriptor数量
 * @param type 获取已分配Descriptor数量的DescriptorHeap类型
 * @return uint32_t
 * @date 2026-05-24
 */
uint32_t DescriptorHeapCollection::GetAllocatedCount(D3D12_DESCRIPTOR_HEAP_TYPE type) const {
    auto it = m_heaps.find(type);
    if (it == m_heaps.end()) {
        return 0;
    }
    return it->second.allocator->GetAllocatedCount();
}

} // namespace DX12Engine::Resource