#pragma once

#include "System/Event/Event.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring> // for memset
#include <mutex>

namespace DX12Engine {
namespace System {
namespace Event {

static constexpr size_t TLS_BUFFER_SIZE = 64 * 1024;
static constexpr size_t ALIGNMENT_CACHE_LINE = 64;
static constexpr uint32_t MAX_TLS_CONTEXTS = 64;

/**
 * @brief 环形缓冲区大小 (2^20 = 1,048,576)
 * 理由：100万条消息 × 32字节 ≈ 32MB，内存可控
 * 足以支撑即使每秒5万条消息积压时，监控系统有20秒反应时间
 */
static constexpr uint32_t RING_BUFFER_SIZE = 1 << 20;

class MessageArena {
public:
    static constexpr uint32_t DEFAULT_CAPACITY = 65536;

    /**
     * @brief TLS 本地缓冲区上下文
     */
    struct alignas(ALIGNMENT_CACHE_LINE) TLSContext {
        struct LocalMeta {
            EventTypeHash typeHash;
            uint32_t senderId;
            uint32_t assignedIndex;

            /**
             * @brief 64位负载数据，采用联合语义存储：
             *
             * 【布局约定】：
             * - 低 32 位 (Bits 0-31):  存储【值类型】或【资源句柄】。
             *   - 如果是简单事件（如 WindowResize），这里存储第一个参数（如 Width）。
             *   - 如果是资源事件，这里存储 ResourceHandle。
             *
             * - 高 32 位 (Bits 32-63): 存储【辅助值】或【扩展信息】。
             *   - 如果是简单事件，这里存储第二个参数（如 Height）。
             *   - 如果是资源事件，通常为 0。
             *
             * 【使用示例】：
             * 1. 资源加载: payload = static_cast<uint64_t>(handle);
             * 2. 窗口变化: payload = (static_cast<uint64_t>(height) << 32) | width;
             */
            uint64_t payloadData;
        };

        static constexpr size_t MAX_LOCAL_MESSAGES = 512;
        LocalMeta localMetas[MAX_LOCAL_MESSAGES];
        uint32_t localCount;
        std::atomic<bool> isRegistered;

        TLSContext() : localCount(0), isRegistered(false) {}

        void Reset() { localCount = 0; }
    };

    static TLSContext &GetTLSContext();
    void RegisterTLSContext(TLSContext *ctx);

    MessageArena(uint32_t capacity = DEFAULT_CAPACITY);
    ~MessageArena();

    MessageArena(const MessageArena &) = delete;
    MessageArena &operator=(const MessageArena &) = delete;

    /**
     * @brief 写入原始 64 位负载数据
     * @param index         消息索引（保留字段，当前未使用）
     * @param typeHash      消息类型哈希
     * @param senderId      发送者 ID
     * @param payloadData   打包后的 64 位数据
     *                      - 低 32 位: 主要值/句柄
     *                      - 高 32 位: 次要值/扩展
     */
    void WriteMessage(MessageIndex index, EventTypeHash typeHash, uint32_t senderId, uint64_t payloadData);

    /**
     * @brief 便捷重载：写入单一句柄或值（自动填充高 32 位为 0）
     */
    inline void WriteMessage(MessageIndex index, EventTypeHash typeHash, uint32_t senderId, uint32_t valueOrHandle) {
        WriteMessage(index, typeHash, senderId, static_cast<uint64_t>(valueOrHandle));
    }

    /**
     * @brief 线程安全地写入消息并获取全局唯一索引
     * 1. 原子分配 Index
     * 2. 将数据写入当前线程的 TLS
     * 3. 在 TLS Meta 中记录该 Index
     * @return MessageIndex 分配的索引，若失败返回 INVALID_INDEX
     */
    MessageIndex WriteMessageAndGetIndex(EventTypeHash typeHash, uint32_t senderId, uint64_t payloadData);

    /**
     * @brief 便捷重载：写入两个 32 位值（如 Width, Height）
     * @param val1 存入低 32 位
     * @param val2 存入高 32 位
     */
    inline void WriteMessage(MessageIndex index, EventTypeHash typeHash, uint32_t senderId, uint32_t val1,
                             uint32_t val2) {
        uint64_t packed = (static_cast<uint64_t>(val2) << 32) | static_cast<uint64_t>(val1);
        WriteMessage(index, typeHash, senderId, packed);
    }

    void FlushAllTLS();
    void ResetFrame();

    // --- 访问接口 ---

    inline uint32_t GetCount() const { return m_currentFrameMessageCount; }
    inline uint32_t GetOverflowCount() const { return m_overflowCount.load(std::memory_order_relaxed); }

    inline const EventTypeHash *GetTypeBuffer() const { return m_currentFrameTypeBuffer; }
    inline const uint32_t *GetSenderBuffer() const { return m_currentFrameSenderBuffer; }
    inline const uint64_t *GetTimestampBuffer() const { return m_currentFrameTimeBuffer; }

    /**
     * @brief 获取当前帧的 Payload 数组
     * @return uint64_t* 指向连续内存，每个元素遵循上述位布局约定
     */
    inline const uint64_t *GetPayloadBuffer() const { return m_currentFramePayloadBuffer; }

    /**
     * @brief 获取当前帧的起始全局索引
     * @note 用于计算环形缓冲区中的有效范围
     */
    inline uint32_t GetCurrentFrameStartIndex() const { return m_currentFrameStartIndex; }

    /**
     * @brief 获取环形缓冲区大小
     */
    static constexpr uint32_t GetRingBufferSize() { return RING_BUFFER_SIZE; }

    static constexpr MessageIndex INVALID_INDEX = 0xFFFFFFFF;

    // ========================================================================
    // 消息完整内容访问接口
    // ========================================================================

    /**
     * @brief 消息完整内容结构体
     * @note 供调度层获取消息的完整数据，包含时间戳
     */
    struct MessageContent {
        EventTypeHash typeHash = 0;
        uint32_t senderId = 0;
        uint64_t payload = 0;
        uint64_t sendTimestamp = 0; // 发送时间（微秒）
    };

    /**
     * @brief 获取消息的完整内容
     * @param index 消息索引
     * @return MessageContent 包含 typeHash, senderId, payload, sendTimestamp
     * @note Arena 只存储数据，不解析 payload；System 层自行解析高低位
     */
    inline MessageContent GetMessage(MessageIndex index) const {
        MessageContent content;
        content.typeHash = m_currentFrameTypeBuffer[index];
        content.senderId = m_currentFrameSenderBuffer[index];
        content.payload = m_currentFramePayloadBuffer[index];
        content.sendTimestamp = m_currentFrameTimeBuffer[index];
        return content;
    }

    /**
     * @brief 获取消息类型哈希
     */
    inline EventTypeHash GetType(MessageIndex index) const { return m_currentFrameTypeBuffer[index]; }

    /**
     * @brief 获取发送者 ID
     */
    inline uint32_t GetSender(MessageIndex index) const { return m_currentFrameSenderBuffer[index]; }

    /**
     * @brief 获取原始 Payload（64位数据）
     * @note System 层使用辅助方法自行解析高低位
     */
    inline uint64_t GetPayload(MessageIndex index) const { return m_currentFramePayloadBuffer[index]; }

    /**
     * @brief 获取发送时间戳（微秒）
     */
    inline uint64_t GetTimestamp(MessageIndex index) const { return m_currentFrameTimeBuffer[index]; }

private:
    // --- 环形缓冲区结构 (纯 SoA) ---
    struct RingBuffer {
        EventTypeHash *typeBuffer;
        uint32_t *senderBuffer;
        uint64_t *timeBuffer;
        uint64_t *payloadBuffer;

        RingBuffer() : typeBuffer(nullptr), senderBuffer(nullptr), timeBuffer(nullptr), payloadBuffer(nullptr) {}
    };

    RingBuffer m_ring;

    // --- 熔断器机制 ---
    // 记录已刷新到环形缓冲区的最新索引，用于检测覆盖风险
    std::atomic<uint32_t> m_lastFlushedIndex{0};
    // 溢出计数（用于监控）
    std::atomic<uint32_t> m_overflowCount{0};

    // --- 当前帧视图 (指向环形缓冲区的只读窗口) ---
    uint32_t m_currentFrameStartIndex;   // 当前帧起始的全局索引
    uint32_t m_currentFrameMessageCount; // 当前帧消息数量
    const EventTypeHash *m_currentFrameTypeBuffer;
    const uint32_t *m_currentFrameSenderBuffer;
    const uint64_t *m_currentFrameTimeBuffer;
    const uint64_t *m_currentFramePayloadBuffer;

    // TLS 注册表
    std::atomic<uint32_t> m_tlsContextCount;
    TLSContext *m_tlsContexts[MAX_TLS_CONTEXTS];

    void EnsureTLSBufferInitialized(TLSContext &ctx);
    void AllocateRingMemory();
    void FreeRingMemory();

    // 全局递增索引（不再每帧重置）
    std::atomic<uint32_t> m_globalIndexCounter;
};

} // namespace Event
} // namespace System
} // namespace DX12Engine