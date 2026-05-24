#pragma once
#include "Resource/Struct/Descriptor.h"
#include "Resource/Struct/DescriptorHandle.h"
#include "Resource/Struct/ResourceHandle.h"
#include <d3d12.h>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

namespace DX12Engine::Resource {

class DescriptorHeapCollection;
struct RenderTargetDesc;
struct RenderTargetHandle;

class RenderTargetPool {
public:
    RenderTargetPool() = default;
    ~RenderTargetPool() = default;

    RenderTargetPool(const RenderTargetPool &) = delete;
    RenderTargetPool &operator=(const RenderTargetPool &) = delete;

    void Initialize(ID3D12Device *device, DescriptorHeapCollection *descriptorHeaps);
    void Shutdown();

    RenderTargetHandle Allocate(const RenderTargetDesc &desc);
    void Free(RenderTargetHandle handle, uint64_t fenceValue);

    ID3D12Resource *GetResource(RenderTargetHandle handle) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetRtvHandle(RenderTargetHandle handle) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetSrvHandle(RenderTargetHandle handle) const;

    void Reclaim(uint64_t completedFence);
    void PurgeUnused(uint64_t currentFrame, uint64_t maxAgeFrames);

    uint32_t GetPoolSize() const { return static_cast<uint32_t>(m_pool.size()); }
    uint32_t GetAllocatedCount() const { return m_allocatedCount; }

private:
    struct RenderTargetEntry {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        RenderTargetDesc desc;
        uint32_t rtvSlot = UINT32_MAX;
        uint32_t srvSlot = UINT32_MAX;
        uint64_t lastUsedFrame = 0;
        bool inUse = false;
        uint32_t generation = 0;
    };

    struct PendingFree {
        uint32_t poolIndex;
        uint32_t generation;
        uint64_t fenceValue;
    };

private:
    uint32_t FindMatchingEntry(const RenderTargetDesc &desc);
    uint32_t CreateNewEntry(const RenderTargetDesc &desc);
    bool IsDescMatch(const RenderTargetDesc &a, const RenderTargetDesc &b) const;

    ID3D12Device *m_device = nullptr;
    DescriptorHeapCollection *m_descriptorHeaps = nullptr;

    std::vector<RenderTargetEntry> m_pool;
    std::vector<PendingFree> m_pendingFree;
    uint32_t m_allocatedCount = 0;
    uint32_t m_nextGeneration = 1;
    bool m_initialized = false;
};

} // namespace DX12Engine::Resource