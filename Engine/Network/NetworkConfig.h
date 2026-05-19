#pragma once

#include <cstdint>
#include <string>

namespace DX12Engine {
namespace Network {

// 网络模式
enum class NetworkMode : uint8_t {
    Disconnected, // 未连接
    Client,       // 客户端模式
    Server,       // 权威服务器模式
    P2P           // 对等网络模式
};

// 传输方式
enum class DeliveryMethod : uint8_t {
    Unreliable, // 不可靠（UDP风格，允许丢包）
    Reliable,   // 可靠有序（TCP风格）
    Critical    // 关键消息（可靠 + 高优先级）
};

// 连接状态
enum class ConnectionState : uint8_t { Disconnected, Connecting, Connected, Failed };

// 网络配置
struct NetworkConfig {
    // ========== 基础配置 ==========
    NetworkMode mode = NetworkMode::Disconnected;
    bool enabled = false;

    // 本地标识
    uint32_t localPlayerId = 0;
    std::string playerName = "Player";

    // 连接目标（Client模式使用）
    std::string serverAddress = "127.0.0.1";
    uint16_t port = 7777;

    // ========== 性能配置 ==========
    uint32_t tickRate = 60;    // 与引擎帧率对齐
    uint32_t sendRate = 60;    // 发送频率（包/秒）
    uint32_t receiveRate = 60; // 接收轮询频率

    // ========== 可靠性配置 ==========
    uint32_t reliableTimeoutMs = 3000; // 可靠消息超时（毫秒）
    uint32_t maxReliableRetries = 5;   // 最大重试次数
    uint32_t pingIntervalMs = 1000;    // Ping 间隔

    // ========== 带宽控制 ==========
    uint32_t maxSendQueueSize = 256; // 最大发送队列大小
    uint32_t maxPacketSize = 2048;   // 最大包大小

    // ========== 帧同步配置（P2P模式）==========
    uint32_t inputBufferSize = 120;   // 输入缓冲区大小（2秒 @ 60fps）
    uint32_t maxPredictionFrames = 3; // 最大预测帧数

    // ========== 调试 ==========
    bool enableSimulatedLatency = false;
    uint32_t simulatedLatencyMs = 0;
    float simulatedLossRate = 0.0f;

    // ========== 工厂方法 ==========
    static NetworkConfig MakeClient(const std::string &serverAddr, uint16_t port) {
        NetworkConfig cfg;
        cfg.enabled = true;
        cfg.mode = NetworkMode::Client;
        cfg.serverAddress = serverAddr;
        cfg.port = port;
        return cfg;
    }

    static NetworkConfig MakeServer(uint16_t port) {
        NetworkConfig cfg;
        cfg.enabled = true;
        cfg.mode = NetworkMode::Server;
        cfg.port = port;
        return cfg;
    }

    static NetworkConfig MakeP2P(uint16_t port) {
        NetworkConfig cfg;
        cfg.enabled = true;
        cfg.mode = NetworkMode::P2P;
        cfg.port = port;
        return cfg;
    }
};

} // namespace Network
} // namespace DX12Engine