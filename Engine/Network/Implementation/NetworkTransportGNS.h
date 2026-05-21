#pragma once
#include "Network/INetworkTransport.h"
#include <map>
#include <memory>
#include <mutex>
#include <steam/steamnetworkingtypes.h>
#include <unordered_map>

namespace DX12Engine {
namespace Network {

/**
 * @brief GameNetworkingSockets 传输层实现
 */
class NetworkTransportGNS : public INetworkTransport {
public:
    NetworkTransportGNS();
    ~NetworkTransportGNS() override;

    // 禁止拷贝
    NetworkTransportGNS(const NetworkTransportGNS &) = delete;
    NetworkTransportGNS &operator=(const NetworkTransportGNS &) = delete;

    // ========== INetworkTransport 接口实现 ==========
    bool Start(const NetworkConfig &config) override;
    void Stop() override;
    void Update(float deltaTime) override;

    bool Connect(const std::string &address, uint16_t port) override;
    bool Send(PlayerId targetPlayerId, const uint8_t *data, size_t size, DeliveryMethod method) override;
    bool Broadcast(const uint8_t *data, size_t size, DeliveryMethod method) override;

    void SetOnConnected(OnConnectedCallback callback) override;
    void SetOnConnectFailed(OnConnectFailedCallback callback) override;
    void SetOnDisconnected(OnDisconnectedCallback callback) override;
    void SetOnDataReceived(OnDataReceivedCallback callback) override;
    void SetOnConnectionRequest(OnConnectionRequestCallback callback) override;
    void SetOnError(OnErrorCallback callback) override;

    PlayerId GetLocalPlayerId() const override;
    ConnectionState GetConnectionState(PlayerId playerId) const override;
    std::vector<PlayerId> GetConnectedPlayers() const override;
    const NetworkConfig &GetConfig() const override;

    static std::map<HSteamListenSocket, NetworkTransportGNS *> s_listenSocketInstances;
    static std::mutex s_instanceMutex;

private:
    // 事件类型
    enum class PendingEventType {
        Connecting,  // 收到连接请求
        Connected,   // 连接建立
        Disconnected // 连接断开
    };

    struct PendingEvent {
        PendingEventType type;
        HSteamNetConnection conn;
        SteamNetConnectionInfo_t info;
    };

    // ========== GNS 回调处理 ==========
    static void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *info);
    void HandleConnectionStatusChanged(const SteamNetConnectionStatusChangedCallback_t &info);
    void ProcessPendingEvents();

    // ========== 内部方法 ==========
    void PollIncomingMessages();
    void PollConnectionStateChanges();
    bool SendToConnection(HSteamNetConnection conn, const uint8_t *data, size_t size, DeliveryMethod method);

    PlayerId AssignNewPlayerId(HSteamNetConnection conn);
    HSteamNetConnection GetConnection(PlayerId playerId) const;
    PlayerId GetPlayerId(HSteamNetConnection conn) const;

    // ========== 状态 ==========
    NetworkConfig m_config;
    ConnectionState m_state = ConnectionState::Disconnected;
    bool m_isServer = false;
    bool m_isP2P = false;

    // ========== GNS 句柄 ==========
    HSteamListenSocket m_listenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup m_pollGroup = k_HSteamNetPollGroup_Invalid;

    // ========== 连接映射 ==========
    // 统一使用映射表管理所有远程连接 (P2P Peers, Server for Client)
    std::unordered_map<PlayerId, HSteamNetConnection> m_playerToConnection;
    std::unordered_map<HSteamNetConnection, PlayerId> m_connectionToPlayer;

    // 用于跟踪正在尝试建立连接的句柄，以区分连接失败和断开连接
    std::unordered_map<HSteamNetConnection, std::pair<std::string, uint16_t>> m_pendingConnections;

    // ========== 玩家 ID 管理 ==========
    PlayerId m_localPlayerId = 0;
    PlayerId m_nextPlayerId = 1;

    // 用于 Client 模式：记录服务器对应的 PlayerId (通常固定为 1 或由回调分配)
    PlayerId m_serverPlayerId = 0;

    // ========== 回调 ==========
    OnConnectedCallback m_onConnected;
    OnConnectFailedCallback m_onConnectFailed;
    OnDisconnectedCallback m_onDisconnected;
    OnDataReceivedCallback m_onDataReceived;
    OnConnectionRequestCallback m_onConnectionRequest;
    OnErrorCallback m_onError;

    // ========== 统计 ==========
    uint64_t m_lastPingSendTime = 0;

    // 事件队列（工作线程入队，主线程处理）
    std::vector<PendingEvent> m_pendingEvents;
};

} // namespace Network
} // namespace DX12Engine