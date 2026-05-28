#include "MessageDispatcher.h"

#include "Common/Common.h"

namespace DX12Engine::Event {

MessageDispatcher *MessageDispatcher::GetInstance() {
    std::shared_lock lock(s_mutex);
    return s_instance;
}

void MessageDispatcher::Init(uint32_t arenaCapacity, uint32_t bucketCapacity) {
    std::unique_lock lock(s_mutex);
    if (s_isInitialized) {
        return;
    }

    if (!s_instance) {
        s_instance = new MessageDispatcher();
    }
    s_instance->m_arena = std::make_unique<MessageArena>(arenaCapacity);
    s_instance->m_bucketManager.Initialize(bucketCapacity);
    s_isInitialized = true;
}

void MessageDispatcher::Shutdown() {
    std::unique_lock lock(s_mutex);
    if (!s_isInitialized) {
        return;
    }

    s_isInitialized = false;
    delete s_instance;
    s_instance = nullptr;
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
    outIndices.reserve(budget.hardLimit);

    // 关键：先刷新 TLS 缓冲区，确保消息从线程本地写入全局缓冲区
    if (m_arena) {
        m_arena->FlushAllTLS();
    }

    uint32_t processedCount = 0;
    auto startTime = std::chrono::high_resolution_clock::now();

    while (processedCount < budget.hardLimit) {
        // 1. 硬限制检查：时间片
        auto now = std::chrono::high_resolution_clock::now();
        uint64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(now - startTime).count();
        if (elapsedUs > budget.maxTimeUs) {
            break;
        }

        // 2. 从 BucketManager 获取下一个最高优先级消息
        MessageIndex index;
        EventPriority priority;

        if (!m_bucketManager.PopNextMessage(index, priority)) {
            break;
        }

        // 3. 加入批次
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

} // namespace DX12Engine::Event