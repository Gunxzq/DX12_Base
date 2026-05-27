#pragma once
#include "Core/GpuHandlePool.h"
#include "Struct/ResourceHandle.h"
#include <d3d12.h>
#include <mutex>
#include <vector>
#include <wrl/client.h>

namespace DX12Engine {

namespace Resource {

/**
 * @brief GPU 资源管理器
 */
class GpuResourceManager {
public:
    static GpuResourceManager &GetInstance();

    void Initialize();
    void Shutdown();

    GpuResourceHandle CreateBuffer(ID3D12Device *device, size_t size,
                                   D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT,
                                   D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON);

    GpuResourceHandle CreateTexture2D(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc,
                                      D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource *GetResource(GpuResourceHandle handle) const;

    void Release(GpuResourceHandle handle, uint64_t fenceValue);

    void Update(uint64_t completedFenceValue);

    uint32_t GetActiveCount() const;
    size_t GetTotalGpuMemoryUsage() const; // 需要手动维护统计

private:
    struct PendingGpuRelease {
        GpuResourceHandle handle;
        uint64_t fenceValue;
    };

private:
    GpuResourceManager() = default;
    ~GpuResourceManager() = default;

    GpuResourceManager(const GpuResourceManager &) = delete;
    GpuResourceManager &operator=(const GpuResourceManager &) = delete;

    bool m_initialized = false;
    GpuHandlePool m_handlePool;

    std::vector<PendingGpuRelease> m_pendingReleases;
    mutable std::mutex m_mutex;

    // 简单的内存统计 (原子操作或锁保护)
    size_t m_totalMemoryUsage = 0;
};

} // namespace Resource

} // namespace DX12Engine