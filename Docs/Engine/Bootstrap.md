# Bootstrap (引导启动模块)

## 1. 概述

Bootstrap 是游戏引擎的**装配层**，负责初始化基础设施、创建 Context 并填充能力、最终创建 Game 实例。

| 职责定位 | 说明 |
|:--------|:-----|
| **做什么** | 初始化基础设施、创建 Context、组装依赖、将 GameContext 注入 Game |
| **不做什么** | 不持有消息循环、不管理运行时生命周期、不进入主循环 |

**设计哲学**：Bootstrap 是"工厂 + 装配工"，负责将散落的子系统组装成 Context，然后创建 Game 并注入 Context，最后移交控制权。

---

## 2. 核心职责

### 2.1 初始化基础设施

| 顺序 | 子系统 | 说明 |
|:----:|:-------|:-----|
| 1 | ConfigManager | 读取配置文件，解析参数 |
| 2 | Logging | 根据配置初始化日志输出 |
| 3 | Window | 创建操作系统窗口 |

### 2.2 创建并填充 Context

Bootstrap 创建 GameContext 并填充具体能力：

```cpp
GameContext* Bootstrap::CreateContext() {
    auto ctx = new GameContext();

    // 基础子系统
    ctx->Config   = new JsonConfigManager("config.json");
    ctx->Logging  = new FileLogging(ctx->Config);
    ctx->Window   = new DX12Window(ctx->Config);

    // 渲染和其他子系统
    ctx->Renderer = new DX12Renderer(ctx->Window);

    return ctx;
}
```

### 2.3 异常捕获

- 设置全局异常处理器
- 处理启动阶段的致命错误
- 提供友好的错误信息

### 2.4 创建 Game 并移交控制权

```cpp
// Bootstrap 的职责到此为止
GameContext* ctx = CreateContext();  // 创建并填充 Context
Game game(ctx);                        // 将 Context 注入 Game
return game.Run();                     // 移交控制权给 Game
```

---

## 3. 架构图表

### 3.1 模块职责边界（包含 Context）

```mermaid
graph TB
    subgraph "操作系统层"
        A["main() / wWinMain()"]
    end

    subgraph "Bootstrap (装配层)"
        B["Initialize()"]
        C["CreateContext()"]
        D["填充能力到 Context"]
    end

    subgraph "Context (中间层)"
        E["GameContext"]
        E1["Window*"]
        E2["Renderer*"]
        E3["Logging*"]
        E4["Config*"]
    end

    subgraph "Game (运行层)"
        G["Run()"]
        H["Shutdown()"]
        I["Update()"]
        J["Render()"]
    end

    A -->|"调用"| B
    B --> C
    C --> D
    D -->|"填充"| E1
    D -->|"填充"| E2
    D -->|"填充"| E3
    D -->|"填充"| E4

    C -->|"返回"| E
    E -->|"注入"| G
    B -->|"移交控制"| G

    style A fill:#f9f,stroke:#333,stroke-width:2px
    style B fill:#e8f5e9,stroke:#2e7d32
    style E fill:#fff3e0,stroke:#e65100
    style G fill:#e3f2fd,stroke:#1565c0
```

### 3.2 启动时序图

```mermaid
sequenceDiagram
    participant main as main() / wWinMain()
    participant Bootstrap
    participant Context as GameContext
    participant Game

    main->>Bootstrap: Initialize(hInstance, cmdShow)
    Activate Bootstrap

    Bootstrap->>Bootstrap: 初始化 ConfigManager
    Bootstrap->>Bootstrap: 初始化 Logging

    Bootstrap->>Context: new GameContext()
    Note over Context: 创建空的上下文容器

    Bootstrap->>Context: Context->Config = new JsonConfig()
    Bootstrap->>Context: Context->Logging = new FileLogging()
    Bootstrap->>Context: Context->Window = new DX12Window()
    Bootstrap->>Context: Context->Renderer = new DX12Renderer()
    Note over Context: 填充所有能力

    Bootstrap->>Game: new Game(Context)
    Context-->>Game: Context 指针
    Deactivate Bootstrap

    main->>Game: Run()
    Activate Game

    loop Game Loop
        Game->>Context: Context->Window->ProcessMessages()
        Game->>Game: Update()
        Game->>Game: Render()
    end

    Game-->>main: exitCode
    Deactivate Game
```

### 3.3 依赖层级

```mermaid
graph TB
    subgraph "Bootstrap (装配层)"
        B["Bootstrap"]
    end

    subgraph "Context (中间层)"
        C["GameContext"]
        C1["Window*"]
        C2["Renderer*"]
        C3["Logging*"]
        C4["Config*"]
    end

    subgraph "具体实现"
        W["DX12Window"]
        R["DX12Renderer"]
        L["FileLogging"]
        CF["JsonConfig"]
    end

    subgraph "Game (运行层)"
        G["Game"]
    end

    B -->|"创建"| C
    B -->|"创建"| W
    B -->|"创建"| R
    B -->|"创建"| L
    B -->|"创建"| CF

    W -->|"填充"| C1
    R -->|"填充"| C2
    L -->|"填充"| C3
    CF -->|"填充"| C4

    C -->|"注入"| G

    style B fill:#e8f5e9,stroke:#2e7d32
    style C fill:#fff3e0,stroke:#e65100
    style G fill:#e3f2fd,stroke:#1565c0
```

### 3.4 能力注入模式（通过 Context）

```mermaid
graph LR
    subgraph "Bootstrap 创建"
        B["Bootstrap"]
        W["DX12Window"]
        R["DX12Renderer"]
        L["FileLogging"]
    end

    subgraph "Context 持有接口"
        C["GameContext"]
        C1["IWindow*"]
        C2["IRenderer*"]
        C3["ILogging*"]
    end

    subgraph "Game 使用"
        G["Game"]
        GL["GameLoop"]
    end

    B -->|"new"| W
    B -->|"new"| R
    B -->|"new"| L

    W -->|"填充"| C1
    R -->|"填充"| C2
    L -->|"填充"| C3

    C -->|"注入"| G
    C1 & C2 & C3 -->|"使用"| GL

    style B fill:#e8f5e9,stroke:#2e7d32
    style C fill:#fff3e0,stroke:#e65100
    style G fill:#e3f2fd,stroke:#1565c0
```

```cpp
// 1. 定义能力接口
class IRenderer {
public:
    virtual void DrawTriangle() = 0;
    virtual ~IRenderer() = default;
};

// 2. Context 只声明接口指针
class GameContext {
public:
    IRenderer*  Renderer = nullptr;
    IWindow*    Window   = nullptr;
    ILogging*   Logging  = nullptr;
};

// 3. Game 只依赖 Context
class Game {
private:
    GameContext* m_Context;  // 单一的注入点

public:
    Game(GameContext* ctx) : m_Context(ctx) {}
    void Render() { m_Context->Renderer->DrawTriangle(); }
};

// 4. Bootstrap 负责制造具体能力并填充 Context
class Bootstrap {
public:
    Game* CreateGame() {
        auto ctx = new GameContext();
        ctx->Window   = new DX12Window();
        ctx->Renderer = new DX12Renderer();
        ctx->Logging  = new FileLogging();
        return new Game(ctx);
    }
};
```

### 3.5 完整架构 PlantUML 风格

```mermaid
graph LR
    subgraph "操作系统"
        Main["WinMain / main"]
    end

    subgraph "Bootstrap (装配层)"
        EB["EngineBootstrap"]
    end

    subgraph "Context (中间层)"
        CTX["GameContext"]
        CTX1["Window*"]
        CTX2["Renderer*"]
        CTX3["Logging*"]
        CTX4["Config*"]
    end

    subgraph "基础子系统 (具体实现)"
        direction TB
        CM["ConfigManager"]
        Log["Logging"]
        Win["Window"]
        Ren["Renderer"]
    end

    subgraph "Game (运行层)"
        G["Game"]
        GL["GameLoop"]
    end

    %% 关系连接
    Main -->|"入口点"| EB

    EB -->|"创建"| CM
    EB -->|"创建"| Log
    EB -->|"创建"| Win
    EB -->|"创建"| Ren

    CM -->|"填充"| CTX4
    Log -->|"填充"| CTX3
    Win -->|"填充"| CTX1
    Ren -->|"填充"| CTX2

    CTX4 & CTX3 & CTX2 & CTX1 -->|"持有"| CTX

    EB -->|"填充 Context"| CTX
    CTX -->|"注入"| G

    G -->|"Run()"| GL

    %% 样式定义
    style EB fill:#e8f5e9,stroke:#2e7d32
    style CTX fill:#fff3e0,stroke:#e65100
    style G fill:#e3f2fd,stroke:#1565c0
    style CM fill:#bbdefb,stroke:#1565c0
    style Log fill:#bbdefb,stroke:#1565c0
    style Win fill:#bbdefb,stroke:#1565c0
    style Ren fill:#bbdefb,stroke:#1565c0
```

---

## 4. 职责边界总结

| 组件 | Bootstrap | Context | Game | wWinMain |
|:-----|:---------:|:-------:|:----:|:--------:|
| 创建 ConfigManager | ✅ | ❌ | ❌ | ❌ |
| 创建 Logging | ✅ | ❌ | ❌ | ❌ |
| 创建 Window | ✅ | ❌ | ❌ | ❌ |
| 创建 GameContext | ✅ | ❌ | ❌ | ❌ |
| 填充 Context 能力 | ✅ | ❌ | ❌ | ❌ |
| 持有能力指针 | ❌ | ✅ | ❌ | ❌ |
| 创建 Game 实例 | ✅ | ❌ | ❌ | ❌ |
| 持有消息循环 | ❌ | ❌ | ✅ | ❌ |
| 管理生命周期 | ❌ | ❌ | ✅ | ❌ |
| 调用 Game.Run() | ❌ | ❌ | ❌ | ✅ |

---

## 5. 设计原则

| 原则 | 说明 |
|:-----|:-----|
| **配置驱动** | 根据配置文件决定初始化哪些模块 |
| **按需初始化** | 只初始化配置中启用的模块 |
| **快速失败** | 配置无效或初始化失败时立即终止 |
| **Context 填充** | 通过 Context 统一注入能力，而非单独注入 |
| **单一职责** | Bootstrap 只做装配，不做运行 |

---

## 6. 未来扩展

随着引擎发展，Bootstrap 将逐步初始化更多子系统：

| 顺序 | 子系统 | Context 字段 | 说明 |
|:----:|:-------|:-------------|:-----|
| 5 | Memory Allocator | MemoryAllocator* | 内存管理器 |
| 6 | File System | FileSystem* | 文件系统 |
| 7 | Render Backend | Renderer* | 渲染后端 (DX12/Vulkan) |
| 8 | Audio System | AudioSystem* | 音频系统 |
| 9 | Input System | InputSystem* | 输入系统 |
| 10 | Physics System | PhysicsSystem* | 物理系统 |

所有新增子系统都遵循相同的模式：Bootstrap 创建 → 填充到 Context → Game 通过 Context 使用。
