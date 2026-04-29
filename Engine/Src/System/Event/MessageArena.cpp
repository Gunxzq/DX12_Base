#include "System/Event/MessageArena.h"
#include <cstdlib> // malloc, free
#include <cstring> // memset

namespace DX12Engine {
namespace System {
namespace Event {

MessageArena::MessageArena(uint32_t capacity) : m_capacity(capacity), m_writeIndex(0) {
    // 分配内存并初始化为0
    // 注意：生产环境建议使用自定义对齐分配器或 Huge Pages
    m_typeBuffer = static_cast<EventTypeHash *>(std::malloc(capacity * sizeof(EventTypeHash)));
    m_senderBuffer = static_cast<uint32_t *>(std::malloc(capacity * sizeof(uint32_t)));
    m_payloadBuffer = static_cast<void **>(std::malloc(capacity * sizeof(void *)));
    m_timeBuffer = static_cast<uint64_t *>(std::malloc(capacity * sizeof(uint64_t)));

    if (!m_typeBuffer || !m_senderBuffer || !m_payloadBuffer || !m_timeBuffer) {
        // 处理分配失败
        std::abort();
    }

    // 可选：清零内存
    std::memset(m_typeBuffer, 0, capacity * sizeof(EventTypeHash));
    std::memset(m_senderBuffer, 0, capacity * sizeof(uint32_t));
    std::memset(m_payloadBuffer, 0, capacity * sizeof(void *));
    std::memset(m_timeBuffer, 0, capacity * sizeof(uint64_t));
}

MessageArena::~MessageArena() {
    std::free(m_typeBuffer);
    std::free(m_senderBuffer);
    std::free(m_payloadBuffer);
    std::free(m_timeBuffer);
}

MessageIndex MessageArena::AllocateSlot() {
    // 原子增加，返回旧值作为索引
    uint32_t index = m_writeIndex.fetch_add(1, std::memory_order_relaxed);

    if (index >= m_capacity) {
        // 溢出处理：在实际引擎中可能触发熔断或丢弃
        // 这里简单回滚并返回无效索引
        m_writeIndex.fetch_sub(1, std::memory_order_relaxed);
        return INVALID_INDEX;
    }

    return index;
}

void MessageArena::WriteMessage(MessageIndex index, EventTypeHash typeHash, uint32_t senderId, void *payloadPtr) {
    if (index == INVALID_INDEX || index >= m_capacity) {
        return;
    }

    // 直接写入对应的 SoA 数组
    // 由于 index 是唯一的，不同线程写入不同索引，不存在竞争
    m_typeBuffer[index] = typeHash;
    m_senderBuffer[index] = senderId;
    m_payloadBuffer[index] = payloadPtr;

    // 记录当前高精度时间戳
    auto now = std::chrono::high_resolution_clock::now();
    // 转换为纳秒或毫秒的整数表示，节省空间并便于计算 Aging
    m_timeBuffer[index] = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

void MessageArena::ResetFrame() {
    // 简单的环形缓冲策略或每帧重置
    // 这里采用每帧重置写指针，假设消费者在一帧内处理完所有消息
    // 更复杂的实现可能需要 Epoch-based 回收
    m_writeIndex.store(0, std::memory_order_release);
}

} // namespace Event
} // namespace System
} // namespace DX12Engine