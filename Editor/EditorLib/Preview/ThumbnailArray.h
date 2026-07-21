#pragma once

#include "Common/d3dUtil.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include <cstdint>
#include <vector>

namespace DX12Engine::Resource {
class DescriptorHeapCollection;
} // namespace DX12Engine::Resource



namespace DX12Engine::Renderer { class D3D12DeviceContext; }

/**
 * @brief 缩略图纹理数组 — 共享 TEX2D_ARRAY 管理
 *
 * 管理一个 TEX2D_ARRAY（256x256, R8G8B8A8_UNORM），每个 slice 对应一个缩略图。
 * 提供 AllocSlice/FreeSlice、RGBA8 像素数据上传/回读，以及 RTV/每 slice SRV 的接口。
 */
class ThumbnailArray {
public:
    static constexpr uint32_t DEFAULT_CAPACITY = 512;
    static constexpr uint32_t SLICE_SIZE = 256; // 256x256

    ThumbnailArray() = default;
    ~ThumbnailArray() { Shutdown(); }

    ThumbnailArray(const ThumbnailArray &) = delete;
    ThumbnailArray &operator=(const ThumbnailArray &) = delete;

    bool Initialize(ID3D12Device *device, DX12Engine::Resource::DescriptorHeapCollection *descriptorHeaps);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    /// 分配一个空闲 slice，返回 slice 索引（0-based）。失败返回 UINT32_MAX。
    uint32_t AllocSlice();

    /// 释放指定 slice
    void FreeSlice(uint32_t slice);

    /// 获取整个纹理数组的 GPU 资源
    ID3D12Resource *GetResource() const { return m_textureArray.Get(); }

    /// 获取某个 slice 的 RTV CPU 句柄
    D3D12_CPU_DESCRIPTOR_HANDLE GetRtvHandle(uint32_t slice) const;

    /// 将 RGBA8 像素数据上传到指定 slice（通过 COPY 队列）
    bool UploadToSlice(uint32_t slice, uint32_t width, uint32_t height, const void *pixels,
                       DX12Engine::Renderer::D3D12DeviceContext *deviceCtx);

    /// 将指定 slice 回读到 CPU 内存（调用方负责释放返回的缓冲区）
    /// @return 宽度、高度、像素数据指针（RGBA8），失败返回 {0,0,nullptr}
    struct ReadbackResult { uint32_t width = 0; uint32_t height = 0; void *pixels = nullptr; };
    ReadbackResult ReadbackSlice(uint32_t slice, DX12Engine::Renderer::D3D12DeviceContext *deviceCtx);

    /// 在指定的 CPU 描述符上为某个 slice 创建 SRV
    void CreateSliceSRV(uint32_t slice, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);

    /// 获取容量
    uint32_t GetCapacity() const { return m_capacity; }

    /// 获取已分配 slice 数量
    uint32_t GetAllocatedCount() const { return m_allocatedCount; }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_textureArray;
    ID3D12Device *m_device = nullptr;
    DX12Engine::Resource::DescriptorHeapCollection *m_descriptorHeaps = nullptr;

    uint32_t m_capacity = 0;
    uint32_t m_allocatedCount = 0;

    // 空闲 slice 列表（栈式，LIFO，提高缓存局部性）
    std::vector<uint32_t> m_freeSlices;

    // 每个 slice 的 RTV CPU 句柄（在 RTV 堆中）
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_rtvHandles;

    // 连续分配的 RTV 槽位基址（供 Shutdown 释放用）
    uint32_t m_baseRtvSlot = UINT32_MAX;

    bool m_initialized = false;
};


