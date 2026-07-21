// MaterialManager.h
#pragma once

#include "Math/HashTypes.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "Renderer/Material/MaterialHandle.h"
#include "Renderer/Material/MaterialResource.h"
#include <unordered_map>
#include <vector>

namespace DX12Engine::Resource {

class MaterialManager {
public:
    MaterialManager() = default;
    ~MaterialManager() = default;

    void Initialize(uint32_t initialCapacity = 256);
    void Shutdown();

    // 注册材质资产，返回句柄
    MaterialHandle RegisterMaterial(const MaterialData &material);

    // 获取材质引用（refCount++）
    MaterialHandle AcquireMaterial(TypeHash materialId);

    // 释放材质引用
    void ReleaseMaterial(MaterialHandle handle);

    // 查询
    const MaterialData *GetMaterial(MaterialHandle handle) const;
    const MaterialData *GetMaterialById(TypeHash materialId) const;
    bool IsValid(MaterialHandle handle) const;

    // GPU 索引 = handle.index
    uint32_t GetGPUIndex(MaterialHandle handle) const { return handle.index; }

    // 获取所有材质的 GPU 数据列表（按 index 顺序）
    std::vector<std::pair<uint32_t, Renderer::MaterialConstants>> GetGPUMaterialList() const;
    static Renderer::MaterialConstants ConvertToGPUConstants(const MaterialData &data);

    // 脏标记
    bool IsDirty() const { return m_dirty; }
    void ClearDirty() { m_dirty = false; }

    // SRV 句柄
    void SetMaterialBufferSRV(D3D12_GPU_DESCRIPTOR_HANDLE srv) { m_materialBufferSRV = srv; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetMaterialBufferSRV() const { return m_materialBufferSRV; }

    uint32_t GetActiveCount() const { return static_cast<uint32_t>(m_entries.size()); }

private:
    struct MaterialEntry {
        MaterialData data;
        uint32_t refCount = 0;
        uint32_t generation = 1;
        bool isValid = true;
    };

    uint32_t AllocateIndex();
    void FreeIndex(uint32_t index);
    void MarkDirty() { m_dirty = true; }

    std::vector<MaterialEntry> m_entries; // index → 条目
    std::vector<uint32_t> m_freeList;     // 空闲索引

    // 映射
    std::unordered_map<TypeHash, uint32_t> m_idToIndex;                // 资产ID → index
    std::unordered_map<uint64_t, std::vector<uint32_t>> m_rendererMap; // 渲染器类型 → 材质索引列表

    D3D12_GPU_DESCRIPTOR_HANDLE m_materialBufferSRV = {0};
    bool m_dirty = false;
    bool m_initialized = false;

    static constexpr uint32_t MAX_INDICES = 0x3FFFFF; // 22 位最大值
};

} // namespace DX12Engine::Resource