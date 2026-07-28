#pragma once
#include "DescriptorSlotAllocator.h"
#include "Resource/Struct/Descriptor.h"
#include <d3d12.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

namespace DX12Engine::Resource {

struct DescriptorHeapConfig;

// ========================================================================
// 描述符分区/堆类型
// 主 CbvSrvUav 堆内分区：
//   Texture  → 纹理 SRV（gTextureMaps[] 无界表）
//   Buffer   → MaterialBuffer, InstanceData 等 StructuredBuffer
//   Shadow   → 阴影贴图 SRV
//   Cubemap  → 反射探针 Cubemap Array SRV
//   PostFx   → 后处理临时 RT SRV
// 独立物理堆：
//   Rtv / Dsv / Sampler
// ========================================================================
enum class PartitionType { Texture, Buffer, Shadow, Cubemap, PostFx, Rtv, Dsv, Sampler, Count };

// ========================================================================
// 堆域标签 — 标识调用方所属的堆域
// 多堆模式下每个标签对应独立物理堆，单堆模式下所有标签映射到同一堆
// ========================================================================
enum class HeapTag : uint32_t {
    Default,        // 主场景 / 全局（向后兼容）
    EditorViewport, // 编辑器视口预览
    PostFx,         // 后处理
    ImGui,          // ImGui 渲染
    Count
};

// ========================================================================
// 堆模式 — 控制分配策略
// ========================================================================
enum class HeapMode : uint32_t {
    Single, // 所有 HeapTag 映射到同一个物理堆（Release Game）
    Multi   // 每个 HeapTag 对应独立物理堆（Editor / Debug Game）
};

// ========================================================================
// Helper: HeapTag 转可读名称（调试用）
// ========================================================================
inline const char *HeapTagToString(HeapTag tag) {
    switch (tag) {
    case HeapTag::Default:
        return "Default";
    case HeapTag::EditorViewport:
        return "EditorViewport";
    case HeapTag::PostFx:
        return "PostFx";
    case HeapTag::ImGui:
        return "ImGui";
    default:
        return "Unknown";
    }
}

// ========================================================================
// DescriptorHeapCollection - 描述符堆集合
// 支持单堆/多堆两种模式：
//   Single：全部 HeapTag 共享同一组物理堆（Release Game）
//   Multi：  每个 HeapTag 拥有独立物理堆（Editor / Debug Game）
// ========================================================================
class DescriptorHeapCollection {
public:
    DescriptorHeapCollection() = default;
    ~DescriptorHeapCollection() = default;

    DescriptorHeapCollection(const DescriptorHeapCollection &) = delete;
    DescriptorHeapCollection &operator=(const DescriptorHeapCollection &) = delete;

    void Initialize(ID3D12Device *device, const std::vector<DescriptorHeapConfig> &configs,
                    HeapMode mode = HeapMode::Single);
    void Shutdown();

    HeapMode GetMode() const { return m_mode; }

    // ── 分区管理 ──
    void AddPartition(D3D12_DESCRIPTOR_HEAP_TYPE heapType, PartitionType partition, uint32_t baseOffset, uint32_t size,
                      HeapTag tag = HeapTag::Default,
                      Resource::DescriptorSlotFlags slotFlags = static_cast<Resource::DescriptorSlotFlags>(0));
    D3D12_GPU_DESCRIPTOR_HANDLE GetPartitionGpuHandle(PartitionType partition, uint32_t index,
                                                      HeapTag tag = HeapTag::Default) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetPartitionCpuHandle(PartitionType partition, uint32_t index,
                                                      HeapTag tag = HeapTag::Default) const;
    uint32_t GetPartitionBaseOffset(PartitionType partition, HeapTag tag = HeapTag::Default) const;

    // ── 分区分配（Tag 感知）──
    uint32_t Allocate(HeapTag tag, PartitionType partition);
    uint32_t AllocateConsecutive(HeapTag tag, PartitionType partition, uint32_t count);
    void Free(HeapTag tag, PartitionType partition, uint32_t index, uint64_t fenceValue);
    void Reclaim(HeapTag tag, PartitionType partition, uint64_t completedFence);

    // ── 旧接口（向后兼容，映射到 HeapTag::Default）──
    uint32_t Allocate(PartitionType partition) { return Allocate(HeapTag::Default, partition); }
    uint32_t AllocateConsecutive(PartitionType partition, uint32_t count) {
        return AllocateConsecutive(HeapTag::Default, partition, count);
    }
    void Free(PartitionType partition, uint32_t index, uint64_t fenceValue) {
        Free(HeapTag::Default, partition, index, fenceValue);
    }
    void Reclaim(PartitionType partition, uint64_t completedFence) {
        Reclaim(HeapTag::Default, partition, completedFence);
    }

    // ── 物理堆查询 ──
    ID3D12DescriptorHeap *GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE type, HeapTag tag = HeapTag::Default) const;
    uint32_t GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE type, HeapTag tag = HeapTag::Default) const;

    /// 为指定 HeapTag 初始化自定义物理堆（用于 ImGui 等小型专用堆）
    void InitializeHeap(HeapTag tag, const std::vector<DescriptorHeapConfig> &configs);

    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t index,
                                             HeapTag tag = HeapTag::Default) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t index,
                                             HeapTag tag = HeapTag::Default) const;
    // Partition-aware 句柄（自动处理分区 baseOffset）
    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(PartitionType partition, uint32_t index,
                                             HeapTag tag = HeapTag::Default) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(PartitionType partition, uint32_t index,
                                             HeapTag tag = HeapTag::Default) const;

    uint32_t GetHeapSize(D3D12_DESCRIPTOR_HEAP_TYPE type, HeapTag tag = HeapTag::Default) const;
    uint32_t GetAllocatedCount(D3D12_DESCRIPTOR_HEAP_TYPE type, HeapTag tag = HeapTag::Default) const;

private:
    // ── 内部数据结构 ──
    struct HeapEntry {
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        std::unique_ptr<DescriptorSlotAllocator> allocator;
        D3D12_DESCRIPTOR_HEAP_TYPE d3d12Type;
        uint32_t descriptorSize = 0;
    };

    struct PartitionEntry {
        D3D12_DESCRIPTOR_HEAP_TYPE heapType;
        std::unique_ptr<DescriptorSlotAllocator> allocator;
        uint32_t baseOffset = 0;
        uint32_t size = 0;
    };

    struct TagHeap {
        std::unordered_map<D3D12_DESCRIPTOR_HEAP_TYPE, HeapEntry> heaps;
        std::unordered_map<PartitionType, PartitionEntry> partitions;
    };

    // ── 内部方法 ──
    TagHeap &GetOrCreateTagHeap(HeapTag tag);
    TagHeap *FindTagHeap(HeapTag tag) const;

    HeapEntry &GetHeapEntry(HeapTag tag, D3D12_DESCRIPTOR_HEAP_TYPE type);
    const HeapEntry &GetHeapEntry(HeapTag tag, D3D12_DESCRIPTOR_HEAP_TYPE type) const;
    PartitionEntry &GetPartitionEntry(HeapTag tag, PartitionType type);

    // ── 底层分配（内部使用） ──
    uint32_t AllocateInternal(HeapTag tag, D3D12_DESCRIPTOR_HEAP_TYPE type);
    uint32_t AllocateConsecutiveInternal(HeapTag tag, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count);
    void FreeInternal(HeapTag tag, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t index, uint64_t fenceValue);
    void ReclaimInternal(HeapTag tag, D3D12_DESCRIPTOR_HEAP_TYPE type, uint64_t completedFence);

    void InitializeTagHeap(TagHeap &tagHeap, ID3D12Device *device, const std::vector<DescriptorHeapConfig> &configs);

    // ── 成员变量 ──
    ID3D12Device *m_device = nullptr;
    HeapMode m_mode = HeapMode::Single;
    std::unordered_map<HeapTag, std::unique_ptr<TagHeap>> m_tagHeaps;
    bool m_initialized = false;
};

} // namespace DX12Engine::Resource