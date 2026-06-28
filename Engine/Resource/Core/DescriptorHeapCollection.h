#pragma once
#include "DescriptorSlotAllocator.h"
#include "Resource/Struct/Descriptor.h"
#include <d3d12.h>
#include <memory>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

namespace DX12Engine::Resource {

struct DescriptorHeapConfig;

// 描述符分区/堆类型
// 主 CbvSrvUav 堆内分区：
//   Texture  → 纹理 SRV（gTextureMaps[] 无界表）
//   Buffer   → MaterialBuffer, InstanceData 等 StructuredBuffer
//   Shadow   → 阴影贴图 SRV
//   Cubemap  → 反射探针 Cubemap Array SRV
//   PostFx   → 后处理临时 RT SRV
// 独立物理堆：
//   Rtv / Dsv / Sampler
enum class PartitionType {
    Texture, Buffer, Shadow, Cubemap, PostFx,
    Rtv, Dsv, Sampler,
    Count
};

class DescriptorHeapCollection {
public:
    DescriptorHeapCollection() = default;
    ~DescriptorHeapCollection() = default;

    DescriptorHeapCollection(const DescriptorHeapCollection &) = delete;
    DescriptorHeapCollection &operator=(const DescriptorHeapCollection &) = delete;

    void Initialize(ID3D12Device *device, const std::vector<DescriptorHeapConfig> &configs);
    void Shutdown();

    // ── 分区管理 ──
    // 在指定 D3D12_DESCRIPTOR_HEAP_TYPE 的物理堆中创建逻辑分区
    void AddPartition(D3D12_DESCRIPTOR_HEAP_TYPE heapType, PartitionType partition, uint32_t baseOffset, uint32_t size);
    D3D12_GPU_DESCRIPTOR_HANDLE GetPartitionGpuHandle(PartitionType partition, uint32_t index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetPartitionCpuHandle(PartitionType partition, uint32_t index) const;
    uint32_t GetPartitionBaseOffset(PartitionType partition) const;

    // ── 分区分配（纹理 SRV 等使用分区分配）──
    uint32_t Allocate(PartitionType partition);
    uint32_t AllocateConsecutive(PartitionType partition, uint32_t count);
    void Free(PartitionType partition, uint32_t index, uint64_t fenceValue);
    void Reclaim(PartitionType partition, uint64_t completedFence);

    // ── 物理堆查询 ──
    ID3D12DescriptorHeap *GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE type) const;
    uint32_t GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE type) const;

    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t index) const;
    // Partition-aware 句柄（自动处理分区 baseOffset）
    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(PartitionType partition, uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(PartitionType partition, uint32_t index) const;

    uint32_t GetHeapSize(D3D12_DESCRIPTOR_HEAP_TYPE type) const;
    uint32_t GetAllocatedCount(D3D12_DESCRIPTOR_HEAP_TYPE type) const;

private:
    // ── 传统底层分配（内部使用，外部请用 PartitionType）──
    uint32_t Allocate(D3D12_DESCRIPTOR_HEAP_TYPE type);
    uint32_t AllocateConsecutive(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count);
    void Free(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t index, uint64_t fenceValue);
    void Reclaim(D3D12_DESCRIPTOR_HEAP_TYPE type, uint64_t completedFence);
    struct HeapEntry {
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        std::unique_ptr<DescriptorSlotAllocator> allocator; // 默认分配器（Common 分区）
        D3D12_DESCRIPTOR_HEAP_TYPE d3d12Type;
        uint32_t descriptorSize = 0;
    };

    struct PartitionEntry {
        D3D12_DESCRIPTOR_HEAP_TYPE heapType;
        std::unique_ptr<DescriptorSlotAllocator> allocator;
        uint32_t baseOffset = 0;
        uint32_t size = 0;
    };

private:
    HeapEntry &GetHeapEntry(D3D12_DESCRIPTOR_HEAP_TYPE type) { return m_heaps[type]; };
    const HeapEntry &GetHeapEntry(D3D12_DESCRIPTOR_HEAP_TYPE type) const { return m_heaps.at(type); };
    PartitionEntry &GetPartitionEntry(PartitionType type);

    ID3D12Device *m_device = nullptr;
    std::unordered_map<D3D12_DESCRIPTOR_HEAP_TYPE, HeapEntry> m_heaps;
    std::unordered_map<PartitionType, PartitionEntry> m_partitions;
    bool m_initialized = false;
};

} // namespace DX12Engine::Resource
