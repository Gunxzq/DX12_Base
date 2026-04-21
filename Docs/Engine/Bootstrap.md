# Bootstrap (引导启动模块)

## 1. 概述

Bootstrap 是游戏引擎的**装配层**，而非运行层。

| 职责定位 | 说明 |
|:--------|:-----|
| **做什么** | 初始化基础设施、组装依赖、创建 Game 实例并注入能力 |
| **不做什么** | 不持有消息循环、不管理运行时生命周期、不进入主循环 |

**设计哲学**：Bootstrap 是"工厂 + 装配工"，负责将散落的子系统组装成可运行的 Game 实例，然后移交控制权。

---

## 2. 核心职责

### 2.1 初始化基础设施

| 顺序 | 子系统 | 说明 |
|:----:|:-------|:-----|
| 1 | ConfigManager | 读取配置文件，解析参数 |
| 2 | Logging | 根据配置初始化日志输出 |
| 3 | Window | 创建操作系统窗口 |

### 2.2 依赖组装

- 按依赖顺序初始化底层系统
- 创建具体子系统的实现实例
- 将能力**注入**给 Game 实例

### 2.3 异常捕获

- 设置全局异常处理器
- 处理启动阶段的致命错误
- 提供友好的错误信息

### 2.4 控制权移交

```cpp
// Bootstrap 的职责到此为止
Game game(window, configManager);  // 创建并注入能力
return game.Run();                  // 移交控制权给 Game
```

---

## 3. 架构图表

### 3.1 模块职责边界

```mermaid
graph TB
    subgraph "操作系统层"
        A["main() / wWinMain()"]
    end

    subgraph "Bootstrap (装配层)"
        B["Initialize()"]
        C["ConfigManager::Initialize()"]
        D["Logging::Initialize()"]
        E["Window::Create()"]
        F["new Game(...)"]
    end

    subgraph "Game (运行层)"
        G["Run()"]
        H["Shutdown()"]
        I["Update()"]
        J["Render()"]
    end

    A -->|"调用"| B
    B --> C
    B --> D
    B --> E
    C -->|"依赖配置"| D
    E --> F
    F -->|"注入能力"| G
    B -->|"移交控制"| G

    G --> I
    G --> J
    G --> H

    style A fill:#f9f,stroke:#333,stroke-width:2px
    style B fill:#e8f5e9,stroke:#2e7d32
    style G fill:#e3f2fd,stroke:#1565c0
```

### 3.2 启动时序图

```mermaid
sequenceDiagram
    participant main as main() / wWinMain()
    participant Bootstrap
    participant ConfigMgr as ConfigManager
    participant Logging
    participant Window
    participant Game

    main->>Bootstrap: Initialize(hInstance, cmdShow)
    Activate Bootstrap

    Bootstrap->>ConfigMgr: Initialize()
    ConfigMgr-->>Bootstrap: OK

    Bootstrap->>Logging: Initialize(config)
    Logging-->>Bootstrap: OK

    Bootstrap->>Window: Create(config)
    Window-->>Bootstrap: window handle

    Bootstrap->>Game: new Game(window, config, logging)
    Note over Game: 接收注入的能力

    Bootstrap-->>main: game instance
    Deactivate Bootstrap

    main->>Game: Run()
    Activate Game

    loop Game Loop
        Game->>Window: ProcessMessages()
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

    subgraph "基础子系统"
        C["ConfigManager"]
        L["Logging"]
        W["Window"]
    end

    subgraph "Game (运行层)"
        G["Game"]
    end

    B -->|"创建"| C
    B -->|"创建"| L
    B -->|"创建"| W
    C -->|"提供配置"| L
    L -->|"依赖"| C

    B -->|"创建 & 注入"| G
    G -->|"使用"| W
    G -->|"使用"| C
    G -->|"使用"| L

    style B fill:#e8f5e9,stroke:#2e7d32
    style G fill:#e3f2fd,stroke:#1565c0
```

### 3.4 能力注入模式

```mermaid
graph LR
    subgraph "定义层"
        I["interface IRenderer"]
    end

    subgraph "Bootstrap (创建 & 注入)"
        BR["DX12Renderer"]
        BG["new Game(renderer)"]
    end

    subgraph "Game (使用)"
        GR["m_Renderer->DrawTriangle()"]
    end

    I --> BR
    BR --> BG
    BG --> GR

    style I fill:#fff3e0,stroke:#e65100
    style BR fill:#e8f5e9,stroke:#2e7d32
    style GR fill:#e3f2fd,stroke:#1565c0
```

```cpp
// 1. 定义能力接口
class IRenderer {
public:
    virtual void DrawTriangle() = 0;
    virtual ~IRenderer() = default;
};

// 2. Game 只依赖接口，不知道具体实现
class Game {
private:
    IRenderer*     m_Renderer;     // 注入的能力
    ConfigManager* m_Config;       // 注入的能力
    Logging*       m_Logging;      // 注入的能力

public:
    Game(IRenderer* renderer, ConfigManager* config, Logging* logging)
        : m_Renderer(renderer)
        , m_Config(config)
        , m_Logging(logging) {}

    void Run();  // 拥有消息循环和主循环
};

// 3. Bootstrap 负责制造具体能力并注入
class Bootstrap {
public:
    Game* Initialize() {
        // 初始化基础设施
        auto config = new ConfigManager();
        auto logging = new Logging();
        auto window = new Window();

        // 制造具体能力
        auto renderer = new DX12Renderer();

        // 注入给 Game
        return new Game(window, config, renderer);
    }
};
```

### 3.5 PlantUML 组件图


```mermaid
graph LR
    subgraph "操作系统"
        Main["WinMain / main"]
    end

    subgraph "Bootstrap"
        EB["EngineBootstrap"]
    end

    subgraph "基础子系统"
        direction TB
        CM["ConfigManager"]
        Log["Logging"]
        Win["Window"]
    end

    subgraph "Game"
        G["Game"]
        GL["GameLoop"]
    end

    %% 关系连接
    Main -->|"入口点"| EB
    
    EB -->|"创建"| CM
    EB -->|"创建"| Log
    EB -->|"创建"| Win
    EB -->|"注入能力"| G
    
    CM -->|"提供配置"| Log
    
    G -->|"Run()"| GL

    %% 样式定义 (对应原 PlantUML 颜色)
    style EB fill:#palegreen,stroke:#333,stroke-width:1px
    
    style CM fill:#lightblue,stroke:#333,stroke-width:1px
    style Log fill:#lightblue,stroke:#333,stroke-width:1px
    style Win fill:#lightblue,stroke:#333,stroke-width:1px
    
    style G fill:#lightcyan,stroke:#333,stroke-width:1px
    style GL fill:#lightcyan,stroke:#333,stroke-width:1px
    
    style Main fill:#f9f9f9,stroke:#333,stroke-width:1px

    %% 注释说明 (通过子图标题或节点文本体现，保持图表简洁)
    click EB "javascript:void(0)" "Bootstrap职责: 加载配置、初始化子系统、异常处理、能力注入" _blank
    click G "javascript:void(0)" "Game职责: 持有主循环、组合游戏逻辑、管理生命周期" _blank
```

---

## 4. 职责边界总结

| 组件 | Bootstrap | Game | wWinMain |
|:-----|:---------:|:----:|:--------:|
| 创建 ConfigManager | ✅ | ❌ | ❌ |
| 创建 Logging | ✅ | ❌ | ❌ |
| 创建 Window | ✅ | ❌ | ❌ |
| 创建 Game 实例 | ✅ | ❌ | ❌ |
| 持有消息循环 | ❌ | ✅ | ❌ |
| 管理生命周期 | ❌ | ✅ | ❌ |
| 调用 Game.Run() | ❌ | ❌ | ✅ |

---

## 5. 设计原则

| 原则 | 说明 |
|:-----|:-----|
| **配置驱动** | 根据配置文件决定初始化哪些模块 |
| **按需初始化** | 只初始化配置中启用的模块 |
| **快速失败** | 配置无效或初始化失败时立即终止 |
| **能力注入** | 通过构造函数或工厂方法注入依赖 |
| **单一职责** | Bootstrap 只做装配，不做运行 |

---

## 6. 未来扩展

随着引擎发展，Bootstrap 将逐步初始化更多子系统：

| 顺序 | 子系统 | 说明 |
|:----:|:-------|:-----|
| 4 | Memory Allocator | 内存管理器 |
| 5 | File System | 文件系统 |
| 6 | Render Backend | 渲染后端 (DX12/Vulkan) |
| 7 | Audio System | 音频系统 |
| 8 | Input System | 输入系统 |
| 9 | Physics System | 物理系统 |

所有新增子系统都遵循相同的注入模式：Bootstrap 创建 → 注入 Game → Game 使用。

