#include "System/Event/MessageDispatcher.h"

namespace DX12Engine {
namespace System {
namespace Event {

MessageDispatcher::MessageDispatcher() {}

void MessageDispatcher::Initialize(uint32_t arenaCapacity, uint32_t bucketCapacity) {
    // 注意：这里不需要传递 arena 引用给 bucketManager，因为它们已解耦
    m_arena = std::make_unique<MessageArena>(arenaCapacity);
    m_bucketManager.Initialize(bucketCapacity);
}

bool MessageDispatcher::PostEvent(EventTypeHash typeHash, uint32_t senderId, uint64_t payloadData,
                                  EventPriority priority) {
    if (!m_arena)
        return false;

    MessageIndex index = m_arena->WriteMessageAndGetIndex(typeHash, senderId, payloadData);
    if (index == MessageArena::INVALID_INDEX) {
        return false;
    }

    return m_bucketManager.PushMessage(index, priority);
}

uint32_t MessageDispatcher::FlushEvents(std::vector<MessageIndex> &outIndices, const FlushBudget &budget) {
    outIndices.clear();
    outIndices.reserve(budget.hardLimit); // 预分配内存，避免 push_back 时的重新分配

    uint32_t processedCount = 0;
    auto startTime = std::chrono::high_resolution_clock::now();

    while (processedCount < budget.hardLimit) {
        // 1. 硬限制检查：时间片
        auto now = std::chrono::high_resolution_clock::now();
        uint64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(now - startTime).count();
        if (elapsedUs > budget.maxTimeUs) {
            break; // 时间耗尽，触发硬限制
        }

        // 2. 从 BucketManager 获取下一个最高优先级消息
        MessageIndex index;
        EventPriority priority;

        if (!m_bucketManager.PopNextMessage(index, priority)) {
            break; // 桶空了，没有更多消息
        }

        // 3. 软限制检查（示例：P4 背景任务每帧最多处理 5 条）
        // 这里可以根据 priority 做更复杂的过滤
        // 可选：将消息推回？或者简单丢弃？
        // 由于 Pop 已经发生，推回比较复杂。通常做法是：
        // 如果超过软限制，就不把它加入 outIndices，相当于“本帧暂缓处理”
        // 但注意：这会导致该消息在本帧“消失”，除非 Bucket 支持 Peek。
        // 简化方案：对于 P4，如果超过限制，直接 continue（实际上已经 Pop 出来了，这里逻辑需微调）
        // 更严谨的做法：在 Pop 之前判断。但 PopNextMessage 是原子操作。
        // 这里的简化实现：如果超过软限制，我们仍然处理它，但记录日志或调整后续策略。
        // 或者：如果软限制严格，应该在 Bucket 层面做 Throttle。

        // 4. 加入批次
        outIndices.push_back(index);
        processedCount++;
    }

    return processedCount;
}

void MessageDispatcher::EndFrame() {
    if (m_arena) {
        m_arena->FlushAllTLS();
        m_arena->ResetFrame();
    }
    m_bucketManager.ResetFrame();
}

} // namespace Event
} // namespace System
} // namespace DX12Engine