#pragma once
#include "System/Resource/Core/HandlePool.h"
#include "System/Resource/ResourceHandle.h"
#include <d3d12.h>
#include <mutex>
#include <vector>
#include <wrl/client.h> // For ComPtr if needed, though we use raw pointers for performance here

namespace DX12Engine {
namespace System {
namespace Resource {

/**
 * @brief GPU 资源管理器
 *
 * 核心职责：
 * 1. 管理 ID3D12Resource (Buffer, Texture) 的生命周期
 * 2. 基于 Fence 值的延迟释放，确保 GPU 不再访问后才销毁
 * 3. 提供无锁句柄分配 (复用 HandlePool)
 */
class GpuResourceManager {
public:
    static GpuResourceManager &GetInstance();

    void Initialize();
    void Shutdown();

    /**
     * @brief 创建 GPU 缓冲区
     * @param device D3D12 设备指针
     * @param size 缓冲区大小
     * @param heapType 堆类型 (DEFAULT, UPLOAD, READBACK)
     * @param initialState 初始资源状态
     * @return 有效的 ResourceHandle
     */
    ResourceHandle CreateBuffer(ID3D12Device *device, size_t size, D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT,
                                D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON);

    /**
     * @brief 创建 GPU 纹理 (2D)
     * @param device D3D12 设备指针
     * @param width 宽度
     * @param height 高度
     * @param format DXGI 格式
     * @param initialState 初始资源状态
     * @return 有效的 ResourceHandle
     */
    ResourceHandle CreateTexture2D(ID3D12Device *device, uint32_t width, uint32_t height,
                                   DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM,
                                   D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON);

    /**
     * @brief 获取 GPU 资源指针
     * @note 如果句柄无效或资源已释放，返回 nullptr
     */
    ID3D12Resource *GetResource(ResourceHandle handle) const;

    /**
     * @brief 请求释放 GPU 资源
     * @param handle 资源句柄
     * @param fenceValue 围栏值。当 GPU 完成到此围栏值时，资源才会被真正销毁
     */
    void Release(ResourceHandle handle, uint64_t fenceValue);

    /**
     * @brief 每帧更新
     * @note 由主线程调用。检查已完成 Fence，清理过期的 GPU 资源
     * @param completedFenceValue 当前 GPU 已完成的最新围栏值
     */
    void Update(uint64_t completedFenceValue);

    // ------------------------------------------------------------------
    // 调试/监控
    // ------------------------------------------------------------------
    uint32_t GetActiveCount() const;
    size_t GetTotalGpuMemoryUsage() const; // 需要手动维护统计

private:
    GpuResourceManager() = default;
    ~GpuResourceManager() = default;

    GpuResourceManager(const GpuResourceManager &) = delete;
    GpuResourceManager &operator=(const GpuResourceManager &) = delete;

    bool m_initialized = false;
    HandlePool m_handlePool;

    struct PendingGpuRelease {
        ResourceHandle handle;
        uint64_t fenceValue;
    };
    std::vector<PendingGpuRelease> m_pendingReleases;
    mutable std::mutex m_mutex;

    // 简单的内存统计 (原子操作或锁保护)
    size_t m_totalMemoryUsage = 0;
};

} // namespace Resource
} // namespace System
} // namespace DX12Engine