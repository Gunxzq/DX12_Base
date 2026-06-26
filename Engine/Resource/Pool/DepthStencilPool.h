#pragma once
#include "Resource/Struct/Descriptor.h"
#include "Resource/Struct/DescriptorHandle.h"
#include "Resource/Struct/ResourceHandle.h"
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

namespace DX12Engine::Resource {

class DescriptorHeapCollection;
struct DepthStencilDesc;
struct DepthStencilHandle;

class DepthStencilPool {
public:
    static DepthStencilPool &GetInstance();

    DepthStencilPool(const DepthStencilPool &) = delete;
    DepthStencilPool &operator=(const DepthStencilPool &) = delete;

    void Initialize(ID3D12Device *device, DescriptorHeapCollection *descriptorHeaps);
    void Shutdown();

    DepthStencilHandle Allocate(const DepthStencilDesc &desc, const D3D12_DEPTH_STENCIL_VIEW_DESC *dsvDesc = nullptr);
    void Free(DepthStencilHandle handle, uint64_t fenceValue);

    ID3D12Resource *GetResource(DepthStencilHandle handle) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetDsvHandle(DepthStencilHandle handle) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetSrvHandle(DepthStencilHandle handle) const;

    void Reclaim(uint64_t completedFence);
    void PurgeUnused(uint64_t currentFrame, uint64_t maxAgeFrames);

    uint32_t GetPoolSize() const { return static_cast<uint32_t>(m_pool.size()); }
    uint32_t GetAllocatedCount() const { return m_allocatedCount; }

private:
    DepthStencilPool() = default;
    ~DepthStencilPool() = default;

    struct DepthStencilEntry {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        DepthStencilDesc desc;
        uint32_t dsvSlot = UINT32_MAX;
        uint32_t srvSlot = UINT32_MAX;
        uint64_t lastUsedFrame = 0;
        uint32_t generation = 0;
        bool inUse = false;
    };

    struct PendingFree {
        uint32_t poolIndex;
        uint32_t generation;
        uint64_t fenceValue;
    };

private:
    uint32_t FindMatchingEntry(const DepthStencilDesc &desc);
    uint32_t CreateNewEntry(const DepthStencilDesc &desc, const D3D12_DEPTH_STENCIL_VIEW_DESC *dsvDesc);
    bool IsDescMatch(const DepthStencilDesc &a, const DepthStencilDesc &b) const;

    ID3D12Device *m_device = nullptr;
    DescriptorHeapCollection *m_descriptorHeaps = nullptr;

    std::vector<DepthStencilEntry> m_pool;
    std::vector<PendingFree> m_pendingFree;
    uint32_t m_allocatedCount = 0;
    uint32_t m_nextGeneration = 1;
    bool m_initialized = false;
};

} // namespace DX12Engine::Resource