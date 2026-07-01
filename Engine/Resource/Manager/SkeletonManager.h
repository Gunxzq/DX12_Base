#pragma once

#include "Resource/Skeleton/SkeletonData.h"
#include "Resource/Struct/SkeletonHandle.h"
#include <cstdint>
#include <string>
#include <vector>

namespace DX12Engine::Resource {

// ============================================================================
// 骨骼资源管理器
// ============================================================================
class SkeletonManager {
public:
    SkeletonManager() = default;
    ~SkeletonManager() = default;

    SkeletonManager(const SkeletonManager &) = delete;
    SkeletonManager &operator=(const SkeletonManager &) = delete;

    // ========================================================================
    // 初始化/关闭
    // ========================================================================
    void Initialize(uint32_t initialCapacity = 128);
    void Shutdown();

    // ========================================================================
    // 注册/加载
    // ========================================================================
    SkeletonHandle RegisterSkeleton(const SkeletonData &data);
    SkeletonHandle LoadFromM3d(const std::string &filepath);

    // ========================================================================
    // 查询
    // ========================================================================
    const SkeletonData *GetSkeleton(SkeletonHandle handle) const;

    bool IsValid(SkeletonHandle handle) const;
    uint32_t GetBoneCount(SkeletonHandle handle) const;

    // ========================================================================
    // 骨骼计算（CPU 端，供 AnimationSystem 使用）
    // ========================================================================
    bool ComputeFinalTransforms(SkeletonHandle handle, const std::string &clipName, float timePos,
                                std::vector<DirectX::XMFLOAT4X4> &outFinalTransforms) const;

    float GetClipDuration(SkeletonHandle handle, const std::string &clipName) const;

    // ========================================================================
    // 释放
    // ========================================================================
    void Release(SkeletonHandle handle, uint64_t fenceValue);
    void Reclaim(uint64_t completedFence);

    // ========================================================================
    // 调试/统计
    // ========================================================================
    uint32_t GetActiveCount() const;
    uint32_t GetCapacity() const;

private:
    struct Entry {
        SkeletonData data;
        uint32_t generation = 0;
        bool inUse = false;
    };

    struct PendingRelease {
        uint32_t index;
        uint32_t generation;
        uint64_t fenceValue;
    };

    uint32_t AllocateEntry();
    void FreeEntry(uint32_t index);

    std::vector<Entry> m_entries;
    std::vector<uint32_t> m_freeList;
    std::vector<PendingRelease> m_pendingReleases;
    uint32_t m_nextGeneration = 1;
    uint32_t m_capacity = 0;
    bool m_initialized = false;

    static constexpr uint32_t INITIAL_CAPACITY = 128;
    static constexpr uint32_t MAX_CAPACITY = 16384;
};

} // namespace DX12Engine::Resource
