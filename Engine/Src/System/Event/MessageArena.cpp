#include "System/Event/MessageArena.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <malloc.h>
#define ArenaAlignedMalloc(size, alignment) _aligned_malloc(size, alignment)
#define ArenaAlignedFree(ptr) _aligned_free(ptr)
#else
#include <stdlib.h>
#define ArenaAlignedMalloc(size, alignment) aligned_alloc(alignment, size)
#define ArenaAlignedFree(ptr) free(ptr)
#endif

#define ARENA_DBG(msg) ((void)0)

namespace DX12Engine {
namespace System {
namespace Event {

thread_local MessageArena::TLSContext *g_currentTlsContext = nullptr;

MessageArena::TLSContext &MessageArena::GetTLSContext() {
    thread_local TLSContext tlsInstance;
    if (!g_currentTlsContext) {
        g_currentTlsContext = &tlsInstance;
    }
    return tlsInstance;
}

void MessageArena::RegisterTLSContext(TLSContext *ctx) {
    if (ctx->isRegistered.load(std::memory_order_acquire)) {
        return;
    }

    uint32_t index = m_tlsContextCount.fetch_add(1, std::memory_order_relaxed);
    if (index < MAX_TLS_CONTEXTS) {
        m_tlsContexts[index] = ctx;
        ctx->isRegistered.store(true, std::memory_order_release);
    } else {
        ARENA_DBG("Too many TLS contexts registered!");
    }
}

void MessageArena::AllocateRingMemory() {
    // 分配环形缓冲区的四个定长数组
    m_ring.typeBuffer =
        static_cast<EventTypeHash *>(ArenaAlignedMalloc(RING_BUFFER_SIZE * sizeof(EventTypeHash), ALIGNMENT_CACHE_LINE));
    m_ring.senderBuffer =
        static_cast<uint32_t *>(ArenaAlignedMalloc(RING_BUFFER_SIZE * sizeof(uint32_t), ALIGNMENT_CACHE_LINE));
    m_ring.timeBuffer = static_cast<uint64_t *>(ArenaAlignedMalloc(RING_BUFFER_SIZE * sizeof(uint64_t), ALIGNMENT_CACHE_LINE));
    m_ring.payloadBuffer =
        static_cast<uint64_t *>(ArenaAlignedMalloc(RING_BUFFER_SIZE * sizeof(uint64_t), ALIGNMENT_CACHE_LINE));

    if (!m_ring.typeBuffer || !m_ring.senderBuffer || !m_ring.timeBuffer || !m_ring.payloadBuffer) {
        ARENA_DBG("Failed to allocate ring buffer memory");
    }
}

void MessageArena::FreeRingMemory() {
    ArenaAlignedFree(m_ring.typeBuffer);
    ArenaAlignedFree(m_ring.senderBuffer);
    ArenaAlignedFree(m_ring.timeBuffer);
    ArenaAlignedFree(m_ring.payloadBuffer);

    m_ring.typeBuffer = nullptr;
    m_ring.senderBuffer = nullptr;
    m_ring.timeBuffer = nullptr;
    m_ring.payloadBuffer = nullptr;
}

MessageArena::MessageArena(uint32_t capacity)
    : m_globalIndexCounter(0),
      m_currentFrameStartIndex(0),
      m_currentFrameMessageCount(0),
      m_currentFrameTypeBuffer(nullptr),
      m_currentFrameSenderBuffer(nullptr),
      m_currentFrameTimeBuffer(nullptr),
      m_currentFramePayloadBuffer(nullptr),
      m_tlsContextCount(0),
      m_overflowCount(0),
      m_lastFlushedIndex(0) {
    memset(m_tlsContexts, 0, sizeof(m_tlsContexts));
    AllocateRingMemory();
}

MessageArena::~MessageArena() {
    FreeRingMemory();
}

void MessageArena::EnsureTLSBufferInitialized(TLSContext &ctx) {
    if (!ctx.isRegistered.load(std::memory_order_acquire)) {
        RegisterTLSContext(&ctx);
    }
}

// ✅ 简化后的 WriteMessage：直接存句柄
void MessageArena::WriteMessage(MessageIndex index, EventTypeHash typeHash, uint32_t senderId, uint64_t payloadData) {
    TLSContext &tls = GetTLSContext();
    EnsureTLSBufferInitialized(tls);

    if (tls.localCount >= TLSContext::MAX_LOCAL_MESSAGES) {
        ARENA_DBG("TLS Buffer Full! Message dropped.");
        return;
    }

    auto &meta = tls.localMetas[tls.localCount++];
    meta.typeHash = typeHash;
    meta.senderId = senderId;

    // 直接存储打包好的 64 位数据
    // 调用者需确保遵循：低32位=主值/句柄, 高32位=辅值
    meta.payloadData = payloadData;
}

MessageIndex MessageArena::WriteMessageAndGetIndex(EventTypeHash typeHash, uint32_t senderId, uint64_t payloadData) {
    // 1. 原子分配全局索引 (多线程安全)
    uint32_t globalIndex = m_globalIndexCounter.fetch_add(1, std::memory_order_relaxed);

    // 2. 计算环形缓冲区槽位
    uint32_t ringSlot = globalIndex % RING_BUFFER_SIZE;

    // 3. 熔断器检查：防止覆盖未读取的数据
    // 如果新消息距离上次刷新位置超过 RING_BUFFER_SIZE，说明有覆盖风险
    uint32_t lastFlushed = m_lastFlushedIndex.load(std::memory_order_acquire);
    uint32_t distance = globalIndex - lastFlushed;

    if (distance >= RING_BUFFER_SIZE) {
        // 环形缓冲区溢出，数据将被覆盖
        // 记录溢出次数（用于监控）
        m_overflowCount.fetch_add(1, std::memory_order_relaxed);
        ARENA_DBG("Ring buffer overflow detected! Message will overwrite unread data.");
        // 不返回 INVALID_INDEX，让写入继续（熔断不断流，只是记录）
        // 如果需要严格保护，可改为: return INVALID_INDEX;
    }

    // 4. 获取 TLS 上下文
    TLSContext &tls = GetTLSContext();
    EnsureTLSBufferInitialized(tls);

    // 5. 检查 TLS 本地缓冲区是否已满
    if (tls.localCount >= TLSContext::MAX_LOCAL_MESSAGES) {
        ARENA_DBG("TLS Buffer Full! Message dropped.");
        return INVALID_INDEX;
    }

    // 6. 写入 TLS 并记录槽位索引（用于后续直接寻址）
    auto &meta = tls.localMetas[tls.localCount++];
    meta.typeHash = typeHash;
    meta.senderId = senderId;
    meta.payloadData = payloadData;
    meta.assignedIndex = ringSlot; // ✅ 关键：存储槽位索引，而非全局ID

    // 返回槽位索引（用于 BucketManager 的 Aging 机制）
    return static_cast<MessageIndex>(ringSlot);
}

void MessageArena::FlushAllTLS() {
    uint64_t frameTimestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::high_resolution_clock::now().time_since_epoch())
                                  .count();

    uint32_t contextCount = m_tlsContextCount.load(std::memory_order_acquire);
    uint32_t maxRingSlot = 0; // 追踪本帧写入的最大槽位
    uint32_t messagesWritten = 0;

    // 记录本帧开始时的全局索引
    uint32_t thisFrameStartIndex = m_globalIndexCounter.load(std::memory_order_relaxed);

    for (uint32_t i = 0; i < contextCount; ++i) {
        TLSContext *tlsPtr = m_tlsContexts[i];
        if (!tlsPtr || tlsPtr->localCount == 0)
            continue;

        TLSContext &tls = *tlsPtr;
        uint32_t count = tls.localCount;

        for (uint32_t j = 0; j < count; ++j) {
            const auto &meta = tls.localMetas[j];
            uint32_t ringSlot = meta.assignedIndex; // ✅ 槽位索引

            // 直接写入环形缓冲区的指定槽位
            m_ring.typeBuffer[ringSlot] = meta.typeHash;
            m_ring.senderBuffer[ringSlot] = meta.senderId;
            m_ring.timeBuffer[ringSlot] = frameTimestamp;
            m_ring.payloadBuffer[ringSlot] = meta.payloadData;

            // 追踪最大槽位
            if (ringSlot > maxRingSlot) {
                maxRingSlot = ringSlot;
            }
            messagesWritten++;
        }

        tls.Reset();
    }

    // 更新已刷新的最新索引（用于熔断器检测）
    uint32_t currentIndex = m_globalIndexCounter.load(std::memory_order_relaxed);
    m_lastFlushedIndex.store(currentIndex, std::memory_order_release);

    // 设置当前帧视图 - 使用环形缓冲区的窗口
    m_currentFrameStartIndex = thisFrameStartIndex;
    m_currentFrameMessageCount = messagesWritten;

    // 更新只读视图指针（指向环形缓冲区）
    m_currentFrameTypeBuffer = m_ring.typeBuffer;
    m_currentFrameSenderBuffer = m_ring.senderBuffer;
    m_currentFrameTimeBuffer = m_ring.timeBuffer;
    m_currentFramePayloadBuffer = m_ring.payloadBuffer;
}
void MessageArena::ResetFrame() {
    // ✅ 不再重置 m_globalIndexCounter！
    // 环形缓冲区的优势：全局索引一直递增，取模自动映射到槽位
    // 这样 Aging 机制使用的槽位索引始终有效
    ARENA_DBG("Frame Reset. Ring buffer index continues from: " << m_globalIndexCounter.load());
}

} // namespace Event
} // namespace System
} // namespace DX12Engine