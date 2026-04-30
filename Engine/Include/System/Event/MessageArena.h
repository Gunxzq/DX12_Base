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
 *
 * @note 架构说明 (2026-04-29 统一 Slot 改造):
 *
 * 原始设计中，Arena 仅作为"元数据缓冲区"，负责存储 Type/Sender/Timestamp，
 * 而 Payload 指向外部数据（如 ENTT 组件或堆内存）。这要求调用方自行管理生命周期。
 *
 * 当前实现增加了物理内存池支持，作为"脚手架"方案：
 * - Arena 内部维护一个大块物理内存池 (m_dataPool)
 * - 每个 Slot 大小固定为 32 字节，所有事件自动补齐到统一大小
 * - WriteMessage 会将 payload 数据 memcpy 到 Arena 内部
 * - 这样做可以解决栈对象生命周期问题，直到 ENTT 框架搭建完成
 *
 * 统一 Slot 大小的优势：
 * 1. 内存布局可预测：index * SLOT_SIZE 直接算地址，无需指针数组
 * 2. 消除大小不一致：所有事件统一大小，代码更安全
 * 3. 简化消费者访问：可直接 reinterpret_cast 读取数据
 *
 * 当 ENTT 集成后，可以将 SLOT_SIZE 改为 8 字节，只存指针即可。
 */

// TLS 缓冲区大小：每个线程 64KB，本地缓冲消除竞争
static constexpr size_t TLS_BUFFER_SIZE = 64 * 1024;
// 对齐要求：16 字节 (SSE), 64 字节 (Cache Line)
static constexpr size_t ALIGNMENT_SSE = 16;
static constexpr size_t ALIGNMENT_CACHE_LINE = 64;
// 统一 Slot 大小：所有事件补齐到 32 字节
// 设计考量：
// - 足够容纳大多数事件（大多数事件 < 32 字节）
// - 2 的幂次，便于位运算优化
// - 兼容 SSE/AVX 对齐要求
static constexpr size_t DEFAULT_SLOT_SIZE = 32;
class MessageArena {
public:
    // 默认容量：支持 65536 条未处理消息
    static constexpr uint32_t DEFAULT_CAPACITY = 65536;
    // 物理内存池大小：8MB，每个消息最大假设 256 字节，约可存 32768 条
    static constexpr size_t DEFAULT_POOL_SIZE = 8 * 1024 * 1024;
    // 每个消息的最大尺寸（安全限制，防止恶意数据）
    static constexpr size_t MAX_PAYLOAD_SIZE = 1024;

    /**
     * @brief TLS 本地缓冲区上下文
     *
     * 每个线程拥有独立的本地缓冲区，避免全局原子竞争。
     * 帧末时，所有线程的本地缓冲区会批量 flush 到全局 Arena。
     */
    struct alignas(ALIGNMENT_CACHE_LINE) TLSContext {
        uint8_t buffer[TLS_BUFFER_SIZE];
        size_t offset;
        bool flushed; // 本帧是否已 flush

        TLSContext() : offset(0), flushed(false) {}
    };

    // 获取当前线程的 TLS 上下文（线程本地存储）
    static TLSContext &GetTLSContext();

    MessageArena(uint32_t capacity = DEFAULT_CAPACITY, size_t poolSize = DEFAULT_POOL_SIZE);
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
     *
     * @note 内部实现：
     * 1. 优先使用 TLS 本地缓冲区（无锁，消除伪共享）
     * 2. TLS 满了则回退到全局 Arena 池
     * 3. 所有数据按 16 字节对齐，防止 SSE 错误
     *
     * @param index 由 AllocateSlot 返回的索引
     * @param typeHash 事件类型哈希
     * @param senderId 发送者实体ID (0表示系统事件)
     * @param payloadPtr 指向实际事件数据的指针 (通常在栈上)
     * @param payloadSize payload 的实际大小（字节）
     */
    void WriteMessage(MessageIndex index, EventTypeHash typeHash, uint32_t senderId, void *payloadPtr,
                      size_t payloadSize);

    /**
     * @brief 将所有 TLS 本地缓冲区 flush 到全局 Arena
     *
     * 在帧末调用，确保所有线程的数据都已写入 Arena。
     * 必须在主线程（消费者线程）中调用。
     */
    void FlushAllTLS();

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
     * 消费者需要自行将 void* cast 为具体事件类型。
     * @note 由于使用统一 Slot 大小，直接通过 index * SLOT_SIZE 计算地址。
     */
    inline void *GetPayload(MessageIndex index) const {
        assert(index < m_capacity);
        // 内存屏障：确保读取到生产者写入的最新数据
        std::atomic_thread_fence(std::memory_order_acquire);
        return const_cast<uint8_t *>(m_dataPool) + static_cast<size_t>(index) * DEFAULT_SLOT_SIZE;
    }

    /**
     * @brief 获取负载指针（带对齐检查，用于调试）
     *
     * @param index 消息索引
     * @param requireAlignment 要求的对齐字节数
     * @return 对齐后的指针（如果不是对齐的，返回原始指针）
     * @note 统一 Slot 大小天然 16 字节对齐，此检查主要用于验证
     */
    inline void *GetPayloadAligned(MessageIndex index, size_t requireAlignment = ALIGNMENT_SSE) const {
        void *ptr = GetPayload(index);
#if defined(_DEBUG)
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        if (addr % requireAlignment != 0) {
            // 指针未对齐！这是危险的，可能导致 SSE 错误
            // 在 Debug 模式下触发断言
            assert(false && "Arena Payload Misaligned! SSE access will crash.");
        }
#endif
        return ptr;
    }

    /**
     * @brief 获取物理内存池起始地址（用于指针范围验证）
     */
    inline const uint8_t *GetPoolStart() const { return m_dataPool; }

    /**
     * @brief 获取物理内存池大小
     */
    inline size_t GetPoolSize() const { return m_poolSize; }

    /**
     * @brief 获取时间戳
     */
    inline uint64_t GetTimestamp(MessageIndex index) const {
        assert(index < m_capacity);
        std::atomic_thread_fence(std::memory_order_acquire);
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

    /**
     * @brief 获取物理内存池使用量（调试用）
     * @note 由于使用统一 Slot，按已分配的 Slot 数量计算
     */
    inline size_t GetPoolUsage() const {
        return static_cast<size_t>(m_writeIndex.load(std::memory_order_relaxed)) * DEFAULT_SLOT_SIZE;
    }

    /**
     * @brief 获取统一 Slot 大小
     */
    inline size_t GetSlotSize() const { return DEFAULT_SLOT_SIZE; }

    static constexpr MessageIndex INVALID_INDEX = 0xFFFFFFFF;

private:
    uint32_t m_capacity;
    size_t m_poolSize;

    // SoA Buffers - 每个 buffer 独立对齐以防止伪共享
    // 使用 alignas(64) 确保每个数组起始于新的 Cache Line

    alignas(64) EventTypeHash *m_typeBuffer; // uint32_t
    alignas(64) uint32_t *m_senderBuffer;    // uint32_t
    alignas(64) uint64_t *m_timeBuffer;      // uint64_t

    // 物理内存池：统一 Slot 大小的连续内存块
    // 每个 Slot 大小固定为 DEFAULT_SLOT_SIZE (32 字节)
    // 布局：| Slot 0 (32B) | Slot 1 (32B) | Slot 2 (32B) | ... |
    // 当 ENTT 集成后，此功能应被移除或简化为只存指针。
    alignas(64) uint8_t *m_dataPool;

    // 原子写索引，用于无锁分配（SoA 元数据）
    std::atomic<uint32_t> m_writeIndex;
};

} // namespace Event
} // namespace System
} // namespace DX12Engine