#pragma once

#include "System/Event/Bucket.h"
#include "System/Event/Event.h"
#include <array>
#include <atomic>
#include <cstdint>

namespace DX12Engine {
namespace System {
namespace Event {

// 最大支持的优先级数量
static constexpr uint32_t MAX_PRIORITY_LEVELS = 5; // P0-P4

/**
 * @brief 桶管理器
 *
 * 负责管理所有优先级桶，计算动态优先级，并供调度器查询下一个要处理的消息。
 *
 * @note 2026-04-29 "急救手术"并发安全改造：
 *
 * 1. ABA 问题防御：通过 Bucket.m_generation 版本号检测数据竞争
 *    - 读取 Mask + Generation → 计算优先级 → 再次检查 Generation
 *    - 如果 Generation 变了，说明数据被修改，放弃本次计算并重试
 *
 * 2. 惊群效应缓解：引入 CPU pause + 指数退避
 *    - 重试次数过半时使用 _mm_pause() 或 yield()
 *    - 避免 10 个线程空转把 CPU 核心锁死
 *
 * 3. 时间戳竞争优化：
 *    - 改用原子递增（atomic_fetch_add）更新 LastServeTime
 *    - 或者在单消费者模式下禁用写入（只读模式）
 */
class BucketManager {
public:
    BucketManager();
    ~BucketManager() = default;

    // 禁止拷贝
    BucketManager(const BucketManager &) = delete;
    BucketManager &operator=(const BucketManager &) = delete;

    /**
     * @brief 初始化桶
     *
     * @param arena 引用全局消息缓冲区
     * @param capacityPerBucket 每个桶的初始容量建议
     */
    void Initialize(uint32_t capacityPerBucket = 1024);

    /**
     * @brief 将消息索引推入指定优先级的桶
     *
     * @param index Arena 中的消息索引
     * @param priority 消息优先级
     * @return true 成功入桶, false 桶满或丢弃
     */
    bool PushMessage(MessageIndex index, EventPriority priority);

    /**
     * @brief 获取下一个最高优先级的消息索引
     *
     * 调度器每帧调用此方法获取待处理消息。
     * @param outIndex 输出的消息索引
     * @param outPriority 输出的消息优先级
     * @return true 如果有消息, false 如果所有桶为空
     */
    bool PopNextMessage(MessageIndex &outIndex, EventPriority &outPriority);

    /**
     * @brief 重置所有桶 (帧末调用)
     *
     * 注意：通常不需要完全重置桶内容，只需重置统计信息。
     * 真正的清空由消费者 Pop 完成。
     */
    void ResetFrame();

    /**
     * @brief 获取所有桶中的消息总数
     * @return uint64_t 消息总数
     */
    uint64_t GetTotalPendingCount() const;

    /**
     * @brief 获取所有桶中被 Sample 策略踢出的消息总数
     * @return uint64_t 被踢出的消息总数
     */
    uint64_t GetTotalEvictedCount() const;

private:
      // 优先级桶数组
    std::array<Bucket, MAX_PRIORITY_LEVELS> m_buckets;

    // 位掩码：第 i 位为 1 表示优先级 i 的桶非空（使用原子操作保证线程安全）
    std::atomic<uint32_t> m_activeMask;

    // 老化系数：每毫秒增加的优先级权重
    static constexpr float AGING_FACTOR_PER_MS = 0.1f;

    /**
     * @brief 更新活跃掩码
     */
    void UpdateActiveMask();

    /**
     * @brief 获取当前时间（微秒）
     * @return uint64_t 当前时间（微秒）
     * @date 2026-04-29
     */
    uint64_t GetCurrentTimeUs() const;

    /**
     * @brief 计算实际优先级
     * @param basePriority 基础优先级
     * @param lastServeTimeUs 上次服务时间
     * @param currentTimeUs 当前时间
     * @return float
     * @date 2026-04-29
     */
    float CalculateEffectivePriority(uint32_t basePriority, uint64_t lastServeTimeUs, uint64_t currentTimeUs) const;
};

} // namespace Event
} // namespace System
} // namespace DX12Engine