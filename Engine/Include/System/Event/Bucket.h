#pragma once

#include "System/Event/Event.h"
#include <algorithm> // for std::min
#include <atomic>
#include <moodycamel/concurrentqueue.h>
#include <vector>

namespace DX12Engine {
namespace System {
namespace Event {

enum class DiscardPolicy { None, Throttle, Sample };

class Bucket {
public:
    /**
     * @brief 构造函数
     * @param capacity 桶容量提示
     * @param policy 丢弃策略
     */
    Bucket(uint32_t capacity = 0, DiscardPolicy policy = DiscardPolicy::None)
        : m_lastServeTimeUs(0), m_policy(policy), m_queue(capacity), m_processedCountThisFrame(0) {}

    void Initialize(uint32_t capacity, DiscardPolicy policy) { m_policy = policy; }

    /**
     * @brief 推入单个消息
     * @param index 消息在 Arena 中的索引
     * @return bool 是否成功入队
     */
    bool Push(MessageIndex index) {
        if (m_policy == DiscardPolicy::Sample) {
            // 采样策略：尝试入队，如果失败（队列满），则丢弃旧数据腾出空间或直接丢弃新数据
            // moodycamel 默认是动态增长的，除非设置了最大块大小。
            // 这里简单实现：如果希望严格采样（只留最新），可以在入队前清空，但这破坏了并发安全性。
            // 更合理的采样：如果队列接近上限，先 dequeue 掉一些旧的。
            // 由于 ConcurrentQueue 是无锁且动态增长的，try_enqueue 通常都会成功，除非内存耗尽。
            // 对于 Sample 策略，通常由生产者控制，或者在这里做一个简单的“覆盖”逻辑比较复杂。
            // 暂保持标准入队，依赖上层控制频率或后续批量处理时的采样。
            return m_queue.try_enqueue(index);
        }

        return m_queue.try_enqueue(index);
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
        // 这个计数可以用于下一帧的“软限制”建议
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

private:
    moodycamel::ConcurrentQueue<MessageIndex> m_queue;

    // 最后服务时间戳，用于计算 Aging
    std::atomic<uint64_t> m_lastServeTimeUs;

    // 本帧已处理的消息数量，用于动态反馈调节
    std::atomic<size_t> m_processedCountThisFrame;

    DiscardPolicy m_policy;
};

} // namespace Event
} // namespace System
} // namespace DX12Engine