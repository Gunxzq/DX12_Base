#include "DescriptorHeapCollection.h"
#include "DescriptorSlotAllocator.h"
#include <cassert>

namespace DX12Engine::Resource {

// ========================================================================
// 辅助：PartitionType → D3D12_DESCRIPTOR_HEAP_TYPE
// ========================================================================
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

// ========================================================================
// 初始化单组 TagHeap 的物理堆
// ========================================================================
void DescriptorHeapCollection::InitializeTagHeap(TagHeap &tagHeap, ID3D12Device *device,
                                                 const std::vector<DescriptorHeapConfig> &configs) {
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

        tagHeap.heaps[config.type] = std::move(entry);
    }
}

// ========================================================================
// 初始化
// ========================================================================
void DescriptorHeapCollection::Initialize(ID3D12Device *device, const std::vector<DescriptorHeapConfig> &configs,
                                          HeapMode mode) {
    if (m_initialized) {
        Shutdown();
    }

    m_device = device;
    m_mode = mode;

    // 始终创建 Default 堆（单堆模式唯一堆，多堆模式的基础堆）
    auto defaultHeap = std::make_unique<TagHeap>();
    InitializeTagHeap(*defaultHeap, device, configs);
    m_tagHeaps[HeapTag::Default] = std::move(defaultHeap);

    // 多堆模式：为每个非 Default 标签预先创建独立堆（ImGui 除外，它通过 InitializeHeap 自定义初始化）
    if (m_mode == HeapMode::Multi) {
        for (uint32_t t = static_cast<uint32_t>(HeapTag::Default) + 1; t < static_cast<uint32_t>(HeapTag::Count); ++t) {
            auto tag = static_cast<HeapTag>(t);
            if (tag == HeapTag::ImGui)
                continue;
            auto tagHeap = std::make_unique<TagHeap>();
            InitializeTagHeap(*tagHeap, device, configs);
            m_tagHeaps[tag] = std::move(tagHeap);
        }
    }

    m_initialized = true;
}

void DescriptorHeapCollection::InitializeHeap(HeapTag tag, const std::vector<DescriptorHeapConfig> &configs) {
    // 单堆模式：路由到 Default，不创建独立堆
    if (m_mode == HeapMode::Single) {
        // 无需额外初始化，AddPartition 会路由到 Default
        return;
    }

    // 多堆模式：创建独立物理堆
    auto tagHeap = std::make_unique<TagHeap>();
    InitializeTagHeap(*tagHeap, m_device, configs);
    m_tagHeaps[tag] = std::move(tagHeap);
}

// ========================================================================
// 获取或创建 TagHeap
// 单堆模式：所有 tag 路由到 Default
// 多堆模式：按 tag 返回独立堆
// ========================================================================
DescriptorHeapCollection::TagHeap &DescriptorHeapCollection::GetOrCreateTagHeap(HeapTag tag) {
    if (m_mode == HeapMode::Single) {
        return *m_tagHeaps[HeapTag::Default];
    }

    auto it = m_tagHeaps.find(tag);
    if (it != m_tagHeaps.end()) {
        return *it->second;
    }

    // 多堆模式下首次使用某个 tag 时创建（懒创建）
    auto newHeap = std::make_unique<TagHeap>();
    // 使用 Default 的配置创建新堆——需要保存初始 configs
    // 但这里我们假设所有 tag 在 Initialize 时已预创建，懒创建为异常路径
    // 如果走到这里，先创建一个空堆，然后由外部调用 AddPartition 等
    auto result = m_tagHeaps.emplace(tag, std::move(newHeap));
    return *result.first->second;
}

DescriptorHeapCollection::TagHeap *DescriptorHeapCollection::FindTagHeap(HeapTag tag) const {
    if (m_mode == HeapMode::Single) {
        auto it = m_tagHeaps.find(HeapTag::Default);
        return it != m_tagHeaps.end() ? it->second.get() : nullptr;
    }
    auto it = m_tagHeaps.find(tag);
    return it != m_tagHeaps.end() ? it->second.get() : nullptr;
}

// ========================================================================
// 分区管理
// ========================================================================
void DescriptorHeapCollection::AddPartition(D3D12_DESCRIPTOR_HEAP_TYPE heapType, PartitionType partition,
                                            uint32_t baseOffset, uint32_t size, HeapTag tag) {
    auto &tagHeap = GetOrCreateTagHeap(tag);
    auto it = tagHeap.heaps.find(heapType);
    if (it == tagHeap.heaps.end()) {
        OutputDebugStringA("[DescriptorHeapCollection] AddPartition FAILED: heap type not found\n");
        return;
    }

    // 从父堆分配器中预占用分区槽位
    uint32_t reservedStart = it->second.allocator->AllocateConsecutive(size);
    char buf[256];
    sprintf_s(
        buf,
        "[DescriptorHeapCollection] AddPartition: tag=%s, partition=%d, baseOffset=%u, size=%u, reservedStart=%u\n",
        HeapTagToString(tag), static_cast<int>(partition), baseOffset, size, reservedStart);
    OutputDebugStringA(buf);

    if (reservedStart == UINT32_MAX) {
        sprintf_s(buf, "[DescriptorHeapCollection] AddPartition FAILED: AllocateConsecutive returned UINT32_MAX\n");
        OutputDebugStringA(buf);
        return;
    }
    if (reservedStart != baseOffset) {
        sprintf_s(buf,
                  "[DescriptorHeapCollection] AddPartition WARNING: reservedStart(%u) != baseOffset(%u), using "
                  "reservedStart\n",
                  reservedStart, baseOffset);
        OutputDebugStringA(buf);
        baseOffset = reservedStart;
    }

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

    tagHeap.partitions[partition] = std::move(entry);
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeapCollection::GetPartitionGpuHandle(PartitionType partition, uint32_t index,
                                                                            HeapTag tag) const {
    auto *tagHeap = FindTagHeap(tag);
    if (!tagHeap)
        return {};

    auto pit = tagHeap->partitions.find(partition);
    if (pit == tagHeap->partitions.end())
        return {};

    auto hit = tagHeap->heaps.find(pit->second.heapType);
    if (hit == tagHeap->heaps.end())
        return {};

    D3D12_GPU_DESCRIPTOR_HANDLE handle = hit->second.heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<uint64_t>(pit->second.baseOffset + index) * hit->second.descriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeapCollection::GetPartitionCpuHandle(PartitionType partition, uint32_t index,
                                                                            HeapTag tag) const {
    auto *tagHeap = FindTagHeap(tag);
    if (!tagHeap)
        return {};

    auto pit = tagHeap->partitions.find(partition);
    if (pit == tagHeap->partitions.end())
        return {};

    auto hit = tagHeap->heaps.find(pit->second.heapType);
    if (hit == tagHeap->heaps.end())
        return {};

    D3D12_CPU_DESCRIPTOR_HANDLE handle = hit->second.heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<uint64_t>(pit->second.baseOffset + index) * hit->second.descriptorSize;
    return handle;
}

uint32_t DescriptorHeapCollection::GetPartitionBaseOffset(PartitionType partition, HeapTag tag) const {
    auto *tagHeap = FindTagHeap(tag);
    if (!tagHeap)
        return 0;

    auto it = tagHeap->partitions.find(partition);
    if (it == tagHeap->partitions.end())
        return 0;
    return it->second.baseOffset;
}

DescriptorHeapCollection::PartitionEntry &DescriptorHeapCollection::GetPartitionEntry(HeapTag tag, PartitionType type) {
    static PartitionEntry invalidEntry;
    auto &tagHeap = GetOrCreateTagHeap(tag);
    auto it = tagHeap.partitions.find(type);
    if (it == tagHeap.partitions.end())
        return invalidEntry;
    return it->second;
}

// ========================================================================
// 分区分配（Tag 感知）
// ========================================================================
uint32_t DescriptorHeapCollection::Allocate(HeapTag tag, PartitionType partition) {
    auto &tagHeap = GetOrCreateTagHeap(tag);
    auto it = tagHeap.partitions.find(partition);
    if (it != tagHeap.partitions.end())
        return it->second.allocator->Allocate();
    // Fallback: 未创建分区的类型直接分配在物理堆上
    auto hit = tagHeap.heaps.find(PartitionToD3D12Type(partition));
    if (hit == tagHeap.heaps.end())
        return UINT32_MAX;
    return hit->second.allocator->Allocate();
}

uint32_t DescriptorHeapCollection::AllocateConsecutive(HeapTag tag, PartitionType partition, uint32_t count) {
    auto &tagHeap = GetOrCreateTagHeap(tag);
    auto it = tagHeap.partitions.find(partition);
    if (it != tagHeap.partitions.end())
        return it->second.allocator->AllocateConsecutive(count);
    auto hit = tagHeap.heaps.find(PartitionToD3D12Type(partition));
    if (hit == tagHeap.heaps.end())
        return UINT32_MAX;
    return hit->second.allocator->AllocateConsecutive(count);
}

void DescriptorHeapCollection::Free(HeapTag tag, PartitionType partition, uint32_t index, uint64_t fenceValue) {
    auto &tagHeap = GetOrCreateTagHeap(tag);
    auto it = tagHeap.partitions.find(partition);
    if (it != tagHeap.partitions.end()) {
        it->second.allocator->Free(index, fenceValue);
        return;
    }
    auto hit = tagHeap.heaps.find(PartitionToD3D12Type(partition));
    if (hit != tagHeap.heaps.end())
        hit->second.allocator->Free(index, fenceValue);
}

void DescriptorHeapCollection::Reclaim(HeapTag tag, PartitionType partition, uint64_t completedFence) {
    auto &tagHeap = GetOrCreateTagHeap(tag);
    auto it = tagHeap.partitions.find(partition);
    if (it == tagHeap.partitions.end())
        return;
    it->second.allocator->Reclaim(completedFence);
}

// ========================================================================
// 物理堆查询
// ========================================================================
ID3D12DescriptorHeap *DescriptorHeapCollection::GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE type, HeapTag tag) const {
    auto *tagHeap = FindTagHeap(tag);
    if (!tagHeap)
        return nullptr;

    auto it = tagHeap->heaps.find(type);
    if (it == tagHeap->heaps.end())
        return nullptr;
    return it->second.heap.Get();
}

uint32_t DescriptorHeapCollection::GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE type, HeapTag tag) const {
    auto *tagHeap = FindTagHeap(tag);
    if (!tagHeap)
        return 0;

    auto it = tagHeap->heaps.find(type);
    if (it == tagHeap->heaps.end())
        return 0;
    return it->second.descriptorSize;
}

// ========================================================================
// 底层分配（内部使用）
// ========================================================================
uint32_t DescriptorHeapCollection::AllocateInternal(HeapTag tag, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    auto &tagHeap = GetOrCreateTagHeap(tag);
    auto it = tagHeap.heaps.find(type);
    if (it == tagHeap.heaps.end())
        return UINT32_MAX;
    return it->second.allocator->Allocate();
}

uint32_t DescriptorHeapCollection::AllocateConsecutiveInternal(HeapTag tag, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                               uint32_t count) {
    auto &tagHeap = GetOrCreateTagHeap(tag);
    auto it = tagHeap.heaps.find(type);
    if (it == tagHeap.heaps.end())
        return UINT32_MAX;
    return it->second.allocator->AllocateConsecutive(count);
}

void DescriptorHeapCollection::FreeInternal(HeapTag tag, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t index,
                                            uint64_t fenceValue) {
    auto &tagHeap = GetOrCreateTagHeap(tag);
    auto it = tagHeap.heaps.find(type);
    if (it == tagHeap.heaps.end())
        return;
    it->second.allocator->Free(index, fenceValue);
}

void DescriptorHeapCollection::ReclaimInternal(HeapTag tag, D3D12_DESCRIPTOR_HEAP_TYPE type, uint64_t completedFence) {
    auto &tagHeap = GetOrCreateTagHeap(tag);
    auto it = tagHeap.heaps.find(type);
    if (it == tagHeap.heaps.end())
        return;
    it->second.allocator->Reclaim(completedFence);
}

// ========================================================================
// 句柄查询
// ========================================================================
D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeapCollection::GetCpuHandle(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t index,
                                                                   HeapTag tag) const {
    auto *tagHeap = FindTagHeap(tag);
    if (!tagHeap)
        return {};

    auto it = tagHeap->heaps.find(type);
    if (it == tagHeap->heaps.end())
        return {};

    D3D12_CPU_DESCRIPTOR_HANDLE handle = it->second.heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<uint64_t>(index) * it->second.descriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeapCollection::GetGpuHandle(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t index,
                                                                   HeapTag tag) const {
    auto *tagHeap = FindTagHeap(tag);
    if (!tagHeap)
        return {};

    auto it = tagHeap->heaps.find(type);
    if (it == tagHeap->heaps.end())
        return {};

    if ((it->second.heap->GetDesc().Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0) {
        return {};
    }

    D3D12_GPU_DESCRIPTOR_HANDLE handle = it->second.heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<uint64_t>(index) * it->second.descriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeapCollection::GetCpuHandle(PartitionType partition, uint32_t index,
                                                                   HeapTag tag) const {
    auto *tagHeap = FindTagHeap(tag);
    if (!tagHeap)
        return {};

    // 如果是已创建的分区，用分区句柄（带 baseOffset）
    auto pit = tagHeap->partitions.find(partition);
    if (pit != tagHeap->partitions.end())
        return GetPartitionCpuHandle(partition, index, tag);
    // 否则回退到物理堆
    auto hit = tagHeap->heaps.find(PartitionToD3D12Type(partition));
    if (hit == tagHeap->heaps.end())
        return {};
    D3D12_CPU_DESCRIPTOR_HANDLE handle = hit->second.heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<uint64_t>(index) * hit->second.descriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeapCollection::GetGpuHandle(PartitionType partition, uint32_t index,
                                                                   HeapTag tag) const {
    auto *tagHeap = FindTagHeap(tag);
    if (!tagHeap)
        return {};

    // 如果是已创建的分区，用分区句柄（带 baseOffset）
    auto pit = tagHeap->partitions.find(partition);
    if (pit != tagHeap->partitions.end())
        return GetPartitionGpuHandle(partition, index, tag);
    // 否则回退到物理堆
    auto hit = tagHeap->heaps.find(PartitionToD3D12Type(partition));
    if (hit == tagHeap->heaps.end())
        return {};
    D3D12_GPU_DESCRIPTOR_HANDLE handle = hit->second.heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<uint64_t>(index) * hit->second.descriptorSize;
    return handle;
}

// ========================================================================
// 容量查询
// ========================================================================
uint32_t DescriptorHeapCollection::GetHeapSize(D3D12_DESCRIPTOR_HEAP_TYPE type, HeapTag tag) const {
    auto *tagHeap = FindTagHeap(tag);
    if (!tagHeap)
        return 0;

    auto it = tagHeap->heaps.find(type);
    if (it == tagHeap->heaps.end())
        return 0;
    return it->second.allocator->GetCapacity();
}

uint32_t DescriptorHeapCollection::GetAllocatedCount(D3D12_DESCRIPTOR_HEAP_TYPE type, HeapTag tag) const {
    auto *tagHeap = FindTagHeap(tag);
    if (!tagHeap)
        return 0;

    auto it = tagHeap->heaps.find(type);
    if (it == tagHeap->heaps.end())
        return 0;
    return it->second.allocator->GetAllocatedCount();
}

// ========================================================================
// 内部辅助
// ========================================================================
DescriptorHeapCollection::HeapEntry &DescriptorHeapCollection::GetHeapEntry(HeapTag tag,
                                                                            D3D12_DESCRIPTOR_HEAP_TYPE type) {
    return GetOrCreateTagHeap(tag).heaps[type];
}

const DescriptorHeapCollection::HeapEntry &
DescriptorHeapCollection::GetHeapEntry(HeapTag tag, D3D12_DESCRIPTOR_HEAP_TYPE type) const {
    auto *tagHeap = FindTagHeap(tag);
    // 注意：此 const 方法仅在已知 tagHeap 存在时调用
    return tagHeap->heaps.at(type);
}

// ========================================================================
// 关闭
// ========================================================================
void DescriptorHeapCollection::Shutdown() {
    if (!m_initialized) {
        return;
    }

    for (auto &pair : m_tagHeaps) {
        if (!pair.second)
            continue;
        auto &tagHeap = *pair.second;

        // 清理分区
        for (auto &partPair : tagHeap.partitions) {
            if (partPair.second.allocator)
                partPair.second.allocator->Shutdown();
        }
        tagHeap.partitions.clear();

        // 清理物理堆
        for (auto &heapPair : tagHeap.heaps) {
            if (heapPair.second.allocator) {
                heapPair.second.allocator->Shutdown();
            }
            heapPair.second.heap.Reset();
        }
        tagHeap.heaps.clear();
    }

    m_tagHeaps.clear();
    m_device = nullptr;
    m_initialized = false;
}

} // namespace DX12Engine::Resource