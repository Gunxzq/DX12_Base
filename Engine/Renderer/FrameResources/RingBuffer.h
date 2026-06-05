#pragma once

#include "Common/d3dUtil.h"
#include <queue>
#include <wrl/client.h>

namespace DX12Engine::Renderer {

/**
 * @brief GPU 环形缓冲区分配器
 *
 * 管理单块环形缓冲区，支持每分配独立围栏追踪和精细回收。
 * 设计用于帧资源的动态分配（ObjectCB、骨骼矩阵、实例数据等）。
 */
class RingBuffer {
public:
    RingBuffer() = default;
    ~RingBuffer();

    RingBuffer(const RingBuffer &) = delete;
    RingBuffer &operator=(const RingBuffer &) = delete;

    bool Initialize(ID3D12Device *device, uint32_t size, D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_UPLOAD);
    void Shutdown();
    void Reset();

    D3D12_GPU_VIRTUAL_ADDRESS Allocate(uint32_t size, uint64_t fence, uint32_t alignment = 256);
    D3D12_GPU_VIRTUAL_ADDRESS AllocateUpload(const void *data, uint32_t size, uint64_t fence, uint32_t alignment = 256);
    void Reclaim(uint64_t completedFence);

#ifdef GetFreeSpace
#undef GetFreeSpace
#endif
    uint32_t GetFreeSpace() const;

    D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const { return m_gpuAddress; }

    uint32_t GetSize() const { return m_size; }
    uint32_t GetAllocatedSize() const { return m_allocatedSize; }
    void *GetCPUAddress(uint32_t offset) const;

    bool IsInitialized() const { return m_initialized; }

    // 获取 GPU 地址（基于偏移）
    D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress(uint32_t offset) const;

    // 释放指定偏移的空间（标记待回收）
    void Free(uint32_t offset, uint64_t fence);

    // 获取当前已使用大小
    uint32_t GetUsedSize() const { return m_allocatedSize; }

private:
    struct PendingAlloc {
        uint32_t size;
        uint64_t fence;
    };

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    void *m_mappedData = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS m_gpuAddress = 0;
    uint32_t m_size = 0;
    uint32_t m_head = 0;          // 写指针
    uint32_t m_tail = 0;          // 读指针（已回收位置）
    uint32_t m_allocatedSize = 0; // 当前已分配总量
    std::queue<PendingAlloc> m_pending;
    bool m_initialized = false;
};

} // namespace DX12Engine::Renderer