明白了，你的需求就是 **引擎提供机制，游戏开发者根据游戏类型选择 P2P 或服务器模式，甚至可以同时支持（通过配置文件/编译选项切换）。**

这是一个非常成熟的引擎架构设计。下面是一套清晰的抽象方案：

---

## 🏗️ 统一抽象层设计

### 1. 核心接口

```cpp
// 网络会话接口（引擎层）
class INetworkSession {
public:
    virtual ~INetworkSession() = default;
    
    // 生命周期
    virtual bool StartHost(uint16_t port) = 0;
    virtual bool JoinSession(const std::string& address, uint16_t port) = 0;
    virtual void LeaveSession() = 0;
    
    // 发送数据
    virtual void SendToAll(const uint8_t* data, size_t size) = 0;
    virtual void SendToPlayer(int playerId, const uint8_t* data, size_t size) = 0;
    
    // 接收数据（每帧调用）
    virtual void PollEvents() = 0;
    virtual bool HasPendingData() = 0;
    virtual bool ReceiveNextData(uint8_t* buffer, size_t& outSize, int& outSenderId) = 0;
    
    // 状态查询
    virtual bool IsHost() const = 0;
    virtual int GetPlayerId() const = 0;
    virtual int GetPlayerCount() const = 0;
};
```

### 2. 两种实现

```cpp
// P2P 实现（使用 Enet / SteamNetworkingSockets）
class P2PSession : public INetworkSession {
    // 主机负责：广播输入、接收状态
    // 客户端负责：发送输入、接收状态
};

// 专用服务器实现
class ServerSession : public INetworkSession {
    // 客户端始终连接中央服务器
    // 服务器负责：计算权威状态、广播
};
```

### 3. 游戏层选择

```cpp
// 引擎初始化时
std::unique_ptr<INetworkSession> session;

if (config.networkMode == "p2p") {
    session = std::make_unique<P2PSession>();
} else if (config.networkMode == "dedicated_server") {
    session = std::make_unique<ServerSession>();
}

// 游戏循环中
session->PollEvents();
while (session->HasPendingData()) {
    session->ReceiveNextData(buffer, size, senderId);
    // 处理网络数据（更新 ECS 组件）
}
```

---

## 🔧 同步模式的选择

| 同步模式 | 适合游戏类型 | 带宽 | 确定性要求 |
|:---|:---|:---|:---|
| **输入同步（帧同步）** | RTS、格斗、平台跳跃 | 极低 | **高**（逻辑必须确定性） |
| **状态同步** | FPS、MOBA、动作游戏 | 较高 | 低 |

**你可以在 `INetworkSession` 之上再抽象一层同步策略**：

```cpp
class ISyncStrategy {
public:
    virtual void OnLocalInput(const InputState& input) = 0;
    virtual void OnNetworkData(const uint8_t* data, size_t size, int sender) = 0;
    virtual void Update(float dt) = 0;  // 每帧更新同步逻辑
};
```

这样：
- **帧同步实现**：收集输入 → 主机广播 → 所有客户端执行
- **状态同步实现**：客户端预测 → 发送输入 → 服务器权威 → 广播状态

---

## 📁 建议的文件结构

```
Engine/Network/
├── INetworkSession.h          // 抽象接口
├── P2PSession.h/cpp           // P2P 实现（Enet/Steam）
├── ServerSession.h/cpp        // 专用服务器实现
├── ISyncStrategy.h            // 同步策略抽象
├── FrameSyncStrategy.h/cpp    // 帧同步实现
├── StateSyncStrategy.h/cpp    // 状态同步实现
└── NetworkConfig.h            // 配置结构体
```

---

## ✅ 下一步行动

| 优先级 | 任务 | 时间 |
|:---|:---|:---|
| **P0** | 定义 `INetworkSession` 接口 | 0.5 天 |
| **P0** | 用 Enet 实现 `P2PSession`（打通收发） | 2 天 |
| **P1** | 实现 `ServerSession`（TCP 版先占位） | 1 天 |
| **P1** | 定义 `ISyncStrategy` + `FrameSyncStrategy` | 1 天 |
| **P2** | 集成到 `FrameDriver` 中 | 1 天 |

