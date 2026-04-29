// File: d:\project\DX12_Base\Engine\Include\System\Event\MessageArena.h
#pragma once

#include "System/Event/Event.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>

namespace DX12Engine {
namespace System {
namespace Event {

/**
 * @brief 全局消息缓冲区 (Global Message Arena)
 *
 * 采用 Structure of Arrays (SoA) 布局，优化缓存命中率。
 * 支持多线程无锁写入（通过原子索引分配）。
 */
class MessageArena {
public:
    // 默认容量：支持 65536 条未处理消息
    static constexpr uint32_t DEFAULT_CAPACITY = 65536;

    MessageArena(uint32_t capacity = DEFAULT_CAPACITY);
    ~MessageArena();

    // 禁止拷贝
    MessageArena(const MessageArena &) = delete;
    MessageArena &operator=(const MessageArena &) = delete;

    /**
     * @brief 分配一个消息槽位
     *
     * 线程安全。通过原子操作获取唯一索引。
     * @return 分配的索引，如果已满返回 INVALID_INDEX
     */
    MessageIndex AllocateSlot();

    /**
     * @brief 写入消息元数据到指定索引
     *
     * 生产者在线程本地构造好事件后，调用此方法写入 Arena。
     * @param index 由 AllocateSlot 返回的索引
     * @param typeHash 事件类型哈希
     * @param senderId 发送者实体ID (0表示系统事件)
     * @param payloadPtr 指向实际事件数据的指针 (通常在栈上或EnTT组件中)
     */
    void WriteMessage(MessageIndex index, EventTypeHash typeHash, uint32_t senderId, void *payloadPtr);

    /**
     * @brief 获取消息类型
     * @param index 消息索引
     */
    inline EventTypeHash GetType(MessageIndex index) const {
        assert(index < m_capacity);
        return m_typeBuffer[index];
    }

    /**
     * @brief 获取发送者ID
     */
    inline uint32_t GetSender(MessageIndex index) const {
        assert(index < m_capacity);
        return m_senderBuffer[index];
    }

    /**
     * @brief 获取负载指针
     *
     * 消费者需要自行将 void* _cast 为具体事件类型。
     */
    inline void *GetPayload(MessageIndex index) const {
        assert(index < m_capacity);
        return m_payloadBuffer[index];
    }

    /**
     * @brief 获取时间戳
     */
    inline uint64_t GetTimestamp(MessageIndex index) const {
        assert(index < m_capacity);
        return m_timeBuffer[index];
    }

    /**
     * @brief 重置写指针 (帧末调用)
     *
     * 注意：这不会清除数据，只是允许下一帧复用空间。
     * 真正的清理由消费端完成。
     */
    void ResetFrame();

    /**
     * @brief 获取当前帧已分配的消息数量
     */
    inline uint32_t GetCount() const { return m_writeIndex.load(std::memory_order_relaxed); }

    static constexpr MessageIndex INVALID_INDEX = 0xFFFFFFFF;

private:
    uint32_t m_capacity;

    // SoA Buffers - 每个 buffer 独立对齐以防止伪共享
    // 使用 alignas(64) 确保每个数组起始于新的 Cache Line

    alignas(64) EventTypeHash *m_typeBuffer; // uint32_t
    alignas(64) uint32_t *m_senderBuffer;    // uint32_t
    alignas(64) void **m_payloadBuffer;      // void*
    alignas(64) uint64_t *m_timeBuffer;      // uint64_t

    // 原子写索引，用于无锁分配
    std::atomic<uint32_t> m_writeIndex;
};

} // namespace Event
} // namespace System
} // namespace DX12Engine