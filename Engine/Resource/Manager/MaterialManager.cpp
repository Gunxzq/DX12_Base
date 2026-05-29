// MaterialManager.cpp
#include "MaterialManager.h"
#include <algorithm>

namespace DX12Engine::Resource {

void MaterialManager::Initialize(uint32_t initialCapacity) {
    if (m_initialized)
        Shutdown();

    m_entries.reserve(initialCapacity);
    m_idToIndex.clear();
    m_rendererMap.clear();
    m_freeList.clear();
    m_dirty = false;
    m_initialized = true;
}

void MaterialManager::Shutdown() {
    if (!m_initialized)
        return;
    m_entries.clear();
    m_idToIndex.clear();
    m_rendererMap.clear();
    m_freeList.clear();
    m_initialized = false;
}

uint32_t MaterialManager::AllocateIndex() {
    if (!m_freeList.empty()) {
        uint32_t idx = m_freeList.back();
        m_freeList.pop_back();
        return idx;
    }
    uint32_t idx = static_cast<uint32_t>(m_entries.size());
    if (idx >= MAX_INDICES)
        return UINT32_MAX;
    m_entries.emplace_back();
    return idx;
}

void MaterialManager::FreeIndex(uint32_t index) {
    if (index < m_entries.size()) {
        m_freeList.push_back(index);
    }
}

MaterialHandle MaterialManager::RegisterMaterial(const MaterialData &material) {
    if (!m_initialized || material.materialId == 0) {
        return MaterialHandle::Invalid();
    }

    // 检查是否已存在
    auto it = m_idToIndex.find(material.materialId);
    if (it != m_idToIndex.end()) {
        uint32_t idx = it->second;
        auto &entry = m_entries[idx];
        entry.data = material;
        entry.generation++;
        entry.isValid = true;
        MarkDirty();
        return MaterialHandle{idx, entry.generation};
    }

    // 分配新索引
    uint32_t idx = AllocateIndex();
    if (idx == UINT32_MAX)
        return MaterialHandle::Invalid();

    m_entries[idx] = {material, 0, 1, true};
    m_idToIndex[material.materialId] = idx;

    if (material.rendererTypeHash != 0) {
        m_rendererMap[material.rendererTypeHash].push_back(idx);
    }

    MarkDirty();
    return MaterialHandle{idx, 1};
}

MaterialHandle MaterialManager::AcquireMaterial(TypeHash materialId) {
    auto it = m_idToIndex.find(materialId);
    if (it == m_idToIndex.end())
        return MaterialHandle::Invalid();

    uint32_t idx = it->second;
    auto &entry = m_entries[idx];
    if (!entry.isValid)
        return MaterialHandle::Invalid();

    entry.refCount++;
    return MaterialHandle{idx, entry.generation};
}

void MaterialManager::ReleaseMaterial(MaterialHandle handle) {
    if (!handle.IsValid())
        return;

    uint32_t idx = handle.index;
    if (idx >= m_entries.size())
        return;

    auto &entry = m_entries[idx];
    if (entry.generation != handle.generation)
        return; // 句柄过期
    if (entry.refCount == 0)
        return;

    entry.refCount--;

    if (entry.refCount == 0) {
        // 清理映射
        m_idToIndex.erase(entry.data.materialId);

        // 清理渲染器映射
        if (entry.data.rendererTypeHash != 0) {
            auto &vec = m_rendererMap[entry.data.rendererTypeHash];
            vec.erase(std::remove(vec.begin(), vec.end(), idx), vec.end());
        }

        entry.isValid = false;
        FreeIndex(idx);
        MarkDirty();
    }
}

const MaterialData *MaterialManager::GetMaterial(MaterialHandle handle) const {
    if (!handle.IsValid())
        return nullptr;
    if (handle.index >= m_entries.size())
        return nullptr;

    const auto &entry = m_entries[handle.index];
    if (!entry.isValid || entry.generation != handle.generation)
        return nullptr;

    return &entry.data;
}

const MaterialData *MaterialManager::GetMaterialById(TypeHash materialId) const {
    auto it = m_idToIndex.find(materialId);
    if (it == m_idToIndex.end())
        return nullptr;
    return &m_entries[it->second].data;
}

bool MaterialManager::IsValid(MaterialHandle handle) const {
    if (!handle.IsValid())
        return false;
    if (handle.index >= m_entries.size())
        return false;
    const auto &entry = m_entries[handle.index];
    return entry.isValid && entry.generation == handle.generation;
}

std::vector<std::pair<uint32_t, Renderer::MaterialConstants>> MaterialManager::GetGPUMaterialList() const {
    // 找出最大有效索引
    uint32_t maxIndex = 0;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].isValid) {
            maxIndex = std::max(maxIndex, static_cast<uint32_t>(i));
        }
    }

    // 预留空洞位置，用默认值填充
    std::vector<std::pair<uint32_t, Renderer::MaterialConstants>> result;
    result.reserve(maxIndex + 1);

    for (uint32_t i = 0; i <= maxIndex; ++i) {
        if (i < m_entries.size() && m_entries[i].isValid) {
            result.emplace_back(i, ConvertToGPUConstants(m_entries[i].data));
        } else {
            // 空洞填默认值
            result.push_back({i, Renderer::MaterialConstants{}});
        }
    }

    return result;
}

Renderer::MaterialConstants MaterialManager::ConvertToGPUConstants(const MaterialData &data) {
    Renderer::MaterialConstants gpu;

    gpu.BaseColor = data.baseColor;
    gpu.Emissive = data.emissive;
    gpu.Metallic = data.metallic;
    gpu.Roughness = data.roughness;
    gpu.Ambient = data.ambient;
    gpu.Alpha = data.alpha;
    gpu.AlphaCutoff = data.alphaCutoff;

    // 纹理索引
    gpu.BaseColorTextureIndex = data.baseColorTextureId;
    gpu.NormalTextureIndex = data.normalTextureId;
    gpu.MetallicRoughnessTextureIndex = data.metallicRoughnessTextureId;
    gpu.EmissiveTextureIndex = data.emissiveTextureId;
    gpu.OcclusionTextureIndex = data.occlusionTextureId;

    return gpu;
}

} // namespace DX12Engine::Resource