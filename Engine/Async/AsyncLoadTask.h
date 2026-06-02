#pragma once

#include "Event/EventTypes.h"
#include "Event/MessageDispatcher.h"
#include "Scheduler/Task.h"
#include <string>

namespace DX12Engine::Async {

/**
 * @brief 异步加载任务基类
 *
 * 特点：
 * - 在 Worker 线程执行
 * - 执行完成后自动发送事件
 * - 支持 requestId 关联多个任务
 * - 不经过 System 层，直接提交到 TaskGraph
 *
 * @tparam HandleType 资源句柄类型（GeometryHandle/MaterialHandle/TextureHandle）
 * @tparam EventHash   完成事件哈希值
 */
template <typename HandleType, uint32_t EventHash> class AsyncLoadTask : public Scheduler::Task {
public:
    AsyncLoadTask(uint32_t requestId, const std::string &path, const std::string &taskName = "")
        : m_requestId(requestId), m_path(path) {
        this->name = taskName.empty() ? typeid(HandleType).name() : taskName;
        this->phase = Scheduler::TaskPhase::Update;
        this->thread = Scheduler::ThreadType::Worker;
        this->priority = static_cast<uint32_t>(Scheduler::TaskPriority::Background);
    }

    void Execute() override {
        // 1. 执行实际加载
        HandleType handle = LoadInternal(m_path);

        // 2. 发送完成事件
        if (handle.IsValid()) {
            uint64_t payload = PackPayload(m_requestId, handle);
            Event::MessageDispatcher::GetInstance()->PostEvent(EventHash,
                                                               0, // senderId
                                                               payload, Event::EventPriority::P4_Background);
        } else {
            OnLoadFailed();
        }
    }

protected:
    virtual HandleType LoadInternal(const std::string &path) = 0;

    virtual void OnLoadFailed() {
        // 默认：发送失败事件（payload 只包含 requestId，handle 为 0）
        uint64_t payload = static_cast<uint64_t>(m_requestId) << 32;
        Event::MessageDispatcher::GetInstance()->PostEvent(EventHash + 1, // 约定失败事件哈希 = 成功哈希 + 1
                                                           0, payload, Event::EventPriority::P4_Background);
    }

    uint64_t PackPayload(uint32_t requestId, HandleType handle) {
        return (static_cast<uint64_t>(requestId) << 42) | (static_cast<uint64_t>(handle.generation) << 32) |
               (static_cast<uint64_t>(handle.index) & 0xFFFFFFFF);
    }

protected:
    uint32_t m_requestId; // 请求 ID，用于关联多个资源
    std::string m_path;   // 资源路径
};

} // namespace DX12Engine::Async