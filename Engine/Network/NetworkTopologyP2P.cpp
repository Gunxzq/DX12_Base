#include "NetworkTopologyP2P.h"
#include "INetworkTransport.h"
#include "Struct/NetworkTypes.h"
#include <cstring>
#include <iostream>

namespace DX12Engine {
namespace Network {

P2PTopology::P2PTopology() = default;

P2PTopology::~P2PTopology() { Shutdown(); }

/**
 * @brief 初始化 P2P 拓扑
 * @param config 网络配置
 * @param transport 网络传输层接口
 * @return bool
 * @date 2026-05-21
 */
bool P2PTopology::Initialize(const NetworkConfig &config, INetworkTransport *transport) {
    if (!config.enabled || config.mode != NetworkMode::P2P || !transport) {
        return false;
    }

    m_config = config;
    m_transport = transport; // 网络传输层
    m_localPlayerId = transport->GetLocalPlayerId();
    m_isRunning = true;
    m_currentFrame = 0;
    m_nextLocalFrame = 0;
    m_localInput = 0;

    // 注册传输层回调
    m_transport->SetOnConnected([this](PlayerId playerId) { AddPlayer(playerId); });
    m_transport->SetOnDisconnected([this](PlayerId playerId, const std::string &) { RemovePlayer(playerId); });
    m_transport->SetOnDataReceived(
        [this](PlayerId senderId, const uint8_t *data, size_t size) { OnTransportData(senderId, data, size); });

    // 如果已存在连接的玩家，先添加
    auto connectedPlayers = transport->GetConnectedPlayers();
    for (auto playerId : connectedPlayers) {
        AddPlayer(playerId);
    }

    return true;
}

/**
 * @brief 关闭 P2P 拓扑
 * @date 2026-05-21
 */
void P2PTopology::Shutdown() {
    if (!m_isRunning)
        return;

    m_isRunning = false;
    m_playerInputQueues.clear();
    m_playerList.clear();
    m_transport = nullptr;
}

/**
 * @brief 更新 P2P 拓扑状态，处理网络事件和帧同步
 * @param deltaTime
 * @date 2026-05-21
 */
void P2PTopology::Update(float deltaTime) {
    if (!m_isRunning || !m_transport)
        return;

    // 更新传输层，接收网络数据
    m_transport->Update();
}

/**
 * @brief 帧同步逻辑，每帧广播本地输入并尝试推进帧
 * @date 2026-05-21
 */
void P2PTopology::OnFrameSync() {

    if (!m_isRunning)
        return;

    // 只在有玩家连接时才广播
    if (!m_playerList.empty()) {
        BroadcastLocalInput();
        m_nextLocalFrame++;
        TryAdvanceFrame();
    }
}

/**
 * @brief 获取指定玩家在指定帧的输入
 * @param playerId 玩家 ID
 * @param frameNum 帧号
 * @return uint32_t
 * @date 2026-05-21
 */
uint32_t P2PTopology::GetPlayerInput(PlayerId playerId, uint32_t frameNum) const {
    // 如果请求的是当前帧，返回本地输入（零延迟）
    if (playerId == m_localPlayerId && frameNum == m_currentFrame) {
        return m_localInput;
    }

    // 查找玩家输入队列
    auto it = m_playerInputQueues.find(playerId);
    if (it == m_playerInputQueues.end())
        return 0;

    // 使用 unordered_map 直接索引查找，O(1) 复杂度
    const auto &inputs = it->second.inputs;
    auto inputIt = inputs.find(frameNum);
    return (inputIt != inputs.end()) ? inputIt->second : 0;
}

/**
 * @brief 发送游戏层消息给指定玩家
 * @param targetPlayerId 目标玩家 ID
 * @param data 消息数据
 * @param size 消息数据大小
 * @param reliable 传输策略
 * @date 2026-05-21
 */
void P2PTopology::SendToPlayer(PlayerId targetPlayerId, const uint8_t *data, size_t size, bool reliable) {
    if (!m_isRunning || !m_transport)
        return;

    DeliveryMethod method = reliable ? DeliveryMethod::Reliable : DeliveryMethod::Unreliable;
    m_transport->Send(targetPlayerId, data, size, method);
}

/**
 * @brief 广播游戏层消息给所有玩家
 * @param data 消息数据
 * @param size 消息数据大小
 * @param reliable 传输策略
 * @date 2026-05-21
 */
void P2PTopology::BroadcastToAll(const uint8_t *data, size_t size, bool reliable) {
    if (!m_isRunning || !m_transport)
        return;

    DeliveryMethod method = reliable ? DeliveryMethod::Reliable : DeliveryMethod::Unreliable;
    m_transport->Broadcast(data, size, method);
}

/**
 * @brief 检查是否有玩家已连接
 * @return bool
 * @date 2026-05-21
 */
bool P2PTopology::IsConnected() const {
    if (!m_transport)
        return false;

    // P2P模式下至少有一个连接的玩家即为连接状态
    return !m_playerList.empty();
}

/**
 * @brief 处理传输层接收到的数据，根据消息类型分发到不同的处理函数
 * @param senderId
 * @param data
 * @param size
 * @date 2026-05-21
 */
void P2PTopology::OnTransportData(PlayerId senderId, const uint8_t *data, size_t size) {
    if (size < sizeof(NetworkMessageType))
        return;

    NetworkMessageType msgType = static_cast<NetworkMessageType>(data[0]);

    switch (msgType) {
    // P2P 帧同步内部消息
    case NetworkMessageType::P2PInput:
        ProcessInputMessage(senderId, data + sizeof(NetworkMessageType), size - sizeof(NetworkMessageType));
        break;
    case NetworkMessageType::P2PFrameAck:
        // 帧确认消息（可扩展实现）
        break;
    // 游戏层消息（0x30 及以上）
    default: {
        if (msgType >= NetworkMessageType::GameCustom && m_onGameMessage) {
            ProcessGameMessage(senderId, data + sizeof(NetworkMessageType), size - sizeof(NetworkMessageType));
        }
        break;
    }
    }
}

/**
 * @brief 处理 P2P 输入消息
 * @param senderId 发送者玩家 ID
 * @param data 输入消息数据
 * @param size
 * @date 2026-05-21
 */
void P2PTopology::ProcessInputMessage(PlayerId senderId, const uint8_t *data, size_t size) {
    if (size < sizeof(uint32_t) + sizeof(uint32_t)) // frameNum + inputData
        return;

    // 解析输入消息
    uint32_t frameNum = *reinterpret_cast<const uint32_t *>(data);
    uint32_t inputData = *reinterpret_cast<const uint32_t *>(data + sizeof(uint32_t));

    // 查找或创建玩家输入队列
    auto &queue = m_playerInputQueues[senderId];

    // 使用 unordered_map 存储，O(1) 插入
    queue.inputs[frameNum] = inputData;

    // 更新最后接收的帧号
    if (frameNum > queue.lastReceivedFrame) {
        queue.lastReceivedFrame = frameNum;
    }

    // 尝试推进帧
    TryAdvanceFrame();
}

/**
 * @brief 处理游戏层消息，直接调用回调函数
 * @param senderId 发送者玩家 ID
 * @param data 消息数据
 * @param size
 * @date 2026-05-21
 */
void P2PTopology::ProcessGameMessage(PlayerId senderId, const uint8_t *data, size_t size) {
    if (m_onGameMessage) {
        // 外部注册的游戏消息处理
        m_onGameMessage(senderId, data, size);
    }
}

/**
 * @brief 广播本地输入给所有玩家
 * @date 2026-05-21
 */
void P2PTopology::BroadcastLocalInput() {
    if (!m_transport)
        return;

    // 构建输入消息
    // 格式: [NetworkMessageType(1)] [frameNum(4)] [inputData(4)]
    uint8_t buffer[1 + 4 + 4];
    buffer[0] = static_cast<uint8_t>(NetworkMessageType::P2PInput);
    *reinterpret_cast<uint32_t *>(buffer + 1) = m_nextLocalFrame;
    *reinterpret_cast<uint32_t *>(buffer + 1 + 4) = m_localInput;

    // 广播给所有玩家
    m_transport->Broadcast(buffer, sizeof(buffer), DeliveryMethod::Unreliable);
}

/**
 * @brief 尝试推进帧，处理已接收的输入
 * @date 2026-05-21
 */
void P2PTopology::TryAdvanceFrame() {
    // 循环检查是否可以推进帧
    while (HasAllInputsForFrame(m_currentFrame + 1)) {
        // 清理已处理的输入（保留最近几帧用于调试）
        for (auto &pair : m_playerInputQueues) {
            auto &queue = pair.second;
            auto &inputs = queue.inputs;

            // 删除已消费的帧
            for (auto it = inputs.begin(); it != inputs.end();) {
                if (it->first <= m_currentFrame) {
                    it = inputs.erase(it);
                } else {
                    ++it;
                }
            }
            queue.lastConsumedFrame = m_currentFrame;
        }

        m_currentFrame++;
    }
}

/**
 * @brief 添加玩家到 P2P 拓扑，初始化输入队列并触发回调
 * @param playerId 玩家 ID
 * @date 2026-05-21
 */
void P2PTopology::AddPlayer(PlayerId playerId) {

    // 检查是否已存在
    if (m_playerInputQueues.find(playerId) != m_playerInputQueues.end())
        return;

    // 添加到输入队列
    m_playerInputQueues[playerId] = PlayerInputQueue();

    // 添加到玩家列表
    m_playerList.push_back(playerId);

    // 触发回调
    if (m_onPlayerJoined) {
        m_onPlayerJoined(playerId);
    }
}

/**
 * @brief 从 P2P 拓扑移除玩家，清理输入队列并触发回调
 * @param playerId 玩家 ID
 * @date 2026-05-21
 */
void P2PTopology::RemovePlayer(PlayerId playerId) {
    // 从输入队列移除
    m_playerInputQueues.erase(playerId);

    // 从玩家列表移除
    auto it = std::find(m_playerList.begin(), m_playerList.end(), playerId);
    if (it != m_playerList.end()) {
        m_playerList.erase(it);
    }

    // 触发回调
    if (m_onPlayerLeft) {
        m_onPlayerLeft(playerId);
    }
}

/**
 * @brief 检查是否所有玩家都已提交指定帧的输入
 * @param frameNum 检查的帧号
 * @return bool
 * @date 2026-05-21
 */
bool P2PTopology::HasAllInputsForFrame(uint32_t frameNum) const {
    // 如果没有其他玩家，直接返回 true
    if (m_playerList.empty())
        return false;

    // 检查每个玩家是否有该帧的输入（O(1) 查找）
    for (const auto &pair : m_playerInputQueues) {
        const auto &inputs = pair.second.inputs;
        if (inputs.find(frameNum) == inputs.end()) {
            return false;
        }
    }

    return true;
}

} // namespace Network
} // namespace DX12Engine