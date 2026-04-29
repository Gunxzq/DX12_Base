#pragma once

#include "System/Event/Event.h"
#include <algorithm>
#include <atomic>
#include <moodycamel/concurrentqueue.h>
// #include <thread>
#include <vector>

namespace DX12Engine {
namespace System {
namespace Event {

enum class DiscardPolicy { None, Throttle, Sample };

// 建议：根据实际帧率预算设置，例如 1000 条/帧 * 2 帧 = 2000
// 注意：moodycamel ConcurrentQueue 的内部块大小可能限制实际容量
// 设置为一个较大的默认值，确保不会成为瓶颈
constexpr uint32_t REASONABLE_CAPACITY = 32768; // 32K

// ===== "急救手术"：Sample 策略最大重试次数，防止物理死锁 =====
constexpr int MAX_SAMPLE_RETRY_COUNT = 10000;

class Bucket {
public:
    /**
     * @brief 默认构造函数
     */
    Bucket() : m_lastServeTimeUs(0), m_policy(DiscardPolicy::None), m_processedCountThisFrame(0), m_generation(0) {}

    /**
     * @brief 构造函数（直接初始化）
     * @param capacity 桶容量提示
     * @param policy 丢弃策略
     */
    Bucket(uint32_t capacity, DiscardPolicy policy)
        : m_lastServeTimeUs(0), m_policy(policy), m_queue(capacity), m_processedCountThisFrame(0), m_generation(0) {}

    void Initialize(uint32_t capacity, DiscardPolicy policy) {
        m_policy = policy;
        capacity = capacity > 0 ? capacity : REASONABLE_CAPACITY;

        // ===== "急救手术"：使用批量预分配 =====
        // moodycamel::ConcurrentQueue 的构造函数参数可能不是直接容量限制
        // 使用 enqueue_bulk 批量操作来确保正确的内部分配
        moodycamel::ConcurrentQueue<MessageIndex> newQueue;
        std::vector<MessageIndex> temp(capacity);
        for (auto &idx : temp)
            idx = 0;
        // 批量入队，触发足够的内部块分配
        size_t enqueued = newQueue.enqueue_bulk(temp.data(), capacity);
        // 清空队列，但保留分配的空间
        MessageIndex dummy;
        while (newQueue.try_dequeue(dummy))
            ;
        m_queue.swap(newQueue);
    }

    /**
     * @brief 推入单个消息
     * @param index 消息在 Arena 中的索引
     * @return bool 是否成功入队
     */
    bool Push(MessageIndex index) {
        switch (m_policy) {
        case DiscardPolicy::Sample: { // 丢旧存新
            // ===== "急救手术"：防死锁保护 =====
            MessageIndex dummy;
            int retryCount = 0;
            while (retryCount < MAX_SAMPLE_RETRY_COUNT) {
                if (m_queue.try_enqueue(index))
                    return true;
                // 队列满，强制移除一个旧的
                if (m_queue.try_dequeue(dummy)) {
                    retryCount++;
                    continue; // 移除后重试入队
                }
                // yield 让出 CPU，减少竞争
                std::this_thread::yield();
                retryCount++;
            }
            // 达到最大重试次数，返回失败（而不是死循环）
            return false;
        }
        case DiscardPolicy::Throttle: {        // 静默丢弃
            return m_queue.try_enqueue(index); // 满了就丢弃新数据
        }
        case DiscardPolicy::None: // 默认不丢弃（可能阻塞或报错）
        default: {
            // ===== "急救手术"：使用 enqueue 自动扩容 =====
            // try_enqueue 在队列满时返回 false，不会扩容
            // enqueue 会在必要时自动扩容（除非内存耗尽）
            try {
                m_queue.enqueue(index);
                return true;
            } catch (const std::bad_alloc &) {
                return false; // 内存不足时返回失败
            }
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
};

} // namespace Event
} // namespace System
} // namespace DX12Engine