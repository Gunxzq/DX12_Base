#pragma once

#include "Event/MessageDispatcher.h"
#include "Network/NetworkTypes.h"

namespace DX12Engine {
namespace Framework {

using NetworkMessageType = DX12Engine::Network::NetworkMessageType;
using namespace DX12Engine::System::Event;

/**
 * @brief 网络消息辅助类
 *
 * 封装 PostEvent，提供简洁的网络消息发送接口
 */
class NetworkMessageHelper {
public:
    /**
     * @brief 发送直接模式消息（小数据）
     */
    static bool PostDirect(NetworkMessageType type, uint32_t senderId, uint32_t high, uint32_t low,
                           EventPriority priority = EventPriority::P2_Normal) {
        uint64_t payload = (static_cast<uint64_t>(static_cast<uint8_t>(type) & 0x7F) << 56) |
                           (static_cast<uint64_t>(high & 0xFFFFFF) << 32) | static_cast<uint64_t>(low);

        auto *dispatcher = MessageDispatcher::GetInstance();
        if (!dispatcher)
            return false;
        return dispatcher->PostEvent(NetworkPacketEvent::StaticTypeHash, senderId, payload, priority);
    }

    /**
     * @brief 发送 Handle 模式消息（大数据引用）
     */
    static bool PostHandle(NetworkMessageType type, uint32_t senderId, uint64_t handle,
                           EventPriority priority = EventPriority::P1_High) {
        uint64_t payload = (1ULL << 63) | // Bit 63 = 1
                           (static_cast<uint64_t>(static_cast<uint8_t>(type) & 0x7F) << 56) |
                           (handle & 0xFFFFFFFFFFFFFFULL);

        auto *dispatcher = MessageDispatcher::GetInstance();
        if (!dispatcher)
            return false;
        return dispatcher->PostEvent(NetworkPacketEvent::StaticTypeHash, senderId, payload, priority);
    }

    /**
     * @brief 发送游戏自定义消息（便捷接口）
     */
    static bool PostGameMessage(uint32_t senderId, uint32_t high, uint32_t low) {
        return PostDirect(NetworkMessageType::GameCustom, senderId, high, low);
    }
};

} // namespace Framework
} // namespace DX12Engine