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
    inline uint32_t GetOverflowCountLastFrame() const { return m_lastFrameOverflowCount; }

    inline const EventTypeHash *GetTypeBuffer() const { return m_currentFrameTypeBuffer; }
    inline const uint32_t *GetSenderBuffer() const { return m_currentFrameSenderBuffer; }
    inline const uint64_t *GetTimestampBuffer() const { return m_currentFrameTimeBuffer; }

    /**
     * @brief 获取当前帧的 Payload 数组
     * @return uint64_t* 指向连续内存，每个元素遵循上述位布局约定
     */
    inline const uint64_t *GetPayloadBuffer() const { return m_currentFramePayloadBuffer; }

    static constexpr MessageIndex INVALID_INDEX = 0xFFFFFFFF;

private:
    uint32_t m_capacity;

    // --- 帧间双缓冲结构 (纯 SoA，无 Blob) ---
    struct FrameData {
        EventTypeHash *typeBuffer;
        uint32_t *senderBuffer;
        uint64_t *timeBuffer;
        uint64_t *payloadBuffer; // ✅ 升级为 64 位，遵循位布局约定

        uint32_t messageCount;

        FrameData()
            : typeBuffer(nullptr), senderBuffer(nullptr), timeBuffer(nullptr), payloadBuffer(nullptr), messageCount(0) {
        }

        void Reset() { messageCount = 0; }
    };

    FrameData m_frames[2];
    int m_currentFrameIndex;

    // 监控数据
    std::atomic<uint32_t> m_lastFrameOverflowCount{0};

    // 当前帧视图
    uint32_t m_currentFrameMessageCount;
    const EventTypeHash *m_currentFrameTypeBuffer;
    const uint32_t *m_currentFrameSenderBuffer;
    const uint64_t *m_currentFrameTimeBuffer;
    const uint64_t *m_currentFramePayloadBuffer; // ✅

    // TLS 注册表
    std::atomic<uint32_t> m_tlsContextCount;
    TLSContext *m_tlsContexts[MAX_TLS_CONTEXTS];

    void EnsureTLSBufferInitialized(TLSContext &ctx);
    void AllocateFrameMemory(FrameData &frame);
    void FreeFrameMemory(FrameData &frame);
};

} // namespace Event
} // namespace System
} // namespace DX12Engine