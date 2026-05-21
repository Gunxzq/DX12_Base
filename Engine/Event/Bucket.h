#pragma once

#include "Event.h"
#include <algorithm>
#include <atomic>
#include <moodycamel/concurrentqueue.h>
// #include <thread>
#include <vector>

namespace DX12Engine {
namespace Event {

enum class DiscardPolicy { None, Throttle, Sample };

// ===== 2026-04-29 精准容量配置 =====
// 容量计算公式：MaxMessagesPerFrame × ExpectedMaxLatencyFrames × SafetyFactor
//
// 生产环境推荐值（典型游戏）：
// - 高频事件（碰撞、物理）: 500 条/帧
// - 延迟容忍: 3 帧
// - 安全系数: 1.3x（内存敏感场景）
// - 结果: 500 × 3 × 1.3 = 1950 ≈ 2048
//
// 保守值：4096（留 2x 余量）
// 注意：测试用例应显式传入更大值进行压测
constexpr uint32_t REASONABLE_CAPACITY = 2048; // 2K（生产环境默认值，省内存）

class Bucket {
public:
    /**
     * @brief 默认构造函数
     */
    Bucket()
        : m_lastServeTimeUs(0), m_policy(DiscardPolicy::None), m_processedCountThisFrame(0), m_generation(0),
          m_evictedCount(0) {}

    /**
     * @brief 构造函数（直接初始化）
     * @param capacity 桶容量提示
     * @param policy 丢弃策略
     */
    Bucket(uint32_t capacity, DiscardPolicy policy)
        : m_lastServeTimeUs(0), m_policy(policy), m_queue(capacity), m_processedCountThisFrame(0), m_generation(0),
          m_evictedCount(0) {}

    /**
     * @brief 初始化桶 - 使用冷启动优化
     *
     * 采用"临时队列交换法"(Swap Trick) 确保队列内部拥有足够的内存块。
     *
     * 手术步骤：
     * 1. 创建临时队列，传入目标容量
     * 2. 暴力填充：enqueue_bulk 强制分配所有内存块
     * 3. 清空数据，保留结构
     * 4. swap 接管预热好的内存结构
     *
     * @param capacity 目标容量
     * @param policy 丢弃策略
     */
    void Initialize(uint32_t capacity, DiscardPolicy policy) {
        m_policy = policy;
        capacity = (capacity > 0) ? capacity : REASONABLE_CAPACITY;

        // ===== 2026-04-29 冷启动优化 =====
        // 目标：确保队列内部拥有足够的"块(Block)"来容纳 capacity 个元素
        // 原理：利用临时对象预分配内存块，然后 Swap 接管

        // 1. 创建临时队列
        moodycamel::ConcurrentQueue<MessageIndex> tempQueue(capacity);

        // 2. 准备占位符数据
        std::vector<MessageIndex> placeholders(capacity, 0);

        // 3. 暴力入队 (Enqueue Bulk)
        // 关键：强制 tempQueue 申请足够的内存块
        size_t actuallyEnqueued = tempQueue.enqueue_bulk(placeholders.data(), capacity);
        if (actuallyEnqueued < capacity) {
            // 内存不足，按实际分配量继续
        }

        // 4. 清空数据，保留内存块结构
        MessageIndex dummy;
        while (tempQueue.try_dequeue(dummy)) {
            // 只读出数据，不释放内存
        }

        // 5. swap 接管
        m_queue.swap(tempQueue);
        // tempQueue 离开作用域，被销毁（此时为空，销毁极快）
    }

    /**
     * @brief 推入单个消息
     * @param index 消息在 Arena 中的索引
     * @return bool 是否成功入队
     */
    bool Push(MessageIndex index) {
        switch (m_policy) {
        case DiscardPolicy::Sample: { // 丢旧存新
            // 1. 先尝试无锁入队（快速路径）
            if (m_queue.try_enqueue(index))
                return true;

            // 2. 满了，dequeue 腾空间（被踢出的旧消息计入 evicted）
            MessageIndex dummy;
            if (m_queue.try_dequeue(dummy)) {
                m_evictedCount.fetch_add(1, std::memory_order_relaxed);
                // 腾出空间后重试
                if (m_queue.try_enqueue(index))
                    return true;
            }

            // 3. 仍失败，不扩容，直接丢弃（尊重容量限制）
            return false;
        }
        case DiscardPolicy::Throttle: {        // 静默丢弃
            return m_queue.try_enqueue(index); // 满了就丢弃新数据
        }
        case DiscardPolicy::None: // 默认不丢弃
        default: {
            // try_enqueue 在队列满时返回 false，不会扩容
            // 如果需要严格容量限制，使用此策略即可
            return m_queue.try_enqueue(index);
        }
        }
    }

    /**
     * @brief 批量推入消息
     * @param indices 消息索引数组指针
     * @param count 索引数量
     * @return size_t 实际入队的数量
     */
    size_t PushBatch(const MessageIndex *indices, size_t count) {
        if (!indices || count == 0)
            return 0;
        return m_queue.enqueue_bulk(indices, count);
    }

    /**
     * @brief 弹出单个消息
     * @param index 输出参数
     * @return bool 是否成功
     */
    bool Pop(MessageIndex &index) { return m_queue.try_dequeue(index); }

    /**
     * @brief 批量窃取/获取所有可用消息
     * @param destinationBuffer 目标缓冲区指针
     * @param bufferSize 目标缓冲区最大容量
     * @return size_t 实际获取的消息数量
     */
    size_t StealAll(MessageIndex *destinationBuffer, size_t bufferSize) {
        if (!destinationBuffer || bufferSize == 0)
            return 0;
        return m_queue.try_dequeue_bulk(destinationBuffer, bufferSize);
    }

    /**
     * @brief 获取被 Sample 策略踢出的消息总数
     */
    uint64_t GetEvictedCount() const { return m_evictedCount.load(std::memory_order_relaxed); }

    bool IsEmpty() const { return m_queue.size_approx() == 0; }

    /**
     * @brief 更新最后服务时间
     * @param timeUs 当前时间戳（微秒）
     * @note 在调度器处理完该桶的消息后调用，用于计算 Aging
     */
    void UpdateLastServeTime(uint64_t timeUs) { m_lastServeTimeUs.store(timeUs, std::memory_order_relaxed); }

    /**
     * @brief 获取最后服务时间
     * @return uint64_t 最后服务时间戳（微秒）
     * @note 供 BucketManager 计算 Aging 使用
     */
    uint64_t GetLastServeTime() const { return m_lastServeTimeUs.load(std::memory_order_relaxed); }

    /**
     * @brief 重置帧统计信息
     * @note 在每帧开始时或结束时调用，重置本帧的处理计数等
     */
    void ResetFrameStats() {
        // 重置本帧已处理的消息计数
        // 这个计数可以用于下一帧的"软限制"建议
        m_processedCountThisFrame.store(0, std::memory_order_relaxed);

        // 注意：不要重置 m_lastServeTimeUs！
        // Aging 需要知道上一次处理是什么时候，如果重置为0或当前时间，
        // 会导致 Aging 计算错误（要么瞬间变大，要么瞬间变0）。
    }

    /**
     * @brief 增加本帧处理计数
     * @param count 增加的数量
     * @note 在调度器处理完消息后调用
     */
    void AddProcessedCount(size_t count) { m_processedCountThisFrame.fetch_add(count, std::memory_order_relaxed); }

    /**
     * @brief 获取本帧已处理的消息数量
     * @return size_t
     * @note 供调度器参考，用于动态调整下一帧的预算
     */
    size_t GetProcessedCountThisFrame() const { return m_processedCountThisFrame.load(std::memory_order_relaxed); }

    /**
     * @brief 获取桶中近似消息数量
     * @return size_t
     */
    size_t SizeApprox() const { return m_queue.size_approx(); }

    /**
     * @brief 获取版本号（用于 ABA 问题检测）
     * @return uint64_t 当前版本号
     * @note 每次 Push/Pop 后版本号都会增加
     */
    uint64_t GetGeneration() const { return m_generation.load(std::memory_order_acquire); }

    /**
     * @brief 增加版本号（供内部使用）
     * @note 在 Push/Pop 操作后调用
     */
    void IncrementGeneration() { m_generation.fetch_add(1, std::memory_order_release); }

private:
    moodycamel::ConcurrentQueue<MessageIndex> m_queue;

    // 最后服务时间戳，用于计算 Aging
    std::atomic<uint64_t> m_lastServeTimeUs;

    // 本帧已处理的消息数量，用于动态反馈调节
    std::atomic<size_t> m_processedCountThisFrame;

    // ===== "急救手术"：版本号防止 ABA =====
    // 每次 Push/Pop 后递增，用于检测数据是否被修改
    std::atomic<uint64_t> m_generation;

    DiscardPolicy m_policy;

    // 被 Sample 策略踢出的消息总数
    std::atomic<uint64_t> m_evictedCount;
};

} // namespace Event

} // namespace DX12Engine