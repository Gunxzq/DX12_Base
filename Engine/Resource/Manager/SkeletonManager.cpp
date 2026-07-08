#include "SkeletonManager.h"
#include "Common/Common.h"
#include <algorithm>
#include <cassert>
#include <fstream>
#include <sstream>

namespace DX12Engine::Resource {

void SkeletonManager::Initialize(uint32_t initialCapacity) {
    if (m_initialized) {
        Shutdown();
    }

    m_capacity = std::min(initialCapacity, MAX_CAPACITY);
    m_entries.resize(m_capacity);
    m_freeList.reserve(m_capacity);

    for (uint32_t i = 0; i < m_capacity; ++i) {
        m_freeList.push_back(m_capacity - 1 - i);
    }

    m_nextGeneration = 1;
    m_initialized = true;
}

void SkeletonManager::Shutdown() {
    if (!m_initialized) {
        return;
    }

    m_entries.clear();
    m_freeList.clear();
    m_pendingReleases.clear();
    m_capacity = 0;
    m_nextGeneration = 1;
    m_initialized = false;
}

// ============================================================================
// 内部：分配/释放条目
// ============================================================================

uint32_t SkeletonManager::AllocateEntry() {
    if (m_freeList.empty()) {
        // 扩容
        uint32_t newCapacity = std::min(m_capacity * 2, MAX_CAPACITY);
        if (newCapacity <= m_capacity) {
            return UINT32_MAX;
        }

        m_entries.resize(newCapacity);
        for (uint32_t i = newCapacity - 1; i >= m_capacity; --i) {
            m_freeList.push_back(i);
        }
        m_capacity = newCapacity;
    }

    uint32_t index = m_freeList.back();
    m_freeList.pop_back();
    return index;
}

void SkeletonManager::FreeEntry(uint32_t index) {
    m_entries[index].inUse = false;
    m_entries[index].data = SkeletonData();
    m_freeList.push_back(index);
}

// ============================================================================
// 注册
// ============================================================================

SkeletonHandle SkeletonManager::RegisterSkeleton(const SkeletonData &data) {
    if (!m_initialized) {
        OutputDebugStringW(L"[SkeletonManager] RegisterSkeleton FAILED: not initialized\n");
        return SkeletonHandle::Invalid();
    }

    uint32_t index = AllocateEntry();
    if (index == UINT32_MAX) {
        OutputDebugStringW(L"[SkeletonManager] RegisterSkeleton FAILED: AllocateEntry returned UINT32_MAX\n");
        return SkeletonHandle::Invalid();
    }

    Entry &entry = m_entries[index];
    entry.data = data;
    entry.generation = m_nextGeneration++;
    entry.refCount = 1; // 首次注册引用计数为 1
    entry.inUse = true;

    SkeletonHandle handle;
    handle.index = index;
    handle.generation = entry.generation & 0x3FF;

    wchar_t dbg[128];
    swprintf_s(dbg, L"[SkeletonManager] Registered skeleton at index=%d gen=%d bones=%zu\n", index, handle.generation,
               data.BoneCount());
    OutputDebugStringW(dbg);
    return handle;
}

struct M3dHeader {
    uint32_t numMaterials;
    uint32_t numVertices;
    uint32_t numTriangles;
    uint32_t numBones;
    uint32_t numAnimationClips;
};

static bool ReadM3dHeader(std::ifstream &fin, M3dHeader &header) {
    std::string ignore;
    fin >> ignore; // file header text
    fin >> ignore >> header.numMaterials;
    fin >> ignore >> header.numVertices;
    fin >> ignore >> header.numTriangles;
    fin >> ignore >> header.numBones;
    fin >> ignore >> header.numAnimationClips;
    return fin.good();
}

static void ReadBoneKeyframes(std::ifstream &fin, BoneAnimation &boneAnimation) {
    std::string ignore;
    uint32_t numKeyframes = 0;
    fin >> ignore >> ignore >> numKeyframes;
    fin >> ignore; // {

    boneAnimation.Keyframes.resize(numKeyframes);
    for (uint32_t i = 0; i < numKeyframes; ++i) {
        float t = 0.0f;
        DirectX::XMFLOAT3 p(0.0f, 0.0f, 0.0f);
        DirectX::XMFLOAT3 s(1.0f, 1.0f, 1.0f);
        DirectX::XMFLOAT4 q(0.0f, 0.0f, 0.0f, 1.0f);

        fin >> ignore >> t;
        fin >> ignore >> p.x >> p.y >> p.z;
        fin >> ignore >> s.x >> s.y >> s.z;
        fin >> ignore >> q.x >> q.y >> q.z >> q.w;

        boneAnimation.Keyframes[i].TimePos = t;
        boneAnimation.Keyframes[i].Translation = p;
        boneAnimation.Keyframes[i].Scale = s;
        boneAnimation.Keyframes[i].RotationQuat = q;
    }
    fin >> ignore; // }
}

SkeletonHandle SkeletonManager::LoadFromM3d(const std::string &filepath) {
    if (!m_initialized) {
        return SkeletonHandle::Invalid();
    }

    std::ifstream fin(filepath, std::ios::binary);
    if (!fin) {
        return SkeletonHandle::Invalid();
    }

    M3dHeader header;
    if (!ReadM3dHeader(fin, header)) {
        return SkeletonHandle::Invalid();
    }

    if (header.numBones == 0) {
        return SkeletonHandle::Invalid(); // 不是蒙皮模型
    }

    SkeletonData data;
    data.BoneHierarchy.resize(header.numBones);
    data.BoneOffsets.resize(header.numBones);
    data.BoneNames.resize(header.numBones);

    // 跳过材质和网格数据（定位到 BoneOffsets）
    std::string ignore;
    for (uint32_t i = 0; i < header.numMaterials; ++i) {
        std::string matName, diffuseMap, normalMap;
        float r, g, b, roughness;
        bool alphaClip;
        fin >> ignore >> matName;
        fin >> ignore >> r >> g >> b;
        fin >> ignore >> r >> g >> b;
        fin >> ignore >> roughness;
        fin >> ignore >> alphaClip;
        fin >> ignore >> matName; // MaterialTypeName
        fin >> ignore >> diffuseMap;
        fin >> ignore >> normalMap;
    }

    // 跳过 SubsetTable
    fin >> ignore; // SubsetTable header
    for (uint32_t i = 0; i < header.numMaterials; ++i) {
        uint32_t id, vStart, vCount, fStart, fCount;
        fin >> ignore >> id;
        fin >> ignore >> vStart;
        fin >> ignore >> vCount;
        fin >> ignore >> fStart;
        fin >> ignore >> fCount;
    }

    // 跳过顶点
    fin >> ignore; // Vertices header
    for (uint32_t i = 0; i < header.numVertices; ++i) {
        float x, y, z;
        std::string dummy;
        fin >> ignore >> x >> y >> z;          // Pos
        fin >> ignore >> x >> y >> z >> dummy; // TangentU + w
        fin >> ignore >> x >> y >> z;          // Normal
        fin >> ignore >> x >> y;               // TexC
        fin >> ignore >> x >> x >> x >> x;     // BoneWeights
        fin >> ignore >> x >> x >> x >> x;     // BoneIndices
    }

    // 跳过三角形
    fin >> ignore; // Triangles header
    for (uint32_t i = 0; i < header.numTriangles; ++i) {
        uint32_t a, b, c;
        fin >> a >> b >> c;
    }

    // 读取 BoneOffsets
    fin >> ignore; // BoneOffsets header
    for (uint32_t i = 0; i < header.numBones; ++i) {
        fin >> ignore >> data.BoneOffsets[i](0, 0) >> data.BoneOffsets[i](0, 1) >> data.BoneOffsets[i](0, 2) >>
            data.BoneOffsets[i](0, 3) >> data.BoneOffsets[i](1, 0) >> data.BoneOffsets[i](1, 1) >>
            data.BoneOffsets[i](1, 2) >> data.BoneOffsets[i](1, 3) >> data.BoneOffsets[i](2, 0) >>
            data.BoneOffsets[i](2, 1) >> data.BoneOffsets[i](2, 2) >> data.BoneOffsets[i](2, 3) >>
            data.BoneOffsets[i](3, 0) >> data.BoneOffsets[i](3, 1) >> data.BoneOffsets[i](3, 2) >>
            data.BoneOffsets[i](3, 3);
    }

    // 读取 BoneHierarchy
    fin >> ignore; // BoneHierarchy header
    for (uint32_t i = 0; i < header.numBones; ++i) {
        fin >> ignore >> data.BoneHierarchy[i];
    }

    // 读取 AnimationClips
    fin >> ignore; // AnimationClips header
    for (uint32_t clipIndex = 0; clipIndex < header.numAnimationClips; ++clipIndex) {
        std::string clipName;
        fin >> ignore >> clipName;
        fin >> ignore; // {

        AnimationClip clip;
        clip.BoneAnimations.resize(header.numBones);
        for (uint32_t boneIndex = 0; boneIndex < header.numBones; ++boneIndex) {
            ReadBoneKeyframes(fin, clip.BoneAnimations[boneIndex]);
        }
        fin >> ignore; // }

        data.Animations[clipName] = clip;
    }

    return RegisterSkeleton(data);
}

// ============================================================================
// 查询
// ============================================================================

const SkeletonData *SkeletonManager::GetSkeleton(SkeletonHandle handle) const {
    if (!IsValid(handle)) {
        return nullptr;
    }
    return &m_entries[handle.index].data;
}

bool SkeletonManager::IsValid(SkeletonHandle handle) const {
    if (!m_initialized) {
        return false;
    }
    if (handle.index >= m_capacity) {
        return false;
    }
    const Entry &entry = m_entries[handle.index];
    return entry.inUse && entry.generation == handle.generation;
}

uint32_t SkeletonManager::GetBoneCount(SkeletonHandle handle) const {
    const auto *data = GetSkeleton(handle);
    return data ? data->BoneCount() : 0;
}

// ============================================================================
// 骨骼计算
// ============================================================================

bool SkeletonManager::ComputeFinalTransforms(SkeletonHandle handle, const std::string &clipName, float timePos,
                                             std::vector<DirectX::XMFLOAT4X4> &outFinalTransforms) const {
    const auto *data = GetSkeleton(handle);
    if (!data) {
        return false;
    }

    outFinalTransforms.resize(data->BoneCount());
    data->GetFinalTransforms(clipName, timePos, outFinalTransforms);
    return true;
}

float SkeletonManager::GetClipDuration(SkeletonHandle handle, const std::string &clipName) const {
    const auto *data = GetSkeleton(handle);
    if (!data) {
        return 0.0f;
    }
    auto it = data->Animations.find(clipName);
    if (it == data->Animations.end()) {
        return 0.0f;
    }
    return it->second.GetClipEndTime();
}

// ============================================================================
// 释放与引用计数
// ============================================================================

void SkeletonManager::Retain(SkeletonHandle handle) {
    if (!IsValid(handle))
        return;
    m_entries[handle.index].refCount++;
}

void SkeletonManager::Release(SkeletonHandle handle, uint64_t fenceValue) {
    if (!IsValid(handle)) {
        return;
    }

    Entry &entry = m_entries[handle.index];
    if (entry.refCount > 0)
        entry.refCount--;
    if (entry.refCount > 0)
        return;

    m_pendingReleases.push_back({handle.index, handle.generation, fenceValue});
}

void SkeletonManager::Reclaim(uint64_t completedFence) {
    auto it = m_pendingReleases.begin();
    while (it != m_pendingReleases.end()) {
        if (it->fenceValue <= completedFence) {
            const Entry &entry = m_entries[it->index];
            if (entry.inUse && entry.generation == it->generation) {
                FreeEntry(it->index);
            }
            it = m_pendingReleases.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================================================
// 统计
// ============================================================================

uint32_t SkeletonManager::GetActiveCount() const {
    uint32_t count = 0;
    for (const auto &entry : m_entries) {
        if (entry.inUse) {
            ++count;
        }
    }
    return count;
}

uint32_t SkeletonManager::GetCapacity() const { return m_capacity; }

} // namespace DX12Engine::Resource
