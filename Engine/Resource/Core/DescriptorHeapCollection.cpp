#include "DescriptorHeapCollection.h"
#include "DescriptorSlotAllocator.h"
#include <cassert>

namespace DX12Engine::Resource {

/**
 * @brief 将DescriptorHeapType转换为D3D12_DESCRIPTOR_HEAP_TYPE
 * @param type 要转换的DescriptorHeap类型
 * @return D3D12_DESCRIPTOR_HEAP_TYPE
 * @date 2026-05-24
 */
static D3D12_DESCRIPTOR_HEAP_TYPE ToD3D12Type(DescriptorHeapType type) {
    switch (type) {
    case DescriptorHeapType::CbvSrvUav:
        return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    case DescriptorHeapType::Rtv:
        return D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    case DescriptorHeapType::Dsv:
        return D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    case DescriptorHeapType::Sampler:
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
        D3D12_DESCRIPTOR_HEAP_TYPE d3d12Type = ToD3D12Type(config.type);

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
void DescriptorHeapCollection::Shutdown() {
    if (!m_initialized) {
        return;
    }

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
ID3D12DescriptorHeap *DescriptorHeapCollection::GetHeap(DescriptorHeapType type) const {
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
uint32_t DescriptorHeapCollection::GetDescriptorSize(DescriptorHeapType type) const {
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
uint32_t DescriptorHeapCollection::Allocate(DescriptorHeapType type) {
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
uint32_t DescriptorHeapCollection::AllocateConsecutive(DescriptorHeapType type, uint32_t count) {
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
void DescriptorHeapCollection::Free(DescriptorHeapType type, uint32_t index, uint64_t fenceValue) {
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
void DescriptorHeapCollection::Reclaim(DescriptorHeapType type, uint64_t completedFence) {
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
D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeapCollection::GetCpuHandle(DescriptorHeapType type, uint32_t index) const {
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
D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeapCollection::GetGpuHandle(DescriptorHeapType type, uint32_t index) const {
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
uint32_t DescriptorHeapCollection::GetHeapSize(DescriptorHeapType type) const {
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
uint32_t DescriptorHeapCollection::GetAllocatedCount(DescriptorHeapType type) const {
    auto it = m_heaps.find(type);
    if (it == m_heaps.end()) {
        return 0;
    }
    return it->second.allocator->GetAllocatedCount();
}

} // namespace DX12Engine::Resource