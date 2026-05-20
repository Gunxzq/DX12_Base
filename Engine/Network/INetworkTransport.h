// Network/INetworkTransport.h
#pragma once
#include "NetworkConfig.h"
#include "NetworkTypes.h"
#include <functional>
#include <string>
#include <vector>

namespace DX12Engine {
namespace Network {

/**
 * @brief 网络传输层抽象接口
 *
 * 职责：建立连接、收发原始数据
 * 不关心上层是 P2P 还是 Client-Server
 */
class INetworkTransport {
public:
    virtual ~INetworkTransport() = default;

    // ========== 生命周期 ==========

    virtual bool Start(const NetworkConfig &config) = 0;

    virtual void Stop() = 0;

    virtual void Update(float deltaTime) = 0;

    // ========== 连接建立 ==========

    /**
     * @brief 主动连接到指定地址（Client/P2P 模式）
     * @param address IP 地址
     * @param port 端口
     * @return true 连接请求已发出，false 失败
     */
    virtual bool Connect(const std::string &address, uint16_t port) = 0;

    // ========== 发送数据 ==========

    /**
     * @brief 发送数据到指定玩家
     * @param targetPlayerId 目标玩家 ID
     * @param data 数据指针
     * @param size 数据大小
     * @param method 传输方式
     * @return true 成功，false 失败
     */
    virtual bool Send(PlayerId targetPlayerId, const uint8_t *data, size_t size, DeliveryMethod method) = 0;

    /**
     * @brief 广播数据到所有已连接玩家
     * @param data 数据指针
     * @param size 数据大小
     * @param method 传输方式
     * @return true 成功，false 失败
     */
    virtual bool Broadcast(const uint8_t *data, size_t size, DeliveryMethod method) = 0;

    // ========== 回调注册 ==========

    using OnConnectedCallback = std::function<void(PlayerId playerId)>;
    using OnConnectFailedCallback =
        std::function<void(const std::string &address, uint16_t port, const std::string &reason)>;
    using OnDisconnectedCallback = std::function<void(PlayerId playerId, const std::string &reason)>;
    using OnDataReceivedCallback = std::function<void(PlayerId senderId, const uint8_t *data, size_t size)>;
    using OnConnectionRequestCallback =
        std::function<bool(PlayerId requestingPlayerId, const std::string &address, uint16_t port)>;
    using OnErrorCallback = std::function<void(uint32_t errorCode, const std::string &message)>;

    // 连接成功
    virtual void SetOnConnected(OnConnectedCallback callback) = 0;

    // 主动连接失败
    virtual void SetOnConnectFailed(OnConnectFailedCallback callback) = 0;

    // 连接断开
    virtual void SetOnDisconnected(OnDisconnectedCallback callback) = 0;

    // 收到数据
    virtual void SetOnDataReceived(OnDataReceivedCallback callback) = 0;

    // 收到连接请求（返回 true 接受，false 拒绝）
    virtual void SetOnConnectionRequest(OnConnectionRequestCallback callback) = 0;

    // 网络错误
    virtual void SetOnError(OnErrorCallback callback) = 0;

    // ========== 状态查询 ==========

    virtual PlayerId GetLocalPlayerId() const = 0;

    virtual ConnectionState GetConnectionState(PlayerId playerId) const = 0;

    virtual std::vector<PlayerId> GetConnectedPlayers() const = 0;

    virtual const NetworkConfig &GetConfig() const = 0;
};

} // namespace Network
} // namespace DX12Engine