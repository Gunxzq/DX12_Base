#pragma once
#include "Resource/Struct/Descriptor.h"
#include <d3d12.h>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

namespace DX12Engine::Resource {

class DescriptorHeapCollection;

enum class SamplerType : uint32_t {
    // 过滤模式 + 寻址模式组合
    PointClamp,
    PointWrap,
    PointMirror,
    LinearClamp,
    LinearWrap,
    LinearMirror,
    AnisotropicClamp,
    AnisotropicWrap,
    AnisotropicMirror,
    ShadowMap, // 深度比较采样器
    Count
};

struct SamplerDesc {
    D3D12_FILTER filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    D3D12_TEXTURE_ADDRESS_MODE addressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    D3D12_TEXTURE_ADDRESS_MODE addressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    D3D12_TEXTURE_ADDRESS_MODE addressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    float mipLODBias = 0.0f;
    UINT maxAnisotropy = 1;
    D3D12_COMPARISON_FUNC comparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    float borderColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float minLOD = 0.0f;
    float maxLOD = D3D12_FLOAT32_MAX;

    bool operator==(const SamplerDesc &other) const;
    size_t GetHash() const;
};

struct SamplerHandle {
    uint32_t slot = UINT32_MAX;
    uint32_t generation = 0;

    bool IsValid() const { return slot != UINT32_MAX; }
};

class SamplerCache {
public:
    SamplerCache() = default;
    ~SamplerCache() = default;

    SamplerCache(const SamplerCache &) = delete;
    SamplerCache &operator=(const SamplerCache &) = delete;

    void Initialize(ID3D12Device *device, DescriptorHeapCollection *descriptorHeaps);
    void Shutdown();

    SamplerHandle Get(SamplerType type);
    SamplerHandle GetOrCreate(const SamplerDesc &desc);

    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(SamplerHandle handle) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(SamplerHandle handle) const;

    void Reclaim(uint64_t completedFence);
    void PurgeUnused(uint64_t currentFrame, uint64_t maxAgeFrames);

    uint32_t GetCacheSize() const { return static_cast<uint32_t>(m_cache.size()); }

private:
    struct CachedSampler {
        SamplerDesc desc;
        uint32_t slot;
        uint32_t generation;
        uint64_t lastUsedFrame;
        bool inUse;
    };

    struct PendingFree {
        uint32_t slot;
        uint32_t generation;
        uint64_t fenceValue;
    };

private:
    uint32_t FindOrCreateSlot(const SamplerDesc &desc);

    ID3D12Device *m_device = nullptr;
    DescriptorHeapCollection *m_descriptorHeaps = nullptr;

    std::vector<CachedSampler> m_cache;
    std::unordered_map<SamplerType, uint32_t> m_presetIndices;
    std::vector<PendingFree> m_pendingFree;
    uint32_t m_nextGeneration = 1;
    uint32_t m_allocatedCount = 0;
    bool m_initialized = false;
};

} // namespace DX12Engine::Resource