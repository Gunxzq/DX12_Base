// Network/Implementation/NetworkTopologyP2P.h
#pragma once
#include "Network/INetworkTopology.h"
#include <queue>
#include <unordered_map>
#include <vector>

namespace DX12Engine {
namespace Network {

class INetworkTransport;

/**
 * @brief P2P 帧同步拓扑实现
 *
 * 集成到 FrameDriver 的调度阶段：
 * - Update():     接收网络包，缓存远端输入
 * - OnFrameSync(): 检查所有玩家输入是否就绪，推进当前帧
 *
 * 游戏逻辑通过 GetPlayerInput() 获取当前帧的输入
 */
class P2PTopology : public INetworkTopology {
public:
    P2PTopology();
    ~P2PTopology() override;

    // ========== 生命周期 ==========
    bool Initialize(const NetworkConfig &config, INetworkTransport *transport) override;
    void Shutdown() override;

    // ========== 帧驱动接口 ==========
    void Update(float deltaTime) override;
    void OnFrameSync() override;

    // ========== 输入接口 ==========
    void SubmitLocalInput(uint32_t inputData) override { m_localInput = inputData; };

    // ========== 数据读取接口 ==========
    uint32_t GetPlayerInput(PlayerId playerId, uint32_t frameNum) const override;
    uint32_t GetCurrentFrame() const override { return m_currentFrame; }
    size_t GetLatestSnapshot(uint8_t *outData, size_t maxSize) const override { return 0; }

    // ========== 发送接口 ==========
    void SendToPlayer(PlayerId targetPlayerId, const uint8_t *data, size_t size, bool reliable) override;
    void BroadcastToAll(const uint8_t *data, size_t size, bool reliable) override;

    // ========== 状态查询 ==========
    NetworkMode GetMode() const override { return NetworkMode::P2P; }
    bool IsConnected() const override;
    PlayerId GetLocalPlayerId() const override { return m_localPlayerId; }
    std::vector<PlayerId> GetPlayers() const override { return m_playerList; };

    // ========== 回调注册 ==========
    void SetOnPlayerJoined(OnPlayerJoinedCallback callback) override { m_onPlayerJoined = std::move(callback); }
    void SetOnPlayerLeft(OnPlayerLeftCallback callback) override { m_onPlayerLeft = std::move(callback); }
    void SetOnGameMessage(OnGameMessageCallback callback) override { m_onGameMessage = std::move(callback); }

private:
    // ========== 内部数据结构 ==========
    struct PlayerInputQueue {
        std::unordered_map<uint32_t, uint32_t> inputs; // frameNum -> inputData
        uint32_t lastReceivedFrame = 0;
        uint32_t lastConsumedFrame = 0;
    };

    // ========== 内部方法 ==========
    void OnTransportData(PlayerId senderId, const uint8_t *data, size_t size);
    void ProcessInputMessage(PlayerId senderId, const uint8_t *data, size_t size);
    void ProcessGameMessage(PlayerId senderId, const uint8_t *data, size_t size);
    void BroadcastLocalInput();
    void TryAdvanceFrame();

    void AddPlayer(PlayerId playerId);
    void RemovePlayer(PlayerId playerId);
    bool HasAllInputsForFrame(uint32_t frameNum) const;

    // ========== 成员变量 ==========
    NetworkConfig m_config;
    INetworkTransport *m_transport = nullptr;

    // 玩家管理
    PlayerId m_localPlayerId = 0;
    std::unordered_map<PlayerId, PlayerInputQueue> m_playerInputQueues;
    std::vector<PlayerId> m_playerList;

    // 帧同步状态
    uint32_t m_currentFrame = 0;
    uint32_t m_nextLocalFrame = 0;
    uint32_t m_localInput = 0;

    // 连接状态
    bool m_isRunning = false;

    // 回调
    OnPlayerJoinedCallback m_onPlayerJoined;
    OnPlayerLeftCallback m_onPlayerLeft;
    OnGameMessageCallback m_onGameMessage;
};

} // namespace Network
} // namespace DX12Engine