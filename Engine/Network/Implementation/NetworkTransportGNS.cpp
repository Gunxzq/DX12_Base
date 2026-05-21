#include "NetworkTransportGNS.h"
#include <iostream>
#include <map>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

// 多实例支持
// 套接字-网络传输层实例映射
std::map<HSteamListenSocket, DX12Engine::Network::NetworkTransportGNS *>
    DX12Engine::Network::NetworkTransportGNS::s_listenSocketInstances;

// 实例互斥锁
std::mutex DX12Engine::Network::NetworkTransportGNS::s_instanceMutex;

namespace DX12Engine::Network {

NetworkTransportGNS::NetworkTransportGNS() {}

NetworkTransportGNS::~NetworkTransportGNS() { Stop(); }

/**
 * @brief 基于配置启动
 * @param config
 * @return bool
 * @date 2026-05-21
 */
bool NetworkTransportGNS::Start(const NetworkConfig &config) {

    if (m_config.enabled) {
        return false;
    }

    m_config = config;

    // 服务状态
    m_isServer = (config.mode == NetworkMode::Server);
    m_isP2P = (config.mode == NetworkMode::P2P);

    m_localPlayerId = config.localPlayerId;

    if (m_localPlayerId == 0 && (m_isServer || m_isP2P)) {
        m_localPlayerId = 1;
    }

    // 注意：GameNetworkingSockets_Init 应由 Bootstrap 统一调用

    if (!SteamNetworkingSockets()) {
        if (m_onError)
            m_onError(1, "GNS not initialized globally");
        return false;
    }

    // 创建 PollGroup
    // 消息池
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
        // 本地端口号
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
        SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
            &NetworkTransportGNS::OnSteamNetConnectionStatusChanged);
    }

    m_config.enabled = true;
    return true;
}

/**
 * @brief 停止服务，清理资源
 * @date 2026-05-21
 */
void NetworkTransportGNS::Stop() {

    if (!m_config.enabled)
        return;

    if (m_listenSocket != k_HSteamListenSocket_Invalid) {
        {
            std::lock_guard lock(s_instanceMutex);

            // 从全局映射中移除套接字实例
            s_listenSocketInstances.erase(m_listenSocket);
        }

        // 关闭监听套接字
        SteamNetworkingSockets()->CloseListenSocket(m_listenSocket);

        // 重置套接字状态
        m_listenSocket = k_HSteamListenSocket_Invalid;
    }

    for (auto &pair : m_playerToConnection) {
        // 关闭连接
        SteamNetworkingSockets()->CloseConnection(pair.second, 0, "Stopping", false);
    }

    // 清理状态
    m_playerToConnection.clear();
    m_connectionToPlayer.clear();
    m_pendingConnections.clear();
    m_pendingAuth.clear();

    m_serverPlayerId = 0;

    // 销毁消息池
    if (m_pollGroup != k_HSteamNetPollGroup_Invalid) {
        SteamNetworkingSockets()->DestroyPollGroup(m_pollGroup);
        m_pollGroup = k_HSteamNetPollGroup_Invalid;
    }

    m_config.enabled = false;
    m_state = ConnectionState::Disconnected;
}

/**
 * @brief 每帧调用，处理事件和消息
 * @date 2026-05-21
 */
void NetworkTransportGNS::Update() {

    if (!m_config.enabled)
        return;

    // 1. 先处理上一帧入队的待处理事件
    ProcessPendingEvents();

    // 2. 调用 RunCallbacks（会触发新事件入队到 m_pendingEvents）
    SteamNetworkingSockets()->RunCallbacks();

    // 3. 轮询消息
    PollIncomingMessages();
}

/**
 * @brief 连接到指定地址和端口
 * @param address IP 地址字符串
 * @param port 端口号
 * @return bool
 * @date 2026-05-21
 */
bool NetworkTransportGNS::Connect(const std::string &address, uint16_t port) {

    if (m_isServer && !m_isP2P) {
        return false;
    }

    SteamNetworkingIPAddr remoteAddr;
    remoteAddr.ParseString(address.c_str());
    remoteAddr.m_port = port;

    // 自动创建连接套接字
    HSteamNetConnection conn = SteamNetworkingSockets()->ConnectByIPAddress(remoteAddr, 0, nullptr);
    if (conn == k_HSteamNetConnection_Invalid) {
        if (m_onConnectFailed)
            m_onConnectFailed(address, port, "Invalid connection handle");
        return false;
    }

    // 记录待处理连接
    m_pendingConnections[conn] = {address, port};

    // 设置用户数据（用于回调时识别实例）
    SteamNetworkingSockets()->SetConnectionUserData(conn, (intptr_t)this);

    // 验证用户数据是否设置成功
    intptr_t verify = SteamNetworkingSockets()->GetConnectionUserData(conn);
    // std::cout << "[GNS] Set userData for conn " << conn << ": expected=" << (intptr_t)this << ", actual=" << verify
    //           << std::endl;
    if (verify != (intptr_t)this) {
        // std::cerr << "[GNS] WARNING: SetConnectionUserData failed!" << std::endl;
    }

    SteamNetworkingSockets()->SetConnectionPollGroup(conn, m_pollGroup);

    m_state = ConnectionState::Connecting;
    return true;
}

/**
 * @brief 发送数据到指定玩家
 * @param targetPlayerId  目标玩家 ID
 * @param data 数据指针
 * @param size 数据大小
 * @param method 传输策略
 * @return bool
 * @date 2026-05-21
 */
bool NetworkTransportGNS::Send(PlayerId targetPlayerId, const uint8_t *data, size_t size, DeliveryMethod method) {

    HSteamNetConnection conn = GetConnection(targetPlayerId);
    if (conn == k_HSteamNetConnection_Invalid)
        return false;

    return SendToConnection(conn, data, size, method);
}

/**
 * @brief 广播数据到所有连接（服务端和 P2P 模式），或发送给服务器（客户端模式）
 * @param data 数据指针
 * @param size 数据大小
 * @param method 传输策略
 * @return bool
 * @date 2026-05-21
 */
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

/**
 * @brief 获取指定玩家的连接状态
 * @param playerId
 * @return ConnectionState
 * @date 2026-05-21
 */
ConnectionState NetworkTransportGNS::GetConnectionState(PlayerId playerId) const {
    if (playerId == m_localPlayerId)
        return ConnectionState::Connected;

    if (m_playerToConnection.find(playerId) != m_playerToConnection.end())
        return ConnectionState::Connected;

    return ConnectionState::Disconnected;
}

/**
 * @brief 获取所有已连接玩家的 ID
 * @return std::vector<PlayerId>
 * @date 2026-05-21
 */
std::vector<PlayerId> NetworkTransportGNS::GetConnectedPlayers() const {
    std::vector<PlayerId> players;
    for (const auto &pair : m_playerToConnection) {
        players.push_back(pair.first);
    }
    return players;
}

// ========== 内部方法 ==========

/**
 * @brief GNS 连接状态变化的全局回调函数。根据回调信息找到对应的实例并调用实例方法处理事件。
 * @param info
 * @date 2026-05-21
 */
void NetworkTransportGNS::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *info) {

    NetworkTransportGNS *instance = nullptr;

    // 1. 优先从连接的用户数据中获取（适用于已建立的连接或主动发起的连接）
    intptr_t userData = info->m_info.m_nUserData;
    if (userData != 0 && userData != (intptr_t)-1) {
        instance = reinterpret_cast<NetworkTransportGNS *>(userData);
    }

    // 2. 尝试从监听套接字映射表获取
    if (!instance && info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid) {
        std::lock_guard lock(s_instanceMutex);
        auto it = s_listenSocketInstances.find(info->m_info.m_hListenSocket);
        if (it != s_listenSocketInstances.end()) {
            instance = it->second;
        }
    }

    // 3. 尝试从 pendingConnections 反向查找（新增）
    if (!instance) {
        std::lock_guard lock(s_instanceMutex);
        for (auto &pair : s_listenSocketInstances) {
            auto &pending = pair.second->m_pendingConnections;
            if (pending.find(info->m_hConn) != pending.end()) {
                instance = pair.second;

                break;
            }
        }
    }

    if (instance) {
        instance->HandleConnectionStatusChanged(*info);
    } else {
        // std::cout << "[GNS]   ERROR: no instance found!" << std::endl;
    }
}
void NetworkTransportGNS::HandleConnectionStatusChanged(const SteamNetConnectionStatusChangedCallback_t &info) {
    HSteamNetConnection conn = info.m_hConn;
    ESteamNetworkingConnectionState state = info.m_info.m_eState;

    // ========== 防御性检查：根据网络模式分离逻辑 ==========
    if (m_isServer || m_isP2P) {
        // --- 服务端 / P2P 节点逻辑 ---
        // 这类节点有 listenSocket，需要根据它来过滤新连接
        if (info.m_info.m_hListenSocket != k_HSteamListenSocket_Invalid) {
            if (info.m_info.m_hListenSocket != m_listenSocket)
                return; // 这个连接不属于我的监听 socket，忽略
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
        event.hListenSocket = info.m_info.m_hListenSocket;
        event.state = state;
        m_pendingEvents.push_back(event);
        break;
    }

    case k_ESteamNetworkingConnectionState_Connected: {
        PendingEvent event;
        event.type = PendingEventType::Connected;
        event.conn = conn;
        event.hListenSocket = info.m_info.m_hListenSocket;
        event.state = state;
        m_pendingEvents.push_back(event);
        break;
    }

    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
        PendingEvent event;
        event.type = PendingEventType::Disconnected;
        event.conn = conn;
        event.hListenSocket = info.m_info.m_hListenSocket;
        event.state = state;
        m_pendingEvents.push_back(event);
        break;
    }

    default:
        break;
    }
}

/**
 * @brief 处理待处理的连接状态变化事件。
 * @date 2026-05-21
 */
void NetworkTransportGNS::ProcessPendingEvents() {
    // 使用 swap 取出队列，避免在处理过程中被修改
    std::vector<PendingEvent> events;
    std::swap(events, m_pendingEvents);

    for (const auto &event : events) {
        switch (event.type) {
        case PendingEventType::Connecting: {
            HSteamNetConnection conn = event.conn;
            SteamNetworkingSockets()->AcceptConnection(conn);
            SteamNetworkingSockets()->SetConnectionPollGroup(conn, m_pollGroup);
            SteamNetworkingSockets()->SetConnectionUserData(conn, (intptr_t)this);
            break;
        }

        case PendingEventType::Connected: {
            // 连接成功建立
            HSteamNetConnection conn = event.conn;

            if (m_isServer) {
                // 服务器模式：等待客户端发送身份声明
                m_pendingAuth[conn] = true;
            } else if (m_isP2P) {
                // P2P 模式：不立即分配 ID，而是等待对方发送 ID
                // 使用 m_pendingAuth 等待 ID 交换
                m_pendingAuth[conn] = true;

                // 主动发送自己的 ID
                if (m_localPlayerId != 0) {
                    uint8_t buffer[8];
                    memcpy(buffer, &m_localPlayerId, 8);
                    SendToConnection(conn, buffer, 8, DeliveryMethod::Reliable);
                }
            } else {
                // 客户端模式：直接使用连接句柄作为服务器的ID
                PlayerId serverId = static_cast<PlayerId>(conn);
                m_playerToConnection[serverId] = conn;
                m_connectionToPlayer[conn] = serverId;
                m_serverPlayerId = serverId;

                // 连接成功后立即发送自己的ID给服务器
                if (m_localPlayerId != 0) {
                    uint8_t buffer[8];
                    memcpy(buffer, &m_localPlayerId, 8);
                    SendToConnection(conn, buffer, 8, DeliveryMethod::Reliable);
                }

                if (m_onConnected)
                    m_onConnected(serverId);
            }

            m_state = ConnectionState::Connected;
            break;
        }

        case PendingEventType::Disconnected: {
            // 连接断开
            HSteamNetConnection conn = event.conn;
            PlayerId id = GetPlayerId(conn);
            if (id != 0 && m_onDisconnected)
                m_onDisconnected(id, "Disconnected");

            m_playerToConnection.erase(id);
            m_connectionToPlayer.erase(conn);
            m_pendingAuth.erase(conn); // 清理待验证连接
            if (id == m_serverPlayerId)
                m_serverPlayerId = 0;

            SteamNetworkingSockets()->CloseConnection(conn, 0, nullptr, false);

            if (m_playerToConnection.empty() && m_serverPlayerId == 0 && m_pendingConnections.empty())
                m_state = ConnectionState::Disconnected;

            break;
        }

        default:
            break;
        }
    }
}

/**
 * @brief 轮询消息池，处理所有待处理的消息。
 * 优先处理身份验证消息，然后处理普通消息。
 * @date 2026-05-21
 */
void NetworkTransportGNS::PollIncomingMessages() {
    constexpr int MAX_MESSAGES = 64;
    ISteamNetworkingMessage *incomingMessages[MAX_MESSAGES];

    // 从消息池中取出所有消息数据包
    int numMessages = SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(m_pollGroup, incomingMessages, MAX_MESSAGES);

    for (int i = 0; i < numMessages; ++i) {
        ISteamNetworkingMessage *msg = incomingMessages[i];

        // 检查该连接是否还未验证身份（服务器/P2P模式）
        auto authIt = m_pendingAuth.find(msg->m_conn);
        if (authIt != m_pendingAuth.end()) {
            // 第一条消息必须是身份声明（8字节的PlayerId）
            if (msg->m_cbSize == sizeof(PlayerId)) {
                PlayerId claimedId = *reinterpret_cast<const PlayerId *>(msg->m_pData);

                // 验证ID是否可用（未重复且不为0）
                if (claimedId != 0 && m_playerToConnection.find(claimedId) == m_playerToConnection.end()) {
                    // 接受身份声明
                    m_playerToConnection[claimedId] = msg->m_conn;
                    m_connectionToPlayer[msg->m_conn] = claimedId;
                    m_pendingAuth.erase(authIt);

                    // 触发连接回调
                    if (m_onConnected)
                        m_onConnected(claimedId);
                } else {
                    // ID已被使用或无效，拒绝连接
                    SteamNetworkingSockets()->CloseConnection(msg->m_conn, 0, "ID in use or invalid", false);
                    m_pendingAuth.erase(authIt);
                }
            } else {
                // 消息格式不正确，拒绝连接
                SteamNetworkingSockets()->CloseConnection(msg->m_conn, 0, "Invalid auth message", false);
                m_pendingAuth.erase(authIt);
            }
            msg->Release();
            continue;
        }

        // 正常消息处理
        PlayerId senderId = GetPlayerId(msg->m_conn);
        if (senderId != 0 && m_onDataReceived) {
            m_onDataReceived(senderId, (const uint8_t *)msg->m_pData, msg->m_cbSize);
        }

        msg->Release();
    }
}

/**
 * @brief 发送数据到指定连接。根据传输策略设置 GNS 发送标志，并调用 SendMessageToConnection 发送数据。
 * @param conn 连接句柄
 * @param data 数据指针
 * @param size 数据大小
 * @param method 传输策略
 * @return bool
 * @date 2026-05-21
 */
bool NetworkTransportGNS::SendToConnection(HSteamNetConnection conn, const uint8_t *data, size_t size,
                                           DeliveryMethod method) {
    if (conn == k_HSteamNetConnection_Invalid)
        return false;

    int sendFlags = 0;
    switch (method) {
        // 不可靠发送，允许丢包，适用于频繁更新的状态数据（如位置、动作等），以减少延迟和带宽占用
    case DeliveryMethod::Unreliable:
        sendFlags = k_nSteamNetworkingSend_Unreliable;
        break;
        // 可靠发送，确保数据按顺序到达，适用于需要高可靠性的场景（如游戏事件、玩家操作等）
    case DeliveryMethod::Reliable:
        sendFlags = k_nSteamNetworkingSend_Reliable;
        break;
        // 关键可靠发送，禁用 Nagle 算法，适用于需要最低延迟的关键数据（如输入、射击等），但可能增加带宽占用
        // 小包立即发送，延迟最低但带宽效率低
    case DeliveryMethod::Critical:
        sendFlags = k_nSteamNetworkingSend_ReliableNoNagle;
        break;
    }

    // 发送消息（GNS 会根据连接状态自动处理分包、重传等机制）
    EResult result = SteamNetworkingSockets()->SendMessageToConnection(conn, data, (uint32)size, sendFlags, nullptr);
    return result == k_EResultOK;
}

/**
 * @brief 根据玩家 ID 获取对应的连接句柄。
 * @param playerId 玩家 ID
 * @return HSteamNetConnection
 * @date 2026-05-21
 */
HSteamNetConnection NetworkTransportGNS::GetConnection(PlayerId playerId) const {
    auto it = m_playerToConnection.find(playerId);
    if (it != m_playerToConnection.end())
        return it->second;
    return k_HSteamNetConnection_Invalid;
}

/**
 * @brief 根据连接句柄获取对应的玩家 ID。
 * @param conn 连接句柄
 * @return PlayerId
 * @date 2026-05-21
 */
PlayerId NetworkTransportGNS::GetPlayerId(HSteamNetConnection conn) const {
    auto it = m_connectionToPlayer.find(conn);
    if (it != m_connectionToPlayer.end()) {
        return it->second;
    }
    return 0;
}

} // namespace DX12Engine