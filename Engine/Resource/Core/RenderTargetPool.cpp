#include "RenderTargetPool.h"
#include "DescriptorHeapCollection.h"
#include "DescriptorSlotAllocator.h"
#include "Common/ThrowHelper.h"
#include <cassert>

using namespace DX12Engine::Resource;

namespace DX12Engine::Resource {

/**
 * @brief 初始化渲染目标池
 * @param device DX12设备指针
 * @param descriptorHeaps 描述符堆集合指针
 * @date 2026-05-24
 */
void RenderTargetPool::Initialize(ID3D12Device *device, DescriptorHeapCollection *descriptorHeaps) {
    if (m_initialized) {
        Shutdown();
    }

    m_device = device;
    m_descriptorHeaps = descriptorHeaps;
    m_initialized = true;
}

/**
 * @brief 关闭渲染目标池
 * @date 2026-05-24
 */
void RenderTargetPool::Shutdown() {
    if (!m_initialized) {
        return;
    }

    for (auto &entry : m_pool) {
        if (entry.rtvSlot != UINT32_MAX) {
            m_descriptorHeaps->Free(DescriptorHeapType::Rtv, entry.rtvSlot, UINT64_MAX);
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

/**
 * @brief 检查渲染目标描述是否匹配
 * @param a 渲染目标描述A
 * @param b 渲染目标描述B
 * @return bool
 * @date 2026-05-24
 * @attention 用于复用存在的
 */
bool RenderTargetPool::IsDescMatch(const RenderTargetDesc &a, const RenderTargetDesc &b) const {
    return a.width == b.width && a.height == b.height && a.format == b.format && a.mipLevels == b.mipLevels &&
           a.arraySize == b.arraySize && a.sampleDesc.Count == b.sampleDesc.Count &&
           a.sampleDesc.Quality == b.sampleDesc.Quality && a.flags == b.flags;
}

/**
 * @brief 查找匹配的渲染目标
 * @param desc 渲染目标描述
 * @return uint32_t
 * @date 2026-05-24
 */
uint32_t RenderTargetPool::FindMatchingEntry(const RenderTargetDesc &desc) {
    for (uint32_t i = 0; i < m_pool.size(); ++i) {
        auto &entry = m_pool[i];
        if (!entry.inUse && IsDescMatch(entry.desc, desc)) {
            return i;
        }
    }
    return UINT32_MAX;
}

/**
 * @brief 创建新的渲染目标
 * @param desc 渲染目标描述
 * @return uint32_t
 * @date 2026-05-24
 */
uint32_t RenderTargetPool::CreateNewEntry(const RenderTargetDesc &desc) {
    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = desc.width;
    resourceDesc.Height = desc.height;
    resourceDesc.DepthOrArraySize = desc.arraySize;
    resourceDesc.MipLevels = desc.mipLevels;
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
        &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_COMMON,
        desc.clearValue.Format != DXGI_FORMAT_UNKNOWN ? &desc.clearValue : nullptr, IID_PPV_ARGS(&resource));

    if (FAILED(hr)) {
        return UINT32_MAX;
    }

    uint32_t rtvSlot = m_descriptorHeaps->Allocate(DescriptorHeapType::Rtv);
    if (rtvSlot == UINT32_MAX) {
        return UINT32_MAX;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::Rtv, rtvSlot);
    m_device->CreateRenderTargetView(resource.Get(), nullptr, rtvHandle);

    uint32_t srvSlot = UINT32_MAX;
    if ((desc.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
        !(desc.flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) {
        srvSlot = m_descriptorHeaps->Allocate(DescriptorHeapType::CbvSrvUav);
        if (srvSlot != UINT32_MAX) {
            D3D12_CPU_DESCRIPTOR_HANDLE srvHandle =
                m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::CbvSrvUav, srvSlot);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = desc.format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = desc.mipLevels;
            m_device->CreateShaderResourceView(resource.Get(), &srvDesc, srvHandle);
        }
    }

    RenderTargetEntry entry;
    entry.resource = resource;
    entry.desc = desc;
    entry.rtvSlot = rtvSlot;
    entry.srvSlot = srvSlot;
    entry.lastUsedFrame = 0;
    entry.generation = m_nextGeneration++;
    entry.inUse = false;

    m_pool.push_back(std::move(entry));
    return static_cast<uint32_t>(m_pool.size() - 1);
}

/**
 * @brief 分配渲染目标
 * @param desc 渲染目标描述
 * @return RenderTargetHandle
 * @date 2026-05-24
 */
RenderTargetHandle RenderTargetPool::Allocate(const RenderTargetDesc &desc) {
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

    RenderTargetHandle handle;
    handle.poolIndex = poolIndex;
    handle.generation = entry.generation;
    handle.rtvSlot = entry.rtvSlot;
    return handle;
}

/**
 * @brief 释放渲染目标
 * @param handle 渲染目标句柄
 * @param fenceValue 围栏值
 * @date 2026-05-24
 */
void RenderTargetPool::Free(RenderTargetHandle handle, uint64_t fenceValue) {
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

/**
 * @brief 获取渲染目标的资源指针
 * @param handle 渲染目标句柄
 * @return ID3D12Resource*
 * @date 2026-05-24
 */
ID3D12Resource *RenderTargetPool::GetResource(RenderTargetHandle handle) const {
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

/**
 * @brief 获取渲染目标的RTV句柄
 * @param handle 渲染目标句柄
 * @return D3D12_CPU_DESCRIPTOR_HANDLE
 * @date 2026-05-24
 */
D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetPool::GetRtvHandle(RenderTargetHandle handle) const {
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

    return m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::Rtv, entry.rtvSlot);
}

/**
 * @brief 回收已完成的渲染目标
 * @param completedFence 已完成的围栏值
 * @date 2026-05-24
 */
void RenderTargetPool::Reclaim(uint64_t completedFence) {
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

/**
 * @brief 清理未使用的渲染目标
 * @param currentFrame 当前帧索引
 * @param maxAgeFrames 最大年龄帧数
 * @date 2026-05-24
 */
void RenderTargetPool::PurgeUnused(uint64_t currentFrame, uint64_t maxAgeFrames) {
    // BugFix: 不再从 m_pool 中 erase 条目，而是递增 generation 使旧句柄失效。
    // erase 会导致后续条目的索引前移，使得外部持有的 handle 指向错误条目。
    for (auto &entry : m_pool) {
        if (!entry.inUse && currentFrame - entry.lastUsedFrame > maxAgeFrames) {
            if (entry.rtvSlot != UINT32_MAX) {
                m_descriptorHeaps->Free(DescriptorHeapType::Rtv, entry.rtvSlot, UINT64_MAX);
                entry.rtvSlot = UINT32_MAX;
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

/**
 * @brief 获取渲染目标的SRV句柄
 * @param handle 渲染目标句柄
 * @return D3D12_CPU_DESCRIPTOR_HANDLE
 * @date 2026-05-24
 */
D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetPool::GetSrvHandle(RenderTargetHandle handle) const {
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

} // namespace DX12Engine::Resource