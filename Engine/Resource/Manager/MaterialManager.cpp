#include "MaterialManager.h"
#include <algorithm>
#include <cassert>

namespace DX12Engine::Resource {

/**
 * @brief 初始化材质管理器
 * @param initialCapacity 初始容量
 * @date 2026-05-26
 */
void MaterialManager::Initialize(uint32_t initialCapacity) {
    if (m_initialized) {
        Shutdown();
    }

    m_capacity = std::min(initialCapacity, MAX_CAPACITY);
    m_entries.resize(m_capacity);
    m_freeList.reserve(m_capacity);
    m_rendererMap.clear();

    for (uint32_t i = 0; i < m_capacity; ++i) {
        m_freeList.push_back(m_capacity - 1 - i);
    }

    m_nextGeneration = 1;
    m_initialized = true;
}

/**
 * @brief 关闭材质管理器
 * @date 2026-05-26
 */
void MaterialManager::Shutdown() {
    if (!m_initialized)
        return;

    m_entries.clear();
    m_freeList.clear();
    m_pendingReleases.clear();
    m_rendererMap.clear();
    m_capacity = 0;
    m_nextGeneration = 1;
    m_initialized = false;
}

/**
 * @brief 注册材质
 * @param material 材质数据
 * @return MaterialHandle
 * @date 2026-05-26
 */
MaterialHandle MaterialManager::RegisterMaterial(const MaterialData &material) {
    if (!m_initialized) {
        return MaterialHandle::Invalid();
    }

    uint32_t index = AllocateEntry();
    if (index == UINT32_MAX) {
        return MaterialHandle::Invalid();
    }

    Entry &entry = m_entries[index];
    entry.data = material;
    entry.generation = m_nextGeneration++;
    entry.inUse = true;

    // 更新渲染器映射
    if (material.rendererTypeHash != 0) {
        m_rendererMap[material.rendererTypeHash].push_back(index);
    }

    MaterialHandle handle;
    handle.index = index;
    handle.generation = entry.generation;
    return handle;
}

/**
 * @brief 获取材质数据
 * @param handle 材质句柄
 * @return const MaterialData*
 * @date 2026-05-26
 */
const MaterialData *MaterialManager::GetMaterial(MaterialHandle handle) const {
    if (!IsValid(handle))
        return nullptr;
    return &m_entries[handle.index].data;
}

/**
 * @brief 获取材质数据
 * @param handle 材质句柄
 * @return MaterialData*
 * @date 2026-05-26
 */
MaterialData *MaterialManager::GetMaterial(MaterialHandle handle) {
    if (!IsValid(handle))
        return nullptr;
    return &m_entries[handle.index].data;
}

/**
 * @brief 检查材质句柄是否有效
 * @param handle 材质句柄
 * @return bool
 * @date 2026-05-26
 */
bool MaterialManager::IsValid(MaterialHandle handle) const {
    if (!m_initialized)
        return false;
    if (handle.index >= m_capacity)
        return false;
    const Entry &entry = m_entries[handle.index];
    return entry.inUse && entry.generation == handle.generation;
}

/**
 * @brief 更新材质数据
 * @param handle 材质句柄
 * @param data 材质数据
 * @date 2026-05-26
 */
void MaterialManager::UpdateMaterial(MaterialHandle handle, const MaterialData &data) {
    if (!IsValid(handle))
        return;

    Entry &entry = m_entries[handle.index];

    // 如果渲染器类型发生变化，更新映射
    if (entry.data.rendererTypeHash != data.rendererTypeHash) {
        // 从旧映射中移除
        auto it = m_rendererMap.find(entry.data.rendererTypeHash);
        if (it != m_rendererMap.end()) {
            auto &vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), handle.index), vec.end());
        }
        // 添加到新映射
        if (data.rendererTypeHash != 0) {
            m_rendererMap[data.rendererTypeHash].push_back(handle.index);
        }
    }

    entry.data = data;
}

/**
 * @brief 释放材质
 * @param handle 材质句柄
 * @param fenceValue 释放栅栏值
 * @date 2026-05-26
 */
void MaterialManager::ReleaseMaterial(MaterialHandle handle, uint64_t fenceValue) {
    if (!IsValid(handle))
        return;

    PendingRelease pending;
    pending.index = handle.index;
    pending.generation = handle.generation;
    pending.fenceValue = fenceValue;
    m_pendingReleases.push_back(pending);

    Entry &entry = m_entries[handle.index];
    entry.inUse = false;
}

void MaterialManager::Reclaim(uint64_t completedFence) {
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

/**
 * @brief 获取当前活动材质数量
 * @return uint32_t
 * @date 2026-05-26
 */
uint32_t MaterialManager::GetActiveCount() const {
    if (!m_initialized)
        return 0;
    uint32_t count = 0;
    for (const auto &entry : m_entries) {
        if (entry.inUse)
            ++count;
    }
    return count;
}

/**
 * @brief 获取当前材质管理器的容量
 * @return uint32_t
 * @date 2026-05-26
 */
uint32_t MaterialManager::GetCapacity() const { return m_capacity; }

/**
 * @brief 获取当前待释放材质数量
 * @return uint32_t
 * @date 2026-05-26
 */
uint32_t MaterialManager::GetPendingReleaseCount() const { return static_cast<uint32_t>(m_pendingReleases.size()); }

/**
 * @brief 根据渲染器类型 Hash 获取所有材质句柄
 * @param rendererTypeHash 渲染器类型 Hash
 * @return std::vector<MaterialHandle>
 * @date 2026-05-26
 */
std::vector<MaterialHandle> MaterialManager::GetMaterialsByRenderer(uint64_t rendererTypeHash) const {
    std::vector<MaterialHandle> result;
    auto it = m_rendererMap.find(rendererTypeHash);
    if (it != m_rendererMap.end()) {
        result.reserve(it->second.size());
        for (uint32_t idx : it->second) {
            MaterialHandle handle;
            handle.index = idx;
            handle.generation = m_entries[idx].generation;
            result.push_back(handle);
        }
    }
    return result;
}

/**
 * @brief 分配材质条目索引
 * @return uint32_t
 * @date 2026-05-26
 */
uint32_t MaterialManager::AllocateEntry() {
    if (m_freeList.empty()) {
        if (m_capacity >= MAX_CAPACITY)
            return UINT32_MAX;
        uint32_t newCapacity = std::min(m_capacity * 2, MAX_CAPACITY);
        if (newCapacity <= m_capacity)
            return UINT32_MAX;

        uint32_t oldCapacity = m_capacity;
        m_entries.resize(newCapacity);
        for (uint32_t i = newCapacity - 1; i >= oldCapacity; --i) {
            m_freeList.push_back(i);
        }
        m_capacity = newCapacity;
    }

    if (m_freeList.empty())
        return UINT32_MAX;

    uint32_t index = m_freeList.back();
    m_freeList.pop_back();
    return index;
}

/**
 * @brief 释放材质条目索引
 * @param index 条目索引
 * @date 2026-05-26
 */
void MaterialManager::FreeEntry(uint32_t index) {
    if (index >= m_capacity)
        return;

    Entry &entry = m_entries[index];

    // 从渲染器映射中移除
    if (entry.data.rendererTypeHash != 0) {
        auto it = m_rendererMap.find(entry.data.rendererTypeHash);
        if (it != m_rendererMap.end()) {
            auto &vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), index), vec.end());
        }
    }

    entry.data = MaterialData{};
    entry.inUse = false;
    m_freeList.push_back(index);
}

Renderer::MaterialConstants MaterialManager::GetGPUConstants(MaterialHandle handle) const {
    const MaterialData *data = GetMaterial(handle);
    if (!data) {
        return Renderer::MaterialConstants{}; // 返回默认值
    }
    return ConvertToGPUConstants(*data);
}

Renderer::MaterialConstants MaterialManager::ConvertToGPUConstants(const MaterialData &data) {
    Renderer::MaterialConstants gpu;

    // 基础颜色
    gpu.BaseColor = data.baseColor;

    // 自发光（合并颜色和强度）
    gpu.Emissive = data.emissive;

    // PBR 参数
    gpu.Metallic = data.metallic;
    gpu.Roughness = data.roughness;
    gpu.Alpha = data.alpha;
    gpu.AlphaCutoff = 0.5f; // 默认阈值

    // 纹理索引（默认 0，表示无纹理）
    gpu.BaseColorTextureIndex = 0;
    gpu.NormalTextureIndex = 0;
    gpu.MetallicRoughnessTextureIndex = 0;
    gpu.EmissiveTextureIndex = 0;
    gpu.OcclusionTextureIndex = 0;

    return gpu;
}

std::vector<Renderer::MaterialConstants> MaterialManager::GetAllGPUConstants() const {
    std::vector<Renderer::MaterialConstants> result;
    result.reserve(m_entries.size());
    for (const auto &entry : m_entries) {
        if (entry.inUse) {
            result.push_back(ConvertToGPUConstants(entry.data));
        }
    }
    return result;
}

void MaterialManager::SetMaterialBufferSRV(D3D12_GPU_DESCRIPTOR_HANDLE srvHandle) { m_materialBufferSRV = srvHandle; }

D3D12_GPU_DESCRIPTOR_HANDLE MaterialManager::GetMaterialBufferSRV() const { return m_materialBufferSRV; }

} // namespace DX12Engine::Resource