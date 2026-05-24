#pragma once
#include "Resource/Struct/Descriptor.h"
#include <d3d12.h>
#include <memory>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

namespace DX12Engine::Resource {

class DescriptorSlotAllocator;
struct DescriptorHeapConfig;

class DescriptorHeapCollection {
public:
    DescriptorHeapCollection() = default;
    ~DescriptorHeapCollection() = default;

    DescriptorHeapCollection(const DescriptorHeapCollection &) = delete;
    DescriptorHeapCollection &operator=(const DescriptorHeapCollection &) = delete;

    void Initialize(ID3D12Device *device, const std::vector<DescriptorHeapConfig> &configs);
    void Shutdown();

    ID3D12DescriptorHeap *GetHeap(DescriptorHeapType type) const;
    uint32_t GetDescriptorSize(DescriptorHeapType type) const;

    uint32_t Allocate(DescriptorHeapType type);
    void Free(DescriptorHeapType type, uint32_t index, uint64_t fenceValue);
    void Reclaim(DescriptorHeapType type, uint64_t completedFence);

    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(DescriptorHeapType type, uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(DescriptorHeapType type, uint32_t index) const;

    uint32_t GetHeapSize(DescriptorHeapType type) const;
    uint32_t GetAllocatedCount(DescriptorHeapType type) const;

private:
    struct HeapEntry {
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        std::unique_ptr<DescriptorSlotAllocator> allocator;
        D3D12_DESCRIPTOR_HEAP_TYPE d3d12Type;
        uint32_t descriptorSize = 0;
    };

private:
    HeapEntry &GetHeapEntry(DescriptorHeapType type);
    const HeapEntry &GetHeapEntry(DescriptorHeapType type) const;

    ID3D12Device *m_device = nullptr;
    std::unordered_map<DescriptorHeapType, HeapEntry> m_heaps;
    bool m_initialized = false;
};

} // namespace DX12Engine::Resource