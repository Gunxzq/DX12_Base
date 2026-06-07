#include "DepthStencilPool.h"
#include "DescriptorHeapCollection.h"
#include "DescriptorSlotAllocator.h"
#include "Common/ThrowHelper.h"
#include <cassert>

namespace DX12Engine::Resource {

void DepthStencilPool::Initialize(ID3D12Device *device, DescriptorHeapCollection *descriptorHeaps) {
    if (m_initialized) {
        Shutdown();
    }

    m_device = device;
    m_descriptorHeaps = descriptorHeaps;
    m_initialized = true;
}

void DepthStencilPool::Shutdown() {
    if (!m_initialized) {
        return;
    }

    for (auto &entry : m_pool) {
        if (entry.dsvSlot != UINT32_MAX) {
            m_descriptorHeaps->Free(DescriptorHeapType::Dsv, entry.dsvSlot, UINT64_MAX);
        }
        if (entry.srvSlot != UINT32_MAX) {
            m_descriptorHeaps->Free(DescriptorHeapType::CbvSrvUav, entry.srvSlot, UINT64_MAX);
        }
    }

    m_pool.clear();
    m_pendingFree.clear();
    m_allocatedCount = 0;
    m_device = nullptr;
    m_descriptorHeaps = nullptr;
    m_initialized = false;
}

bool DepthStencilPool::IsDescMatch(const DepthStencilDesc &a, const DepthStencilDesc &b) const {
    return a.width == b.width && a.height == b.height && a.format == b.format && a.arraySize == b.arraySize &&
           a.sampleDesc.Count == b.sampleDesc.Count && a.sampleDesc.Quality == b.sampleDesc.Quality &&
           a.flags == b.flags;
}

uint32_t DepthStencilPool::FindMatchingEntry(const DepthStencilDesc &desc) {
    for (uint32_t i = 0; i < m_pool.size(); ++i) {
        auto &entry = m_pool[i];
        if (!entry.inUse && IsDescMatch(entry.desc, desc)) {
            return i;
        }
    }
    return UINT32_MAX;
}

uint32_t DepthStencilPool::CreateNewEntry(const DepthStencilDesc &desc) {
    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = desc.width;
    resourceDesc.Height = desc.height;
    resourceDesc.DepthOrArraySize = desc.arraySize;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = desc.format;
    resourceDesc.SampleDesc = desc.sampleDesc;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = desc.flags;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        desc.clearValue.Format != DXGI_FORMAT_UNKNOWN ? &desc.clearValue : nullptr, IID_PPV_ARGS(&resource));

    if (FAILED(hr)) {
        return UINT32_MAX;
    }

    uint32_t dsvSlot = m_descriptorHeaps->Allocate(DescriptorHeapType::Dsv);
    if (dsvSlot == UINT32_MAX) {
        return UINT32_MAX;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::Dsv, dsvSlot);
    ThrowIfFailed(m_device->CreateDepthStencilView(resource.Get(), nullptr, dsvHandle));

    uint32_t srvSlot = UINT32_MAX;
    if (!(desc.flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) {
        srvSlot = m_descriptorHeaps->Allocate(DescriptorHeapType::CbvSrvUav);
        if (srvSlot != UINT32_MAX) {
            D3D12_CPU_DESCRIPTOR_HANDLE srvHandle =
                m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::CbvSrvUav, srvSlot);

            DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
            switch (desc.format) {
            case DXGI_FORMAT_D32_FLOAT:
                srvFormat = DXGI_FORMAT_R32_FLOAT;
                break;
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
                srvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                break;
            case DXGI_FORMAT_D16_UNORM:
                srvFormat = DXGI_FORMAT_R16_UNORM;
                break;
            default:
                srvFormat = desc.format;
                break;
            }

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = srvFormat;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            ThrowIfFailed(m_device->CreateShaderResourceView(resource.Get(), &srvDesc, srvHandle));
        }
    }

    DepthStencilEntry entry;
    entry.resource = resource;
    entry.desc = desc;
    entry.dsvSlot = dsvSlot;
    entry.lastUsedFrame = 0;
    entry.generation = m_nextGeneration++;
    entry.inUse = false;

    m_pool.push_back(std::move(entry));
    return static_cast<uint32_t>(m_pool.size() - 1);
}

DepthStencilHandle DepthStencilPool::Allocate(const DepthStencilDesc &desc) {
    if (!m_initialized) {
        return {};
    }

    uint32_t poolIndex = FindMatchingEntry(desc);
    if (poolIndex == UINT32_MAX) {
        poolIndex = CreateNewEntry(desc);
        if (poolIndex == UINT32_MAX) {
            return {};
        }
    }

    auto &entry = m_pool[poolIndex];
    entry.inUse = true;
    ++m_allocatedCount;

    DepthStencilHandle handle;
    handle.poolIndex = poolIndex;
    handle.generation = entry.generation;
    handle.dsvSlot = entry.dsvSlot;
    return handle;
}

void DepthStencilPool::Free(DepthStencilHandle handle, uint64_t fenceValue) {
    if (!m_initialized || !handle.IsValid()) {
        return;
    }

    if (handle.poolIndex >= m_pool.size()) {
        return;
    }

    auto &entry = m_pool[handle.poolIndex];
    if (entry.generation != handle.generation) {
        return;
    }

    if (!entry.inUse) {
        return;
    }

    entry.inUse = false;
    --m_allocatedCount;

    PendingFree pending;
    pending.poolIndex = handle.poolIndex;
    pending.generation = entry.generation;
    pending.fenceValue = fenceValue;
    m_pendingFree.push_back(pending);
}

ID3D12Resource *DepthStencilPool::GetResource(DepthStencilHandle handle) const {
    if (!m_initialized || !handle.IsValid()) {
        return nullptr;
    }

    if (handle.poolIndex >= m_pool.size()) {
        return nullptr;
    }

    const auto &entry = m_pool[handle.poolIndex];
    if (entry.generation != handle.generation) {
        return nullptr;
    }

    return entry.resource.Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilPool::GetDsvHandle(DepthStencilHandle handle) const {
    if (!m_initialized || !handle.IsValid()) {
        return {};
    }

    if (handle.poolIndex >= m_pool.size()) {
        return {};
    }

    const auto &entry = m_pool[handle.poolIndex];
    if (entry.generation != handle.generation) {
        return {};
    }

    return m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::Dsv, entry.dsvSlot);
}

D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilPool::GetSrvHandle(DepthStencilHandle handle) const {
    if (!handle.IsValid() || handle.poolIndex >= m_pool.size()) {
        return {};
    }
    const auto &entry = m_pool[handle.poolIndex];
    if (entry.generation != handle.generation) {
        return {};
    }
    if (entry.srvSlot == UINT32_MAX) {
        return {};
    }
    return m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::CbvSrvUav, entry.srvSlot);
}

void DepthStencilPool::Reclaim(uint64_t completedFence) {
    auto it = m_pendingFree.begin();
    while (it != m_pendingFree.end()) {
        if (completedFence >= it->fenceValue) {
            auto &entry = m_pool[it->poolIndex];
            if (entry.generation == it->generation && !entry.inUse) {
                entry.lastUsedFrame = 0;
            }
            it = m_pendingFree.erase(it);
        } else {
            ++it;
        }
    }
}

void DepthStencilPool::PurgeUnused(uint64_t currentFrame, uint64_t maxAgeFrames) {
    // BugFix: 不再从 m_pool 中 erase 条目，而是递增 generation 使旧句柄失效。
    // erase 会导致后续条目的索引前移，使得外部持有的 handle 指向错误条目。
    for (auto &entry : m_pool) {
        if (!entry.inUse && currentFrame - entry.lastUsedFrame > maxAgeFrames) {
            if (entry.dsvSlot != UINT32_MAX) {
                m_descriptorHeaps->Free(DescriptorHeapType::Dsv, entry.dsvSlot, UINT64_MAX);
                entry.dsvSlot = UINT32_MAX;
            }
            if (entry.srvSlot != UINT32_MAX) {
                m_descriptorHeaps->Free(DescriptorHeapType::CbvSrvUav, entry.srvSlot, UINT64_MAX);
                entry.srvSlot = UINT32_MAX;
            }
            entry.resource.Reset();
            // 递增 generation 使所有指向此条目的旧句柄立即失效
            entry.generation = m_nextGeneration++;
        }
    }

    // 注意：不再 erase 条目，空闲条目留在 m_pool 中等待 FindMatchingEntry 复用。
    // 被 Purge 的条目 generation 已递增，旧句柄无法通过 generation 验证，
    // 而新 Allocate 会创建新条目（或复用已有空闲条目并赋予新 generation）。
}

} // namespace DX12Engine::Resource