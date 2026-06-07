#include "SamplerCache.h"
#include "DescriptorHeapCollection.h"
#include "DescriptorSlotAllocator.h"
#include <cstring>

namespace DX12Engine::Resource {

static SamplerDesc MakePresetDesc(SamplerType type) {
    SamplerDesc desc;
    switch (type) {
    case SamplerType::PointClamp:
        desc.filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        desc.addressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.addressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.addressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        break;
    case SamplerType::PointWrap:
        desc.filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        desc.addressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        desc.addressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        desc.addressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        break;
    case SamplerType::PointMirror:
        desc.filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        desc.addressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        desc.addressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        desc.addressW = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        break;
    case SamplerType::LinearClamp:
        desc.filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        desc.addressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.addressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.addressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        break;
    case SamplerType::LinearWrap:
        desc.filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        desc.addressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        desc.addressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        desc.addressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        break;
    case SamplerType::LinearMirror:
        desc.filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        desc.addressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        desc.addressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        desc.addressW = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        break;
    case SamplerType::AnisotropicClamp:
        desc.filter = D3D12_FILTER_ANISOTROPIC;
        desc.addressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.addressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.addressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.maxAnisotropy = 16;
        break;
    case SamplerType::AnisotropicWrap:
        desc.filter = D3D12_FILTER_ANISOTROPIC;
        desc.addressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        desc.addressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        desc.addressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        desc.maxAnisotropy = 16;
        break;
    case SamplerType::AnisotropicMirror:
        desc.filter = D3D12_FILTER_ANISOTROPIC;
        desc.addressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        desc.addressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        desc.addressW = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        desc.maxAnisotropy = 16;
        break;
    case SamplerType::ShadowMap:
        desc.filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        desc.addressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        desc.addressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        desc.addressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        desc.comparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        desc.borderColor[0] = 1.0f;
        desc.borderColor[1] = 1.0f;
        desc.borderColor[2] = 1.0f;
        desc.borderColor[3] = 1.0f;
        break;
    default:
        break;
    }
    return desc;
}

bool SamplerDesc::operator==(const SamplerDesc &other) const {
    return filter == other.filter && addressU == other.addressU && addressV == other.addressV &&
           addressW == other.addressW && mipLODBias == other.mipLODBias && maxAnisotropy == other.maxAnisotropy &&
           comparisonFunc == other.comparisonFunc && minLOD == other.minLOD && maxLOD == other.maxLOD &&
           memcmp(borderColor, other.borderColor, sizeof(borderColor)) == 0;
}

size_t SamplerDesc::GetHash() const {
    size_t hash = 0;
    hash = hash * 31 + static_cast<size_t>(filter);
    hash = hash * 31 + static_cast<size_t>(addressU);
    hash = hash * 31 + static_cast<size_t>(addressV);
    hash = hash * 31 + static_cast<size_t>(addressW);
    hash = hash * 31 + *reinterpret_cast<const size_t *>(&mipLODBias);
    hash = hash * 31 + maxAnisotropy;
    hash = hash * 31 + static_cast<size_t>(comparisonFunc);
    hash = hash * 31 + *reinterpret_cast<const size_t *>(&minLOD);
    hash = hash * 31 + *reinterpret_cast<const size_t *>(&maxLOD);
    for (int i = 0; i < 4; ++i) {
        hash = hash * 31 + *reinterpret_cast<const size_t *>(&borderColor[i]);
    }
    return hash;
}

void SamplerCache::Initialize(ID3D12Device *device, DescriptorHeapCollection *descriptorHeaps) {
    if (m_initialized) {
        Shutdown();
    }

    m_device = device;
    m_descriptorHeaps = descriptorHeaps;

    for (uint32_t i = 0; i < static_cast<uint32_t>(SamplerType::Count); ++i) {
        SamplerType type = static_cast<SamplerType>(i);
        SamplerDesc desc = MakePresetDesc(type);
        uint32_t slot = FindOrCreateSlot(desc);
        if (slot != UINT32_MAX) {
            m_presetIndices[type] = slot;
        }
    }

    m_initialized = true;
}

void SamplerCache::Shutdown() {
    if (!m_initialized) {
        return;
    }

    for (auto &entry : m_cache) {
        if (entry.slot != UINT32_MAX) {
            m_descriptorHeaps->Free(DescriptorHeapType::Sampler, entry.slot, UINT64_MAX);
        }
    }

    m_cache.clear();
    m_presetIndices.clear();
    m_pendingFree.clear();
    m_device = nullptr;
    m_descriptorHeaps = nullptr;
    m_initialized = false;
}

uint32_t SamplerCache::FindOrCreateSlot(const SamplerDesc &desc) {
    for (uint32_t i = 0; i < m_cache.size(); ++i) {
        if (!m_cache[i].inUse && m_cache[i].desc == desc) {
            m_cache[i].inUse = true;
            m_cache[i].lastUsedFrame = 0;
            ++m_allocatedCount;
            return i;
        }
    }

    uint32_t slot = m_descriptorHeaps->Allocate(DescriptorHeapType::Sampler);
    if (slot == UINT32_MAX) {
        return UINT32_MAX;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::Sampler, slot);
    D3D12_SAMPLER_DESC d3dDesc = {};
    d3dDesc.Filter = desc.filter;
    d3dDesc.AddressU = desc.addressU;
    d3dDesc.AddressV = desc.addressV;
    d3dDesc.AddressW = desc.addressW;
    d3dDesc.MipLODBias = desc.mipLODBias;
    d3dDesc.MaxAnisotropy = desc.maxAnisotropy;
    d3dDesc.ComparisonFunc = desc.comparisonFunc;
    memcpy(d3dDesc.BorderColor, desc.borderColor, sizeof(d3dDesc.BorderColor));
    d3dDesc.MinLOD = desc.minLOD;
    d3dDesc.MaxLOD = desc.maxLOD;
    m_device->CreateSampler(&d3dDesc, cpuHandle);

    CachedSampler entry;
    entry.desc = desc;
    entry.slot = slot;
    entry.generation = m_nextGeneration++;
    entry.lastUsedFrame = 0;
    entry.inUse = true;

    m_cache.push_back(entry);
    ++m_allocatedCount;
    return static_cast<uint32_t>(m_cache.size() - 1);
}

SamplerHandle SamplerCache::Get(SamplerType type) {
    auto it = m_presetIndices.find(type);
    if (it == m_presetIndices.end()) {
        return {};
    }

    uint32_t index = it->second;
    if (index >= m_cache.size()) {
        return {};
    }

    auto &entry = m_cache[index];
    entry.inUse = true;
    entry.lastUsedFrame = 0;

    SamplerHandle handle;
    handle.slot = entry.slot;
    handle.generation = entry.generation;
    return handle;
}

SamplerHandle SamplerCache::GetOrCreate(const SamplerDesc &desc) {
    uint32_t index = FindOrCreateSlot(desc);
    if (index == UINT32_MAX) {
        return {};
    }

    auto &entry = m_cache[index];
    SamplerHandle handle;
    handle.slot = entry.slot;
    handle.generation = entry.generation;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE SamplerCache::GetCpuHandle(SamplerHandle handle) const {
    // BugFix: 添加 generation 验证，防止 PurgeUnused 后旧句柄访问错误条目
    if (!handle.IsValid() || handle.slot >= m_cache.size()) {
        return {};
    }
    const auto &entry = m_cache[handle.slot];
    if (entry.generation != handle.generation) {
        return {};
    }
    return m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::Sampler, handle.slot);
}

D3D12_GPU_DESCRIPTOR_HANDLE SamplerCache::GetGpuHandle(SamplerHandle handle) const {
    // BugFix: 添加 generation 验证，防止 PurgeUnused 后旧句柄访问错误条目
    if (!handle.IsValid() || handle.slot >= m_cache.size()) {
        return {};
    }
    const auto &entry = m_cache[handle.slot];
    if (entry.generation != handle.generation) {
        return {};
    }
    return m_descriptorHeaps->GetGpuHandle(DescriptorHeapType::Sampler, handle.slot);
}

void SamplerCache::Reclaim(uint64_t completedFence) {
    auto it = m_pendingFree.begin();
    while (it != m_pendingFree.end()) {
        if (completedFence >= it->fenceValue) {
            if (it->slot < m_cache.size()) {
                auto &entry = m_cache[it->slot];
                if (entry.generation == it->generation && !entry.inUse) {
                    entry.lastUsedFrame = 0;
                }
            }
            it = m_pendingFree.erase(it);
        } else {
            ++it;
        }
    }
}

void SamplerCache::PurgeUnused(uint64_t currentFrame, uint64_t maxAgeFrames) {
    // BugFix: 不再从 m_cache 中 erase 条目，而是递增 generation 使旧句柄失效。
    // erase 会导致后续条目的索引前移，使得外部持有的 handle（如 m_presetIndices）指向错误条目。
    for (auto &entry : m_cache) {
        if (!entry.inUse && currentFrame - entry.lastUsedFrame > maxAgeFrames) {
            m_descriptorHeaps->Free(DescriptorHeapType::Sampler, entry.slot, UINT64_MAX);
            entry.slot = UINT32_MAX;
            // 递增 generation 使所有指向此条目的旧句柄立即失效
            entry.generation = m_nextGeneration++;
        }
    }

    // 注意：不再 erase 条目。被 Purge 的条目 generation 已递增，旧句柄无法通过
    // generation 验证（GetCpuHandle/GetGpuHandle 已添加 generation 检查）。
    // 空闲条目（slot==UINT32_MAX, inUse==false）留在 m_cache 中等待 FindOrCreateSlot 复用。
}

} // namespace DX12Engine::Resource