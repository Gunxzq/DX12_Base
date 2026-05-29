#pragma once

#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "Resource/Material/MaterialResource.h"
#include "Resource/Struct/MaterialHandle.h"
#include <unordered_map>
#include <vector>

namespace DX12Engine::Resource {

// ============================================================================
// 材质管理器 - 管理材质数据的生命周期
// ============================================================================

class MaterialManager {
public:
    MaterialManager() = default;
    ~MaterialManager() = default;
    MaterialManager(const MaterialManager &) = delete;
    MaterialManager &operator=(const MaterialManager &) = delete;

    void Initialize(uint32_t initialCapacity = 1024);
    void Shutdown();

    MaterialHandle RegisterMaterial(const MaterialData &material);
    void UpdateMaterial(MaterialHandle handle, const MaterialData &data);
    void ReleaseMaterial(MaterialHandle handle, uint64_t fenceValue);
    void Reclaim(uint64_t completedFence);

    const MaterialData *GetMaterial(MaterialHandle handle) const;
    MaterialData *GetMaterial(MaterialHandle handle);
    bool IsValid(MaterialHandle handle) const;

    //
    static Renderer::MaterialConstants ConvertToGPUConstants(const MaterialData &data);
    Renderer::MaterialConstants GetGPUConstants(MaterialHandle handle) const;
    std::vector<Renderer::MaterialConstants> GetAllGPUConstants() const;

    //  调试/统计
    uint32_t GetActiveCount() const;
    uint32_t GetCapacity() const;
    uint32_t GetPendingReleaseCount() const;

    // 根据渲染器类型 Hash 获取所有材质句柄
    std::vector<MaterialHandle> GetMaterialsByRenderer(uint64_t rendererTypeHash) const;

private:
    struct Entry {
        MaterialData data;
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

    std::vector<Entry> m_entries;
    std::vector<uint32_t> m_freeList;
    std::vector<PendingRelease> m_pendingReleases;
    std::unordered_map<uint64_t, std::vector<uint32_t>> m_rendererMap; // rendererTypeHash → 材质索引列表

    uint32_t m_nextGeneration = 1;
    uint32_t m_capacity = 0;
    bool m_initialized = false;

    static constexpr uint32_t INITIAL_CAPACITY = 1024;
    static constexpr uint32_t MAX_CAPACITY = 65536;
};

} // namespace DX12Engine::Resource