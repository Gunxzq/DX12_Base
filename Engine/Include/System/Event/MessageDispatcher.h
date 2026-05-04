#pragma once

#include "System/Event/BucketManager.h"
#include "System/Event/Event.h"
#include "System/Event/MessageArena.h"
#include <cstdint>
#include <vector>

namespace DX12Engine {
namespace System {
namespace Event {

struct FlushBudget {
    uint32_t hardLimit; // 硬限制：最多处理多少条
    uint64_t maxTimeUs; // 时间限制：最多花费多少微秒
};

/**
 * @brief 消息分发器（中间层）
 *
 * 职责：
 * 1. 封装 MessageArena 和 BucketManager 的交互，提供统一的 Post 接口。
 * 2. 执行“双阀门”预算控制，批量获取消息供调度器使用。
 * 3. 确保“写入 Arena”与“入桶”的原子性（要么都成功，要么都失败）。
 */
class MessageDispatcher {
public:
    MessageDispatcher();
    ~MessageDispatcher() = default;

    /**
     * @brief 初始化分发器
     * @param arenaCapacity Arena 容量
     * @param bucketCapacity 每个桶的容量
     */
    void Initialize(uint32_t arenaCapacity = 65536, uint32_t bucketCapacity = 2048);

    /**
     * @brief 【核心接口】发布事件
     *
     * 步骤：
     * 1. 写入 MessageArena 获取 Index。
     * 2. 将 Index 推入对应优先级的 Bucket。
     *
     * @param typeHash 事件类型哈希
     * @param senderId 发送者ID
     * @param payloadData 负载数据（64位打包值）
     * @param priority 事件优先级
     * @return true 成功发布, false 失败（Arena满或Bucket满/丢弃策略触发）
     */
    bool PostEvent(EventTypeHash typeHash, uint32_t senderId, uint64_t payloadData, EventPriority priority);

    /**
     * @brief 【便捷重载】发布双值事件（如 WindowResize）
     */
    inline bool PostEvent(EventTypeHash typeHash, uint32_t senderId, uint32_t val1, uint32_t val2,
                          EventPriority priority) {
        uint64_t packed = (static_cast<uint64_t>(val2) << 32) | static_cast<uint64_t>(val1);
        return PostEvent(typeHash, senderId, packed, priority);
    }

    /**
     * @brief 【便捷重载】发布单值/句柄事件
     */
    inline bool PostEvent(EventTypeHash typeHash, uint32_t senderId, uint32_t handleOrValue, EventPriority priority) {
        return PostEvent(typeHash, senderId, static_cast<uint64_t>(handleOrValue), priority);
    }

    /**
     * @brief 【双阀门核心】批量获取消息索引
     * @param outIndices 输出的索引列表
     * @param budget 预算限制
     * @return 实际获取的消息数量
     */
    uint32_t FlushEvents(std::vector<MessageIndex> &outIndices, const FlushBudget &budget);

    /**
     * @brief 帧结束清理
     * 重置 Arena 和 BucketManager 的帧统计信息
     */
    void EndFrame();

    // --- 供调度器使用的访问接口 ---

    inline MessageArena &GetArena() { return *m_arena; }
    inline BucketManager &GetBucketManager() { return m_bucketManager; }

private:
    std::unique_ptr<MessageArena> m_arena;
    BucketManager m_bucketManager;
};

} // namespace Event
} // namespace System
} // namespace DX12Engine