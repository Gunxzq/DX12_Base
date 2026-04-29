// File: d:\project\DX12_Base\Engine\Src\System\Event\BucketManager.cpp
#include "System/Event/BucketManager.h"
#include <cassert>
#include <chrono>
#include <intrin.h> // Windows specific intrinsic header

#pragma intrinsic(_BitScanForward)

namespace DX12Engine {
namespace System {
namespace Event {

BucketManager::BucketManager() : m_arena(nullptr), m_activeMask(0) {}

void BucketManager::Initialize(MessageArena &arena, uint32_t capacityPerBucket) {
    m_arena = &arena;
    m_activeMask = 0;

    // 初始化每个优先级的桶
    for (uint32_t i = 0; i < MAX_PRIORITY_LEVELS; ++i) {
        // P0-P2: None, P3: Sample, P4: Throttle (示例)
        DiscardPolicy policy = DiscardPolicy::None;
        if (i == 3)
            policy = DiscardPolicy::Sample;
        if (i == 4)
            policy = DiscardPolicy::Throttle;

        m_buckets[i].Initialize(capacityPerBucket, policy);
    }

    UpdateActiveMask();
}

bool BucketManager::PushMessage(MessageIndex index, EventPriority priority) {
    if (!m_arena || static_cast<uint32_t>(priority) >= MAX_PRIORITY_LEVELS) {
        return false;
    }

    uint32_t prioIdx = static_cast<uint32_t>(priority);
    bool success = m_buckets[prioIdx].Push(index);

    if (success) {
        // 设置对应位为 1
        m_activeMask |= (1u << prioIdx);
    }

    return success;
}

/**
 * @brief 辅助函数：Windows x64 获取最低位 1 的索引
 * @param mask 非零掩码
 * @return 最低位 1 的索引 (0-31)
 */
static inline uint32_t GetLowestSetBitIndex(uint32_t mask) {
    assert(mask != 0 && "Mask must not be zero");

    unsigned long index;
    // _BitScanForward searches for the first set bit starting from the LSB
    if (_BitScanForward(&index, mask)) {
        return static_cast<uint32_t>(index);
    }
    return 32; // Should not happen if mask != 0
}

uint64_t BucketManager::GetCurrentTimeUs() const {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

bool BucketManager::PopNextMessage(MessageIndex &outIndex, EventPriority &outPriority) {
    if (m_activeMask == 0) {
        return false;
    }

    // 获取当前时间，用于计算 Aging
    uint64_t currentTimeUs = GetCurrentTimeUs();

    // --- 核心逻辑：基于 Aging 的动态优先级选择 ---

    uint32_t bestBucketIdx = 0;
    float maxEffectivePriority = -1.0f;
    bool found = false;

    // 遍历所有非空桶，计算有效优先级
    uint32_t tempMask = m_activeMask;
    while (tempMask != 0) {
        // 获取当前最低位的 1 的索引
        uint32_t idx = GetLowestSetBitIndex(tempMask);

        // 清除该位，继续循环
        tempMask &= ~(1u << idx);

        const Bucket &bucket = m_buckets[idx];

        // 再次确认非空（并发环境下可能刚变空）
        if (bucket.IsEmpty()) {
            continue;
        }

        // 计算有效优先级: Base + Aging
        uint64_t lastServeTime = bucket.GetLastServeTime();
        float score = CalculateEffectivePriority(idx, lastServeTime, currentTimeUs);

        if (score > maxEffectivePriority) {
            maxEffectivePriority = score;
            bestBucketIdx = idx;
            found = true;
        }
    }

    if (!found) {
        // 理论上不会发生，除非并发导致所有桶瞬间变空
        m_activeMask = 0;
        return false;
    }

    // --- 从选定的最佳桶中弹出消息 ---
    Bucket &bestBucket = m_buckets[bestBucketIdx];

    MessageIndex index;
    if (bestBucket.Pop(index)) {
        outIndex = index;
        outPriority = static_cast<EventPriority>(bestBucketIdx);

        // 更新最后服务时间为当前时间
        bestBucket.UpdateLastServeTime(currentTimeUs);

        // 检查桶是否变空，如果是，更新掩码
        if (bestBucket.IsEmpty()) {
            m_activeMask &= ~(1u << bestBucketIdx);
        }

        return true;
    } else {
        // 并发竞争：刚才非空，现在空了
        m_activeMask &= ~(1u << bestBucketIdx);
        // 递归重试，直到找到消息或所有桶为空
        return PopNextMessage(outIndex, outPriority);
    }
}

void BucketManager::ResetFrame() {
    // 帧末重置统计信息
    for (auto &bucket : m_buckets) {
        bucket.ResetFrameStats();
    }

    // 重新同步活跃掩码，确保与桶的实际状态一致
    UpdateActiveMask();
}

void BucketManager::UpdateActiveMask() {
    m_activeMask = 0;
    for (uint32_t i = 0; i < MAX_PRIORITY_LEVELS; ++i) {
        if (!m_buckets[i].IsEmpty()) {
            m_activeMask |= (1u << i);
        }
    }
}

float BucketManager::CalculateEffectivePriority(uint32_t basePriority, uint64_t lastServeTimeUs,
                                                uint64_t currentTimeUs) const {
    // 如果从未被服务过，lastServeTimeUs 为 0
    if (lastServeTimeUs == 0) {
        // 给予一个初始的基础分数
        return static_cast<float>(MAX_PRIORITY_LEVELS - basePriority) + 1000.0f; // 加上一个大常数确保优先
    }

    uint64_t delayUs = 0;
    if (currentTimeUs >= lastServeTimeUs) {
        delayUs = currentTimeUs - lastServeTimeUs;
    } else {
        // 防止时间回滚
        delayUs = 0;
    }

    float delayMs = static_cast<float>(delayUs) / 1000.0f;

    // 公式：Urgency = (MaxPrio - BasePrio) + (DelayMs * Factor)
    // 基础分数：优先级越高（数值越小），基础分数越高
    float baseScore = static_cast<float>(MAX_PRIORITY_LEVELS - basePriority);
    float agingScore = delayMs * AGING_FACTOR_PER_MS;

    return baseScore + agingScore;
}

} // namespace Event
} // namespace System
} // namespace DX12Engine