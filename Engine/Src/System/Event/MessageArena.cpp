#include "System/Event/MessageArena.h"
#include <algorithm> // std::min
#include <cstdlib>   // malloc, free
#include <cstring>   // memset
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <malloc.h> // _aligned_malloc, _aligned_free
#define ArenaAlignedMalloc(size, alignment) _aligned_malloc(size, alignment)
#define ArenaAlignedFree(ptr) _aligned_free(ptr)
#else
#include <stdlib.h>
#define ArenaAlignedMalloc(size, alignment) aligned_alloc(alignment, size)
#define ArenaAlignedFree(ptr) free(ptr)
#endif

// ===== 调试日志到文件 =====
#define ARENA_DEBUG 1
#if ARENA_DEBUG
#include <chrono>
#include <iomanip>

namespace {
std::mutex g_logMutex;
std::ofstream g_debugLogFile;
bool g_debugInitialized = false;

void InitDebugLogFile() {
    if (!g_debugInitialized) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (!g_debugInitialized) {
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

            char filename[64];
            struct tm localTm;
            localtime_s(&localTm, &time_t_now);
            strftime(filename, sizeof(filename), "arena_debug_%Y%m%d_%H%M%S", &localTm);

            char fullFilename[80];
            snprintf(fullFilename, sizeof(fullFilename), "%s_%03lld.log", filename, (long long)ms.count());

            g_debugLogFile.open(fullFilename, std::ios::out | std::ios::trunc);
            g_debugInitialized = true;
        }
    }
}

void ArenaDebugLog(const char *msg) {
    InitDebugLogFile();
    if (g_debugLogFile.is_open()) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;

        struct tm localTm;
        localtime_s(&localTm, &time_t_now);
        char timeStr[32];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &localTm);

        g_debugLogFile << "[" << timeStr << "." << std::setfill('0') << std::setw(6) << us.count() << "] [Arena] "
                       << msg << "\n";
        g_debugLogFile.flush();
    }
}
} // namespace

#define ARENA_DBG(msg)                                                                                                 \
    do {                                                                                                               \
        std::ostringstream ss;                                                                                         \
        ss << msg;                                                                                                     \
        ArenaDebugLog(ss.str().c_str());                                                                               \
    } while (0)
#else
#define ARENA_DBG(msg) ((void)0)
#endif

namespace DX12Engine {
namespace System {
namespace Event {

// TLS 键：每个线程独立的 Arena 上下文
// 注意：这是一个简化的 TLS 实现，生产环境可考虑使用 tbb::task_arena 或自定义线程池
thread_local MessageArena::TLSContext *g_tlsContext = nullptr;

MessageArena::TLSContext &MessageArena::GetTLSContext() {
    thread_local TLSContext tlsInstance;
    return tlsInstance;
}

MessageArena::MessageArena(uint32_t capacity, size_t poolSize)
    : m_capacity(capacity), m_poolSize(poolSize), m_writeIndex(0) {
    // 使用 _aligned_malloc 分配 SoA 元数据缓冲区
    // 确保按 Cache Line (64字节) 对齐，消除伪共享
    m_typeBuffer =
        static_cast<EventTypeHash *>(ArenaAlignedMalloc(capacity * sizeof(EventTypeHash), ALIGNMENT_CACHE_LINE));
    m_senderBuffer = static_cast<uint32_t *>(ArenaAlignedMalloc(capacity * sizeof(uint32_t), ALIGNMENT_CACHE_LINE));
    m_timeBuffer = static_cast<uint64_t *>(ArenaAlignedMalloc(capacity * sizeof(uint64_t), ALIGNMENT_CACHE_LINE));

    // 分配物理内存池：容量 * 固定 Slot 大小
    // 统一 Slot 大小，确保所有事件存储布局一致
    // 按 16 字节对齐，确保 SSE 指令可以安全访问
    size_t actualPoolSize = static_cast<size_t>(capacity) * DEFAULT_SLOT_SIZE;
    m_dataPool = static_cast<uint8_t *>(ArenaAlignedMalloc(actualPoolSize, ALIGNMENT_SSE));

    if (!m_typeBuffer || !m_senderBuffer || !m_timeBuffer || !m_dataPool) {
        // 处理分配失败
        ArenaAlignedFree(m_typeBuffer);
        ArenaAlignedFree(m_senderBuffer);
        ArenaAlignedFree(m_timeBuffer);
        ArenaAlignedFree(m_dataPool);
        throw std::bad_alloc();
    }

    // 清零元数据缓冲区
    std::memset(m_typeBuffer, 0, capacity * sizeof(EventTypeHash));
    std::memset(m_senderBuffer, 0, capacity * sizeof(uint32_t));
    std::memset(m_timeBuffer, 0, capacity * sizeof(uint64_t));
}

MessageArena::~MessageArena() {
    ArenaAlignedFree(m_typeBuffer);
    ArenaAlignedFree(m_senderBuffer);
    ArenaAlignedFree(m_timeBuffer);
    ArenaAlignedFree(m_dataPool);
}

MessageIndex MessageArena::AllocateSlot() {
    // 使用顺序一致性，确保多线程安全
    uint32_t index = m_writeIndex.fetch_add(1, std::memory_order_seq_cst);

    if (index >= m_capacity) {
        // 溢出处理：回滚并返回无效索引
        m_writeIndex.fetch_sub(1, std::memory_order_seq_cst);
        return INVALID_INDEX;
    }

    return index;
}

void MessageArena::WriteMessage(MessageIndex index, EventTypeHash typeHash, uint32_t senderId, void *payloadPtr,
                                size_t payloadSize) {
    if (index == INVALID_INDEX || index >= m_capacity) {
        ARENA_DBG("WriteMessage: invalid index " << index);
        return;
    }

    // 限制 payload 大小，防止截断（但统一 Slot 仍会复制 safeSize 字节）
    size_t safeSize = std::min(payloadSize, DEFAULT_SLOT_SIZE);

    // 写入对应的 SoA 数组
    m_typeBuffer[index] = typeHash;
    m_senderBuffer[index] = senderId;

    // 物理内存池：统一 Slot 大小存储
    // 每个 Slot 固定为 DEFAULT_SLOT_SIZE (32 字节)，无需运行时对齐计算
    if (payloadPtr && safeSize > 0) {
        // 直接通过 index * SLOT_SIZE 计算地址，无需 CAS 竞争
        uint8_t *slotPtr = m_dataPool + static_cast<size_t>(index) * DEFAULT_SLOT_SIZE;

        // 记录写入信息（调试）
        uint32_t *data32 = static_cast<uint32_t *>(payloadPtr);
        ARENA_DBG("WriteMessage: idx=" << index << ", slotOffset=" << (index * DEFAULT_SLOT_SIZE) << ", size="
                                       << safeSize << ", data[0]=" << data32[0] << ", data[1]=" << data32[1]);

        // 执行 memcpy 到固定 Slot
        std::memcpy(slotPtr, payloadPtr, safeSize);

        // 如果实际数据小于 Slot 大小，清零剩余部分（防止垃圾数据）
        if (safeSize < DEFAULT_SLOT_SIZE) {
            std::memset(slotPtr + safeSize, 0, DEFAULT_SLOT_SIZE - safeSize);
        }
    }

    // 记录当前高精度时间戳
    auto now = std::chrono::high_resolution_clock::now();
    m_timeBuffer[index] = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

    // 内存屏障：确保所有写入对消费者线程可见
    std::atomic_thread_fence(std::memory_order_release);
}

void MessageArena::FlushAllTLS() {
    // TLS 缓冲已暂时禁用（统一 Slot 模式下，生产者直接写入 Arena）
    // 此函数保留供未来 TLS 功能恢复时使用
    TLSContext &tls = GetTLSContext();
    tls.offset = 0;
    tls.flushed = true;
}

void MessageArena::ResetFrame() {
    // 重置元数据写指针
    m_writeIndex.store(0, std::memory_order_release);

    // 重置物理内存池使用量（统一 Slot 模式下按 Slot 数量计算）
    // 注意：这意味着上一帧的数据在帧末会被覆盖
    // 如果需要持久化，应该在 ENTT 集成后使用实体组件存储

    // 重置所有线程的 TLS 上下文
    // 注意：这必须在所有生产者线程完成后再调用
    TLSContext &tls = GetTLSContext();
    tls.offset = 0;
    tls.flushed = false;
}

} // namespace Event
} // namespace System
} // namespace DX12Engine