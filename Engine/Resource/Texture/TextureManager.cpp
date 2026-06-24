#include "TextureManager.h"
#include "Common/d3dUtil.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include <algorithm>
#include <cassert>
#include <d3dcompiler.h>

namespace DX12Engine::Resource {

void TextureManager::Initialize(ID3D12Device *device, DescriptorHeapCollection *descriptorHeaps) {
    if (m_initialized) {
        Shutdown();
        for (uint32_t i = 0; i < m_capacity; ++i) {
            m_freeList.push_back(m_capacity - 1 - i);
        }
    }

    m_device = device;
    m_descriptorHeaps = descriptorHeaps;

    m_capacity = INITIAL_CAPACITY;
    m_entries.resize(m_capacity);
    m_freeList.reserve(m_capacity);

    for (uint32_t i = 0; i < m_capacity; ++i) {
        m_freeList.push_back(m_capacity - 1 - i);
    }

    m_nextGeneration = 1;
    m_initialized = true;
}

void TextureManager::Shutdown() {
    if (!m_initialized) {
        return;
    }

    for (auto &entry : m_entries) {
        if (entry.srvIndex != UINT32_MAX && m_descriptorHeaps) {
            m_descriptorHeaps->Free(DescriptorHeapType::CbvSrvUav, entry.srvIndex, UINT64_MAX);
        }
    }

    m_entries.clear();
    m_freeList.clear();
    m_pendingReleases.clear();
    m_capacity = 0;
    m_nextGeneration = 1;
    m_device = nullptr;
    m_descriptorHeaps = nullptr;
    m_initialized = false;
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetSRV(TextureHandle handle) const {
    if (!handle.IsValid() || handle.index >= m_capacity) {
        if (handle.IsValid()) {
            char msg[256];
            sprintf_s(msg, "[WARN] TextureManager::GetSRV: handle invalid or index out of range. index=%u cap=%u\n",
                      handle.index, m_capacity);
            OutputDebugStringA(msg);
        }
        return {};
    }

    const TextureEntry &entry = m_entries[handle.index];
    if (!entry.inUse || entry.generation != handle.generation) {
        char msg[256];
        sprintf_s(msg,
                  "[WARN] TextureManager::GetSRV: entry not in use or generation mismatch. "
                  "index=%u inUse=%d entryGen=%u handleGen=%u\n",
                  handle.index, entry.inUse, entry.generation, handle.generation);
        OutputDebugStringA(msg);
        return {};
    }

    if (entry.srvIndex == UINT32_MAX || !m_descriptorHeaps) {
        char msg[256];
        sprintf_s(msg, "[WARN] TextureManager::GetSRV: srvIndex invalid or descriptorHeaps null. srvIndex=%u\n",
                  entry.srvIndex);
        OutputDebugStringA(msg);
        return {};
    }

    return m_descriptorHeaps->GetGpuHandle(DescriptorHeapType::CbvSrvUav, entry.srvIndex);
}

/**
 * @brief 获取纹理 SRV 索引
 * @param handle 纹理句柄
 * @return uint32_t SRV 索引
 * @note 用于无界纹理数组 gSharedTextures[TexIndex]
 * @date 2026-06-05
 */
uint32_t TextureManager::GetSRVIndex(TextureHandle handle) const {
    if (!handle.IsValid() || handle.index >= m_capacity) {
        return UINT32_MAX;
    }

    const TextureEntry &entry = m_entries[handle.index];
    if (!entry.inUse || entry.generation != handle.generation) {
        return UINT32_MAX;
    }

    return entry.srvIndex;
}

void TextureManager::Release(TextureHandle handle, uint64_t fenceValue) {
    if (!handle.IsValid() || handle.index >= m_capacity) {
        return;
    }

    TextureEntry &entry = m_entries[handle.index];
    if (!entry.inUse || entry.generation != handle.generation) {
        return;
    }

    entry.inUse = false;

    PendingRelease pending;
    pending.index = handle.index;
    pending.generation = handle.generation;
    pending.fenceValue = fenceValue;
    m_pendingReleases.push_back(pending);
}

void TextureManager::Reclaim(uint64_t completedFence) {
    auto it = m_pendingReleases.begin();
    while (it != m_pendingReleases.end()) {
        if (completedFence >= it->fenceValue) {
            FreeEntry(it->index);
            it = m_pendingReleases.erase(it);
        } else {
            ++it;
        }
    }
}

uint32_t TextureManager::GetActiveCount() const {
    if (!m_initialized) {
        return 0;
    }

    uint32_t count = 0;
    for (const auto &entry : m_entries) {
        if (entry.inUse) {
            ++count;
        }
    }
    return count;
}

uint32_t TextureManager::GetCapacity() const { return m_capacity; }

uint32_t TextureManager::GetPendingReleaseCount() const { return static_cast<uint32_t>(m_pendingReleases.size()); }

uint32_t TextureManager::AllocateEntry() {
    if (m_freeList.empty()) {
        if (m_capacity >= MAX_CAPACITY) {
            return UINT32_MAX;
        }

        uint32_t newCapacity = std::min(m_capacity * 2, MAX_CAPACITY);
        if (newCapacity <= m_capacity) {
            return UINT32_MAX;
        }

        uint32_t oldCapacity = m_capacity;
        m_entries.resize(newCapacity);

        for (uint32_t i = newCapacity - 1; i >= oldCapacity; --i) {
            m_freeList.push_back(i);
        }

        m_capacity = newCapacity;
    }

    if (m_freeList.empty()) {
        return UINT32_MAX;
    }

    uint32_t index = m_freeList.back();
    m_freeList.pop_back();
    return index;
}

void TextureManager::FreeEntry(uint32_t index) {
    if (index >= m_capacity) {
        return;
    }

    TextureEntry &entry = m_entries[index];

    if (entry.srvIndex != UINT32_MAX && m_descriptorHeaps) {
        m_descriptorHeaps->Free(DescriptorHeapType::CbvSrvUav, entry.srvIndex, UINT64_MAX);
    }

    entry.srvIndex = UINT32_MAX;
    entry.generation = 0;
    entry.inUse = false;

    m_freeList.push_back(index);
}

void TextureManager::ExpandCapacity() {}

ID3D12Resource *TextureManager::GetTexture(TextureHandle handle) const {
    if (!handle.IsValid() || handle.index >= m_capacity) {
        return nullptr;
    }

    const TextureEntry &entry = m_entries[handle.index];
    if (!entry.inUse || entry.generation != handle.generation) {
        return nullptr;
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();
    return gpuMgr.GetResource(entry.gpuHandle);
}

bool TextureManager::IsValid(TextureHandle handle) const {
    if (!handle.IsValid() || handle.index >= m_capacity) {
        return false;
    }

    const TextureEntry &entry = m_entries[handle.index];
    return entry.inUse && entry.generation == handle.generation;
}

bool TextureManager::GetTextureDesc(TextureHandle handle, D3D12_RESOURCE_DESC &outDesc) const {
    ID3D12Resource *resource = GetTexture(handle);
    if (!resource) {
        return false;
    }

    outDesc = resource->GetDesc();
    return true;
}

GpuResourceHandle TextureManager::GetGpuHandle(TextureHandle handle) const {

    if (!handle.IsValid() || handle.index >= m_capacity)
        return {};
    const auto &entry = m_entries[handle.index];
    if (!entry.inUse || entry.generation != handle.generation)
        return {};
    return entry.gpuHandle;
}

TextureHandle TextureManager::RegisterTexture(GpuResourceHandle gpuHandle, uint32_t srvIndex) {
    if (!m_initialized || !gpuHandle.IsValid() || srvIndex == UINT32_MAX) {
        return TextureHandle::Invalid();
    }

    uint32_t index = AllocateEntry();
    if (index == UINT32_MAX) {
        return TextureHandle::Invalid();
    }

    TextureEntry &entry = m_entries[index];

    entry.gpuHandle = gpuHandle;
    entry.srvIndex = srvIndex;
    entry.generation = m_nextGeneration++;
    entry.inUse = true;

    TextureHandle handle;
    handle.index = index;
    handle.generation = entry.generation;
    return handle;
}

} // namespace DX12Engine::Resource