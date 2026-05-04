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

void MessageArena::AllocateFrameMemory(FrameData &frame) {
    // 分配四个定长数组
    frame.typeBuffer =
        static_cast<EventTypeHash *>(ArenaAlignedMalloc(m_capacity * sizeof(EventTypeHash), ALIGNMENT_CACHE_LINE));
    frame.senderBuffer =
        static_cast<uint32_t *>(ArenaAlignedMalloc(m_capacity * sizeof(uint32_t), ALIGNMENT_CACHE_LINE));
    frame.timeBuffer = static_cast<uint64_t *>(ArenaAlignedMalloc(m_capacity * sizeof(uint64_t), ALIGNMENT_CACHE_LINE));

    // ✅ Payload 现在是 uint64_t 数组
    frame.payloadBuffer =
        static_cast<uint64_t *>(ArenaAlignedMalloc(m_capacity * sizeof(uint64_t), ALIGNMENT_CACHE_LINE));

    if (!frame.typeBuffer || !frame.senderBuffer || !frame.timeBuffer || !frame.payloadBuffer) {
        ARENA_DBG("Failed to allocate frame memory");
    }
}

void MessageArena::FreeFrameMemory(FrameData &frame) {
    ArenaAlignedFree(frame.typeBuffer);
    ArenaAlignedFree(frame.senderBuffer);
    ArenaAlignedFree(frame.timeBuffer);
    ArenaAlignedFree(frame.payloadBuffer);

    frame.typeBuffer = nullptr;
    frame.senderBuffer = nullptr;
    frame.timeBuffer = nullptr;
    frame.payloadBuffer = nullptr;
}

MessageArena::MessageArena(uint32_t capacity)
    : m_capacity(capacity), m_currentFrameIndex(0), m_currentFrameMessageCount(0), m_currentFrameTypeBuffer(nullptr),
      m_currentFrameSenderBuffer(nullptr), m_currentFrameTimeBuffer(nullptr), m_currentFramePayloadBuffer(nullptr),
      m_tlsContextCount(0), m_lastFrameOverflowCount(0) {
    memset(m_tlsContexts, 0, sizeof(m_tlsContexts));

    AllocateFrameMemory(m_frames[0]);
    AllocateFrameMemory(m_frames[1]);
}

MessageArena::~MessageArena() {
    FreeFrameMemory(m_frames[0]);
    FreeFrameMemory(m_frames[1]);
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

void MessageArena::FlushAllTLS() {
    int nextFrameIndex = (m_currentFrameIndex + 1) % 2;
    FrameData &nextFrame = m_frames[nextFrameIndex];
    nextFrame.Reset();

    uint64_t frameTimestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::high_resolution_clock::now().time_since_epoch())
                                  .count();

    uint32_t overflowCount = 0;
    uint32_t contextCount = m_tlsContextCount.load(std::memory_order_acquire);

    for (uint32_t i = 0; i < contextCount; ++i) {
        TLSContext *tlsPtr = m_tlsContexts[i];
        if (!tlsPtr || tlsPtr->localCount == 0)
            continue;

        TLSContext &tls = *tlsPtr;
        uint32_t count = tls.localCount;
        uint32_t remainingCapacity = m_capacity - nextFrame.messageCount;

        if (remainingCapacity == 0) {
            overflowCount += count;
            tls.Reset();
            continue;
        }

        uint32_t toCopyCount = (count <= remainingCapacity) ? count : remainingCapacity;
        if (count > remainingCapacity) {
            overflowCount += (count - toCopyCount);
        }

        // ✅ 批量拷贝：直接赋值，无内存跳转
        for (uint32_t j = 0; j < toCopyCount; ++j) {
            const auto &meta = tls.localMetas[j];
            uint32_t globalIdx = nextFrame.messageCount;

            nextFrame.typeBuffer[globalIdx] = meta.typeHash;
            nextFrame.senderBuffer[globalIdx] = meta.senderId;
            nextFrame.timeBuffer[globalIdx] = frameTimestamp;
            nextFrame.payloadBuffer[globalIdx] = meta.payloadData; // ✅ 直接存句柄

            nextFrame.messageCount++;
        }

        tls.Reset();

        if (nextFrame.messageCount >= m_capacity) {
            for (uint32_t k = i + 1; k < contextCount; ++k) {
                if (m_tlsContexts[k])
                    m_tlsContexts[k]->Reset();
            }
            break;
        }
    }

    m_lastFrameOverflowCount.store(overflowCount, std::memory_order_release);
    if (overflowCount > 0) {
        ARENA_DBG("Frame Overflow Detected: Dropped " << overflowCount << " messages.");
    }

    // 切换视图
    m_currentFrameIndex = nextFrameIndex;
    m_currentFrameMessageCount = nextFrame.messageCount;
    m_currentFrameTypeBuffer = nextFrame.typeBuffer;
    m_currentFrameSenderBuffer = nextFrame.senderBuffer;
    m_currentFrameTimeBuffer = nextFrame.timeBuffer;
    m_currentFramePayloadBuffer = nextFrame.payloadBuffer; // ✅
}

void MessageArena::ResetFrame() { ARENA_DBG("Frame Reset."); }

} // namespace Event
} // namespace System
} // namespace DX12Engine