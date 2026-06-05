#pragma once

#include "Resource/Struct/ResourceHandle.h"
#include "Resource/Struct/TextureHandle.h"
#include <d3d12.h>
#include <string>
#include <vector>
#include <wrl/client.h>

namespace DX12Engine::Resource {

class DescriptorHeapCollection;

class TextureManager {
public:
    void Initialize(ID3D12Device *device, DescriptorHeapCollection *descriptorHeaps);
    void Shutdown();

    D3D12_GPU_DESCRIPTOR_HANDLE GetSRV(TextureHandle handle) const;
    uint32_t GetSRVIndex(TextureHandle handle) const;
    TextureHandle RegisterTexture(GpuResourceHandle gpuHandle, uint32_t srvIndex);

    void Release(TextureHandle handle, uint64_t fenceValue);
    void Reclaim(uint64_t completedFence);

    uint32_t GetActiveCount() const;
    uint32_t GetCapacity() const;
    uint32_t GetPendingReleaseCount() const;

    // 获取纹理资源（用于验证）
    ID3D12Resource *GetTexture(TextureHandle handle) const;

    // 检查纹理句柄是否有效（更详细的验证）
    bool IsValid(TextureHandle handle) const;

    // 获取纹理描述
    bool GetTextureDesc(TextureHandle handle, D3D12_RESOURCE_DESC &outDesc) const;

private:
    struct TextureEntry {
        GpuResourceHandle gpuHandle; // 引用 GPU 资源管理器中的资源
        uint32_t srvIndex = UINT32_MAX;
        uint32_t generation = 0;
        bool inUse = false;
    };

    struct PendingRelease {
        uint32_t index;
        uint32_t generation;
        uint64_t fenceValue;
    };

private:
    uint32_t AllocateEntry();
    void FreeEntry(uint32_t index);
    void ExpandCapacity();

    ID3D12Device *m_device = nullptr;
    DescriptorHeapCollection *m_descriptorHeaps = nullptr;

    std::vector<TextureEntry> m_entries;
    std::vector<uint32_t> m_freeList;
    std::vector<PendingRelease> m_pendingReleases;

    uint32_t m_nextGeneration = 1;
    uint32_t m_capacity = 0;
    bool m_initialized = false;

    static constexpr uint32_t INITIAL_CAPACITY = 256;
    static constexpr uint32_t MAX_CAPACITY = 4096;
};

} // namespace DX12Engine::Resource