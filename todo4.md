
---

### 第三阶段：P2 - 引擎层协议消息

**目标**：实现 Ping/Pong/Connect/Disconnect 等引擎层消息。

**依赖**：P1 完成后

**需要做的事**：

1. 创建 `Network/NetworkProtocol.h` - 定义消息头、引擎消息结构体
2. 创建 `Network/NetworkEngineHandler` - 自动处理 Ping/Pong/RTT 计算
3. 集成到 `FrameDriver::Tick()` 中的合适位置

验证：能看到 RTT 数值变化，连接/断开有日志

---

### 第四阶段：P3 - 游戏层事件集成

**目标**：游戏消息通过 `MessageDispatcher` 发送和接收。

**依赖**：P0、P1、P2 完成后

**需要做的事**：

1. 创建 `Network/NetworkGameSender.h` - 游戏消息发送接口（使用 `MessageDispatcher`）
2. 创建 `NetworkReceiveSystem` - 从 `MessageDispatcher` 接收网络事件并转发给游戏
3. 在 `TaskGraphBuilder` 中注册网络接收 System

验证：两个客户端能通过事件系统交换游戏消息



远端网络消息 → 接收 → 反序列化 → NetworkEventReceiver → PostEvent → 本地事件系统
                                                                          ↓
                                                              游戏 System 响应