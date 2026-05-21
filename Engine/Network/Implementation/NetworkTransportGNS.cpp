#include "NetworkTransportGNS.h"
#include <iostream>
#include <map>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

std::map<HSteamListenSocket, DX12Engine::Network::NetworkTransportGNS *>
    DX12Engine::Network::NetworkTransportGNS::s_listenSocketInstances;

std::mutex DX12Engine::Network::NetworkTransportGNS::s_instanceMutex;

namespace DX12Engine {
namespace Network {

NetworkTransportGNS::NetworkTransportGNS() {}

NetworkTransportGNS::~NetworkTransportGNS() { Stop(); }

bool NetworkTransportGNS::Start(const NetworkConfig &config) {

    if (m_config.enabled) {
        return false;
    }

    m_config = config;
    m_isServer = (config.mode == NetworkMode::Server);
    m_isP2P = (config.mode == NetworkMode::P2P);
    m_localPlayerId = config.localPlayerId;

    if (m_localPlayerId == 0 && (m_isServer || m_isP2P)) {
        m_localPlayerId = 1;
    }

    // 注意：GameNetworkingSockets_Init 应由 Bootstrap 统一调用
    // 这里仅检查是否已初始化，或者由上层保证
    if (!SteamNetworkingSockets()) {
        if (m_onError)
            m_onError(1, "GNS not initialized globally");
        return false;
    }

    // 创建 PollGroup
    m_pollGroup = SteamNetworkingSockets()->CreatePollGroup();
    if (m_pollGroup == k_HSteamNetPollGroup_Invalid) {
        if (m_onError)
            m_onError(2, "Failed to create poll group");
        return false;
    }

    // 创建监听套接字 (Server 和 P2P)
    if (m_isServer || m_isP2P) {
        SteamNetworkingIPAddr localAddr;
        localAddr.Clear();
        localAddr.m_port = m_config.port;

        m_listenSocket = SteamNetworkingSockets()->CreateListenSocketIP(localAddr, 0, nullptr);
        if (m_listenSocket == k_HSteamListenSocket_Invalid) {
            if (m_onError)
                m_onError(3, "Failed to create listen socket");
            return false;
        }

        {
            std::lock_guard lock(s_instanceMutex);
            s_listenSocketInstances[m_listenSocket] = this;
        }

        // 设置全局回调 (同样，全局回调通常只设置一次，如果多实例需注意)
        // 如果 Bootstrap 已经设置了全局回调并路由到某个管理器，这里可能需要调整
        // 假设当前是单实例或全局回调直接转发
        SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
            &NetworkTransportGNS::OnSteamNetConnectionStatusChanged);
    }

    m_config.enabled = true;
    return true;
}

void NetworkTransportGNS::Stop() {

    if (!m_config.enabled)
        return;

    if (m_listenSocket != k_HSteamListenSocket_Invalid) {
        {
            std::lock_guard lock(s_instanceMutex);
            s_listenSocketInstances.erase(m_listenSocket);
        }
        SteamNetworkingSockets()->CloseListenSocket(m_listenSocket);
        m_listenSocket = k_HSteamListenSocket_Invalid;
    }

    for (auto &pair : m_playerToConnection) {
        SteamNetworkingSockets()->CloseConnection(pair.second, 0, "Stopping", false);
    }
    m_playerToConnection.clear();
    m_connectionToPlayer.clear();

    m_pendingConnections.clear();

    m_serverPlayerId = 0;

    if (m_pollGroup != k_HSteamNetPollGroup_Invalid) {
        SteamNetworkingSockets()->DestroyPollGroup(m_pollGroup);
        m_pollGroup = k_HSteamNetPollGroup_Invalid;
    }

    m_config.enabled = false;
    m_state = ConnectionState::Disconnected;
}

void NetworkTransportGNS::Update(float deltaTime) {

    if (!m_config.enabled)
        return;

    // 1. 先处理上一帧入队的待处理事件（调用 GNS API，可能触发新回调入队）
    ProcessPendingEvents();

    // 2. 调用 RunCallbacks（会触发新事件入队到 m_pendingEvents）
    SteamNetworkingSockets()->RunCallbacks();

    // 3. 轮询消息
    PollIncomingMessages();
}

bool NetworkTransportGNS::Connect(const std::string &address, uint16_t port) {

    if (m_isServer && !m_isP2P) {
        return false;
    }

    SteamNetworkingIPAddr remoteAddr;
    remoteAddr.ParseString(address.c_str());
    remoteAddr.m_port = port;

    HSteamNetConnection conn = SteamNetworkingSockets()->ConnectByIPAddress(remoteAddr, 0, nullptr);
    if (conn == k_HSteamNetConnection_Invalid) {
        if (m_onConnectFailed)
            m_onConnectFailed(address, port, "Invalid connection handle");
        return false;
    }

    m_pendingConnections[conn] = {address, port};

    // 设置用户数据（用于回调时识别实例）
    SteamNetworkingSockets()->SetConnectionUserData(conn, (intptr_t)this);

    // 验证用户数据是否设置成功
    intptr_t verify = SteamNetworkingSockets()->GetConnectionUserData(conn);
    std::cout << "[GNS] Set userData for conn " << conn << ": expected=" << (intptr_t)this << ", actual=" << verify
              << std::endl;
    if (verify != (intptr_t)this) {
        std::cerr << "[GNS] WARNING: SetConnectionUserData failed!" << std::endl;
    }

    SteamNetworkingSockets()->SetConnectionPollGroup(conn, m_pollGroup);

    m_state = ConnectionState::Connecting;
    return true;
}

bool NetworkTransportGNS::Send(PlayerId targetPlayerId, const uint8_t *data, size_t size, DeliveryMethod method) {

    HSteamNetConnection conn = GetConnection(targetPlayerId);
    if (conn == k_HSteamNetConnection_Invalid) {
        return false;
    }

    return SendToConnection(conn, data, size, method);
}

bool NetworkTransportGNS::Broadcast(const uint8_t *data, size_t size, DeliveryMethod method) {

    if (m_isServer || m_isP2P) {
        bool success = true;
        for (auto &pair : m_playerToConnection) {
            if (!SendToConnection(pair.second, data, size, method)) {
                success = false;
            }
        }
        return success;
    } else {
        // Client 模式：广播即发送给服务器
        if (m_serverPlayerId != 0) {
            auto it = m_playerToConnection.find(m_serverPlayerId);
            if (it != m_playerToConnection.end()) {
                return SendToConnection(it->second, data, size, method);
            }
        }
        return false;
    }
}

// ========== 回调注册 ==========
void NetworkTransportGNS::SetOnConnected(OnConnectedCallback callback) { m_onConnected = std::move(callback); }
void NetworkTransportGNS::SetOnConnectFailed(OnConnectFailedCallback callback) {
    m_onConnectFailed = std::move(callback);
}
void NetworkTransportGNS::SetOnDisconnected(OnDisconnectedCallback callback) { m_onDisconnected = std::move(callback); }
void NetworkTransportGNS::SetOnDataReceived(OnDataReceivedCallback callback) { m_onDataReceived = std::move(callback); }
void NetworkTransportGNS::SetOnConnectionRequest(OnConnectionRequestCallback callback) {
    m_onConnectionRequest = std::move(callback);
}
void NetworkTransportGNS::SetOnError(OnErrorCallback callback) { m_onError = std::move(callback); }

// ========== 状态查询 ==========
PlayerId NetworkTransportGNS::GetLocalPlayerId() const { return m_localPlayerId; }

ConnectionState NetworkTransportGNS::GetConnectionState(PlayerId playerId) const {
    if (playerId == m_localPlayerId)
        return ConnectionState::Connected;
    if (m_playerToConnection.find(playerId) != m_playerToConnection.end())
        return ConnectionState::Connected;
    return ConnectionState::Disconnected;
}

std::vector<PlayerId> NetworkTransportGNS::GetConnectedPlayers() const {
    std::vector<PlayerId> players;
    for (const auto &pair : m_playerToConnection) {
        players.push_back(pair.first);
    }
    return players;
}

const NetworkConfig &NetworkTransportGNS::GetConfig() const { return m_config; }

// ========== 内部方法 ==========

void NetworkTransportGNS::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *info) {

    std::cout << "[GNS] Global callback triggered" << std::endl;
    NetworkTransportGNS *instance = nullptr;

    // 1. 优先从连接的用户数据中获取（适用于已建立的连接或主动发起的连接）
    intptr_t userData = info->m_info.m_nUserData;
    std::cout << "[GNS] userData from callback: " << userData << std::endl;

    // 检查 userData 是否有效（不是 -1 或 0）
    if (userData != 0 && userData != (intptr_t)-1) {
        instance = reinterpret_cast<NetworkTransportGNS *>(userData);
        std::cout << "[GNS] Found instance from user data: " << instance << std::endl;
    }
    // 2. 如果是监听套接字上的事件（如新连接请求），从静态映射表中查找
    else if (info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid) {
        {
            std::lock_guard lock(s_instanceMutex);
            auto it = s_listenSocketInstances.find(info->m_info.m_hListenSocket);
            if (it != s_listenSocketInstances.end()) {
                instance = it->second;
                std::cout << "[GNS] Found instance from listen socket map: " << instance
                          << " (listenSocket: " << info->m_info.m_hListenSocket << ")" << std::endl;
            }
        }
    }

    // 额外检查：如果 userData 是 -1，尝试从监听套接字查找
    if (!instance && userData == (intptr_t)-1 && info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid) {
        std::cout << "[GNS] userData is -1, trying listen socket map..." << std::endl;
        std::lock_guard lock(s_instanceMutex);
        auto it = s_listenSocketInstances.find(info->m_info.m_hListenSocket);
        if (it != s_listenSocketInstances.end()) {
            instance = it->second;
            std::cout << "[GNS] Found instance from listen socket map (fallback): " << instance << std::endl;
        }
    }

    if (instance) {
        std::cout << "[GNS] Calling HandleConnectionStatusChanged on instance: " << instance
                  << ", localPlayerId=" << instance->GetLocalPlayerId() << std::endl;
        instance->HandleConnectionStatusChanged(*info);
    } else {
        std::cout << "[GNS] WARNING: No instance found for callback!" << std::endl;
    }
}
void NetworkTransportGNS::HandleConnectionStatusChanged(const SteamNetConnectionStatusChangedCallback_t &info) {
    HSteamNetConnection conn = info.m_hConn;
    ESteamNetworkingConnectionState state = info.m_info.m_eState;

    std::cout << "[GNS] State: " << state << " for conn " << conn << ", hListenSocket=" << info.m_info.m_hListenSocket
              << ", mode=" << (m_isServer ? "Server" : (m_isP2P ? "P2P" : "Client")) << std::endl;

    // ========== 防御性检查：根据网络模式分离逻辑 ==========
    if (m_isServer || m_isP2P) {
        // --- 服务端 / P2P 节点逻辑 ---
        // 这类节点有 listenSocket，需要根据它来过滤新连接
        if (info.m_info.m_hListenSocket != k_HSteamListenSocket_Invalid) {
            if (info.m_info.m_hListenSocket != m_listenSocket) {
                std::cout << "[GNS] WARNING: Skipping connection not from my listen socket! "
                          << "Expected: " << m_listenSocket << ", Got: " << info.m_info.m_hListenSocket << std::endl;
                return; // 这个连接不属于我的监听 socket，忽略
            }
        }
        // info.m_info.m_hListenSocket == 0 表示主动发起的连接，直接通过
    }
    // --- 客户端逻辑 ---
    // 客户端没有 listenSocket，不进行监听套接字检查
    // 客户端只关心自己主动发起的连接，直接处理

    // 将事件入队，不在回调中直接调用 GNS API（防止递归回调导致状态破坏）
    switch (state) {
    case k_ESteamNetworkingConnectionState_Connecting: {
        PendingEvent event;
        event.type = PendingEventType::Connecting;
        event.conn = conn;
        event.info = info.m_info;
        m_pendingEvents.push_back(event);
        break;
    }

    case k_ESteamNetworkingConnectionState_Connected: {
        PendingEvent event;
        event.type = PendingEventType::Connected;
        event.conn = conn;
        event.info = info.m_info;
        m_pendingEvents.push_back(event);
        break;
    }

    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
        PendingEvent event;
        event.type = PendingEventType::Disconnected;
        event.conn = conn;
        event.info = info.m_info;
        m_pendingEvents.push_back(event);
        break;
    }

    default:
        break;
    }
}

void NetworkTransportGNS::ProcessPendingEvents() {
    // 使用 swap 取出队列，避免在处理过程中被修改
    std::vector<PendingEvent> events;
    std::swap(events, m_pendingEvents);

    for (const auto &event : events) {
        switch (event.type) {
        case PendingEventType::Connecting: {
            std::cout << "[GNS] Processing Connecting event, this=" << this << std::endl;
            HSteamNetConnection conn = event.conn;
            // 在主线程中调用 GNS API
            SteamNetworkingSockets()->AcceptConnection(conn);
            SteamNetworkingSockets()->SetConnectionPollGroup(conn, m_pollGroup);
            SteamNetworkingSockets()->SetConnectionUserData(conn, (intptr_t)this);
            break;
        }

        case PendingEventType::Connected: {
            // 连接成功建立
            HSteamNetConnection conn = event.conn;
            PlayerId newId = AssignNewPlayerId(conn);
            if (!m_isP2P && !m_isServer && m_serverPlayerId == 0) {
                m_serverPlayerId = newId;
            }
            if (m_onConnected) {
                m_onConnected(newId);
            }
            m_state = ConnectionState::Connected;
            break;
        }

        case PendingEventType::Disconnected: {
            // 连接断开
            HSteamNetConnection conn = event.conn;
            PlayerId id = GetPlayerId(conn);
            if (id != 0 && m_onDisconnected) {
                m_onDisconnected(id, "Disconnected");
            }
            m_playerToConnection.erase(id);
            m_connectionToPlayer.erase(conn);
            if (id == m_serverPlayerId) {
                m_serverPlayerId = 0;
            }
            SteamNetworkingSockets()->CloseConnection(conn, 0, nullptr, false);

            if (m_playerToConnection.empty() && m_serverPlayerId == 0 && m_pendingConnections.empty()) {
                m_state = ConnectionState::Disconnected;
            }
            break;
        }

        default:
            break;
        }
    }
}
void NetworkTransportGNS::PollIncomingMessages() {
    constexpr int MAX_MESSAGES = 64;
    ISteamNetworkingMessage *incomingMessages[MAX_MESSAGES];

    int numMessages = SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(m_pollGroup, incomingMessages, MAX_MESSAGES);

    for (int i = 0; i < numMessages; ++i) {
        ISteamNetworkingMessage *msg = incomingMessages[i];

        PlayerId senderId = GetPlayerId(msg->m_conn);
        if (senderId != 0 && m_onDataReceived) {
            m_onDataReceived(senderId, (const uint8_t *)msg->m_pData, msg->m_cbSize);
        }

        msg->Release();
    }
}

void NetworkTransportGNS::PollConnectionStateChanges() {
    // No-op if using global callbacks
}

bool NetworkTransportGNS::SendToConnection(HSteamNetConnection conn, const uint8_t *data, size_t size,
                                           DeliveryMethod method) {
    if (conn == k_HSteamNetConnection_Invalid)
        return false;

    int sendFlags = 0;
    switch (method) {
    case DeliveryMethod::Unreliable:
        sendFlags = k_nSteamNetworkingSend_Unreliable;
        break;
    case DeliveryMethod::Reliable:
        sendFlags = k_nSteamNetworkingSend_Reliable;
        break;
    case DeliveryMethod::Critical:
        sendFlags = k_nSteamNetworkingSend_ReliableNoNagle;
        break;
    }

    EResult result = SteamNetworkingSockets()->SendMessageToConnection(conn, data, (uint32)size, sendFlags, nullptr);
    return result == k_EResultOK;
}

PlayerId NetworkTransportGNS::AssignNewPlayerId(HSteamNetConnection conn) {
    PlayerId newId = m_nextPlayerId++;
    m_playerToConnection[newId] = conn;
    m_connectionToPlayer[conn] = newId;
    return newId;
}

HSteamNetConnection NetworkTransportGNS::GetConnection(PlayerId playerId) const {
    auto it = m_playerToConnection.find(playerId);
    if (it != m_playerToConnection.end()) {
        return it->second;
    }
    return k_HSteamNetConnection_Invalid;
}

PlayerId NetworkTransportGNS::GetPlayerId(HSteamNetConnection conn) const {
    auto it = m_connectionToPlayer.find(conn);
    if (it != m_connectionToPlayer.end()) {
        return it->second;
    }
    return 0;
}

} // namespace Network
} // namespace DX12Engine