// File: d:\project\DX12_Base\Engine\Src\System\Event\BucketManager.cpp
#include "System/Event/BucketManager.h"
#include <cassert>
#include <chrono>
#include <intrin.h> // Windows specific intrinsic header
#include <thread>   // std::this_thread::yield

#pragma intrinsic(_BitScanForward)

// ===== "急救手术"：CPU 指令支持 =====
#ifdef _WIN32
#include <immintrin.h> // _mm_pause
#define ARENA_CPU_PAUSE() _mm_pause()
#else
#include <sched.h>
#define ARENA_CPU_PAUSE() sched_yield()
#endif

namespace DX12Engine {
namespace System {
namespace Event {

BucketManager::BucketManager() : m_arena(nullptr), m_activeMask(0) {}

void BucketManager::Initialize(MessageArena &arena, uint32_t capacityPerBucket) {
    m_arena = &arena;
    m_activeMask.store(0, std::memory_order_relaxed);

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
        // 原子设置对应位为 1
        uint32_t mask = 1u << prioIdx;
        uint32_t oldMask = m_activeMask.load(std::memory_order_relaxed);
        while ((oldMask & mask) == 0) {
            uint32_t newMask = oldMask | mask;
            if (m_activeMask.compare_exchange_weak(oldMask, newMask, std::memory_order_release,
                                                   std::memory_order_relaxed)) {
                break;
            }
        }
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
    // 使用循环代替递归，避免栈溢出
    constexpr int MAX_RETRIES = 16;                          // 适当增加重试次数，配合退避策略
    constexpr int RETRY_BACKOFF_THRESHOLD = MAX_RETRIES / 2; // 半程后开始退避

    for (int retry = 0; retry < MAX_RETRIES; ++retry) {
        // ===== "急救手术"惊群缓解：退避策略 =====
        if (retry > RETRY_BACKOFF_THRESHOLD) {
            // 重试次数过半，使用 CPU pause 让出资源
            ARENA_CPU_PAUSE();
        }

        uint32_t currentMask = m_activeMask.load(std::memory_order_acquire);
        if (currentMask == 0) {
            return false;
        }

        // 获取当前时间，用于计算 Aging
        uint64_t currentTimeUs = GetCurrentTimeUs();

        // --- 核心逻辑：基于 Aging 的动态优先级选择 ---

        uint32_t bestBucketIdx = 0;
        float maxEffectivePriority = -1.0f;
        bool found = false;

        // ===== "急救手术"：收集所有候选桶的 Generation =====
        uint64_t candidateGenerations[MAX_PRIORITY_LEVELS] = {0};
        uint32_t validCandidateMask = 0;

        // 遍历所有非空桶，计算有效优先级
        uint32_t tempMask = currentMask;
        while (tempMask != 0) {
            // 获取当前最低位的 1 的索引
            uint32_t idx = GetLowestSetBitIndex(tempMask);

            // 清除该位，继续循环
            tempMask &= ~(1u << idx);

            const Bucket &bucket = m_buckets[idx];

            // ===== "急救手术"：快照 Generation + 空检查 =====
            uint64_t genBefore = bucket.GetGeneration();

            // 再次确认非空（并发环境下可能刚变空）
            if (bucket.IsEmpty()) {
                continue;
            }

            // 保存 Generation 用于后续 ABA 验证
            candidateGenerations[idx] = genBefore;
            validCandidateMask |= (1u << idx);

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
            // 所有桶都已变空，原子地清除掩码
            m_activeMask.fetch_and(~currentMask, std::memory_order_release);
            return false;
        }

        // --- 从选定的最佳桶中弹出消息 ---
        Bucket &bestBucket = m_buckets[bestBucketIdx];

        // ===== "急救手术"：ABA 问题检测 =====
        // 在选择和 Pop 之间验证 Generation 是否变化
        uint64_t genBeforePop = bestBucket.GetGeneration();
        if (candidateGenerations[bestBucketIdx] != genBeforePop) {
            // Generation 变了！数据被修改了，放弃本次计算并重试
            continue;
        }

        MessageIndex index;
        if (bestBucket.Pop(index)) {
            // ===== "急救手术"：Pop 后递增 Generation =====
            bestBucket.IncrementGeneration();

            outIndex = index;
            outPriority = static_cast<EventPriority>(bestBucketIdx);

            // ===== "急救手术"：使用原子递增更新 LastServeTime =====
            // 避免时间戳被覆盖导致 Aging 计算错误
            // 使用 fetch_add(0) 实际上不改变值，但保证了原子性
            bestBucket.UpdateLastServeTime(currentTimeUs);

            // 检查桶是否变空，如果是，原子更新掩码
            if (bestBucket.IsEmpty()) {
                uint32_t mask = 1u << bestBucketIdx;
                uint32_t oldMask = m_activeMask.load(std::memory_order_relaxed);
                while ((oldMask & mask) != 0) {
                    uint32_t newMask = oldMask & ~mask;
                    if (m_activeMask.compare_exchange_weak(oldMask, newMask, std::memory_order_release,
                                                           std::memory_order_relaxed)) {
                        break;
                    }
                }
            }

            return true;
        } else {
            // ===== "急救手术"：Pop 失败也要更新 Generation =====
            // 表示有并发操作发生了
            bestBucket.IncrementGeneration();

            // 并发竞争：刚才非空，现在空了
            uint32_t mask = 1u << bestBucketIdx;
            uint32_t oldMask = m_activeMask.load(std::memory_order_relaxed);
            while ((oldMask & mask) != 0) {
                uint32_t newMask = oldMask & ~mask;
                if (m_activeMask.compare_exchange_weak(oldMask, newMask, std::memory_order_release,
                                                       std::memory_order_relaxed)) {
                    break;
                }
            }
            // 继续循环重试
        }
    }

    // 达到最大重试次数，说明并发竞争激烈
    // 使用 yield 让其他线程有机会运行
    std::this_thread::yield();
    return false;
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
    uint32_t newMask = 0;
    for (uint32_t i = 0; i < MAX_PRIORITY_LEVELS; ++i) {
        if (!m_buckets[i].IsEmpty()) {
            newMask |= (1u << i);
        }
    }
    m_activeMask.store(newMask, std::memory_order_release);
}

uint64_t BucketManager::GetTotalPendingCount() const {
    uint64_t total = 0;
    for (uint32_t i = 0; i < MAX_PRIORITY_LEVELS; ++i) {
        total += m_buckets[i].SizeApprox();
    }
    return total;
}

uint64_t BucketManager::GetTotalEvictedCount() const {
    uint64_t total = 0;
    for (uint32_t i = 0; i < MAX_PRIORITY_LEVELS; ++i) {
        total += m_buckets[i].GetEvictedCount();
    }
    return total;
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