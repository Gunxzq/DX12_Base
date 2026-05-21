#pragma once
#include <cstdint>

namespace DX12Engine {
namespace Network {

// ========== 基础类型（语义化别名）==========
using PlayerId = uint64_t;
using SequenceNumber = uint32_t;
using FrameNumber = uint32_t;
using TimestampMs = uint32_t;
using PacketHandle = uint32_t;

/**
 * @brief 网络消息类型
 *
 * 引擎层保留 0x00-0x0F
 * 游戏层可用 0x10-0x7F
 */
enum class NetworkMessageType : uint8_t {
    // ========== 引擎层 (0x00-0x0F) ==========
    Reserved = 0x00,

    // RTT 测量、连接保活
    Ping = 0x01,
    Pong = 0x02,
    // 连接建立、协议版本协商
    Connect = 0x03,
    ConnectAck = 0x04,

    // 断开
    Disconnect = 0x05,

    // 可靠消息确认
    ReliableAck = 0x06,

    // ========== 状态消息 (0x08-0x0F) ==========
    ConnectionStatus = 0x07,

    // 带宽估计
    BandwidthEstimate = 0x08,
    // 0x09-0x0F 保留给引擎未来扩展

    // ========== 游戏层 (0x10-0x7F) ==========
    GameCustom = 0x10, // 游戏自定义起始
    // 0x11-0x7F 完全由游戏开发者自由使用
};

/**
 * @brief 检查是否为引擎层消息
 */
inline bool IsEngineMessage(NetworkMessageType type) { return static_cast<uint8_t>(type) < 0x10; }

/**
 * @brief 检查是否为游戏层消息
 */
inline bool IsGameMessage(NetworkMessageType type) { return static_cast<uint8_t>(type) >= 0x10; }

} // namespace Network
} // namespace DX12Engine