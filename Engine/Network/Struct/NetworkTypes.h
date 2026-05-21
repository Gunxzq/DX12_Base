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
    Ping = 0x01,
    Pong = 0x02,
    Connect = 0x03,
    ConnectAck = 0x04,
    Disconnect = 0x05,
    ReliableAck = 0x06,
    ConnectionStatus = 0x07,
    BandwidthEstimate = 0x08,
    // 0x09-0x0F 保留给引擎未来扩展

    // ========== P2P 帧同步 (0x10-0x1F) ==========
    P2PInput = 0x10,    // 输入同步
    P2PFrameAck = 0x11, // 帧确认
    // 0x12-0x1F 保留给 P2P 扩展

    // ========== Client-Server 权威模式 (0x20-0x2F) ==========
    CSInput = 0x20,    // 客户端输入
    CSSnapshot = 0x21, // 服务器快照
    CSDelta = 0x22,    // 增量更新
    CSLobby = 0x23,    // 大厅/匹配消息
    // 0x24-0x2F 保留给 CS 扩展

    // ========== 游戏层自由使用 (0x30-0x7F) ==========
    GameCustom = 0x30, // 游戏自定义起始
    // 0x31-0x7F 完全由游戏开发者自由使用
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