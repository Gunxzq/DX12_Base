// Network/INetworkTopology.h
#pragma once
#include "Network/Struct/NetworkConfig.h"
#include "Network/Struct/NetworkTypes.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace DX12Engine {
namespace Network {

class INetworkTransport;

/**
 * @brief 网络拓扑层抽象接口
 *
 * 集成到 FrameDriver 的调度阶段：
 * - EarlyUpdate: 调用 Update() 处理网络包
 * - FrameSync:   调用 OnFrameSync() 处理同步逻辑
 *
 * 游戏逻辑通过 GetPlayerInput() / GetLatestSnapshot() 获取网络数据
 */
class INetworkTopology {
public:
    virtual ~INetworkTopology() = default;

    // ====================================================================
    // 生命周期
    // ====================================================================

    virtual bool Initialize(const NetworkConfig &config, INetworkTransport *transport) = 0;
    virtual void Shutdown() = 0;

    // ====================================================================
    // 帧驱动接口（由 FrameDriver 在特定阶段调用）
    // ====================================================================

    /**
     * @brief 每帧更新（在 EarlyUpdate 阶段调用）
     *
     * 职责：
     * - 调用 Transport->Update() 收发原始数据
     * - 处理连接事件
     * - 接收远端消息并缓存
     */
    virtual void Update(float deltaTime) = 0;

    /**
     * @brief 帧同步点（在 FrameSync 回调中调用）
     *
     * 职责：
     * - P2P: 检查所有玩家输入是否就绪，推进当前帧
     * - CS: 服务器广播快照，客户端接收并和解
     */
    virtual void OnFrameSync() = 0;

    // ====================================================================
    // 输入接口（由 InputSystem 在 EarlyUpdate 之前调用）
    // ====================================================================

    /**
     * @brief 提交本地输入（立即生效，零延迟）
     *
     * 调用时机：InputSystem 更新后，Update 阶段前
     * 职责：存储本地输入，同时标记为“待发送”
     */
    virtual void SubmitLocalInput(uint32_t inputData) = 0;

    // ====================================================================
    // 数据读取接口（由游戏 System 在 Update 阶段调用）
    // ====================================================================

    /**
     * @brief 获取指定玩家的输入（P2P 模式）
     * @param playerId 玩家 ID
     * @param frameNum 帧号（0 = 当前帧）
     */
    virtual uint32_t GetPlayerInput(PlayerId playerId, uint32_t frameNum) const = 0;

    /**
     * @brief 获取当前帧号（P2P 模式）
     */
    virtual uint32_t GetCurrentFrame() const = 0;

    /**
     * @brief 获取最新快照（CS 客户端模式）
     */
    virtual size_t GetLatestSnapshot(uint8_t *outData, size_t maxSize) const = 0;

    // ====================================================================
    // 发送接口（由游戏 System 调用）
    // ====================================================================

    virtual void SendToPlayer(PlayerId targetPlayerId, const uint8_t *data, size_t size, bool reliable) = 0;
    virtual void BroadcastToAll(const uint8_t *data, size_t size, bool reliable) = 0;

    // ====================================================================
    // 状态查询
    // ====================================================================

    virtual NetworkMode GetMode() const = 0;
    virtual bool IsConnected() const = 0;
    virtual PlayerId GetLocalPlayerId() const = 0;
    virtual std::vector<PlayerId> GetPlayers() const = 0;

    // ====================================================================
    // 回调注册
    // ====================================================================

    using OnPlayerJoinedCallback = std::function<void(PlayerId playerId)>;
    using OnPlayerLeftCallback = std::function<void(PlayerId playerId)>;
    using OnGameMessageCallback = std::function<void(PlayerId senderId, const uint8_t *data, size_t size)>;

    virtual void SetOnPlayerJoined(OnPlayerJoinedCallback callback) = 0;
    virtual void SetOnPlayerLeft(OnPlayerLeftCallback callback) = 0;
    virtual void SetOnGameMessage(OnGameMessageCallback callback) = 0;
};

} // namespace Network
} // namespace DX12Engine