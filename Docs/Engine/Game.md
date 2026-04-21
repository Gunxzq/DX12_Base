# Game (游戏主逻辑)

## 1. 概述

Game 是游戏引擎的**运行层**，负责：

- 接收 Bootstrap 注入的基础能力
- 持有并运行主循环 (Game Loop)
- 组合游戏逻辑模块
- 管理整个游戏的生命周期

| 职责定位 | 说明 |
|:--------|:-----|
| **做什么** | 运行主循环、组合游戏逻辑、管理 Update/Render |
| **不做什么** | 不创建基础设施（由 Bootstrap 创建并注入） |

**设计哲学**：Game 是"运行者 + 组合者"，负责将基础能力和游戏逻辑组合成完整的游戏体验。

---

## 2. 核心职责

### 2.1 接收注入的能力

Game 不创建基础子系统，而是接收 Bootstrap 注入的实例：

| 能力 | 来源 | 用途 |
|:-----|:-----|:-----|
| Window | Bootstrap 创建并注入 | 窗口消息处理、渲染目标 |
| ConfigManager | Bootstrap 创建并注入 | 读取游戏配置 |
| Logging | Bootstrap 创建并注入 | 记录游戏日志 |

### 2.2 运行主循环

```cpp
void Game::Run() {
    while (!m_window->ShouldClose()) {
        m_window->ProcessMessages();  // 消息循环
        Update();                       // 更新逻辑
        Render();                       // 渲染画面
    }
    Shutdown();
}
```

### 2.3 组合游戏逻辑模块

Game 负责组合和协调各个游戏逻辑子系统：

| 模块 | 职责 | Game 的角色 |
|:-----|:-----|:-----------|
| AssetManager | 资源加载与管理 | 组合 (Composition) |
| InputManager | 输入处理 | 组合 (Composition) |
| PlayerManager | 玩家控制 | 组合 (Composition) |
| LevelManager | 关卡加载 | 组合 (Composition) |
| AudioManager | 音频播放 | 组合 (Composition) |

---

## 3. 架构图表

### 3.1 模块组合关系

```mermaid
graph TB
    subgraph "Game"
        subgraph "注入的基础能力"
            W["Window"]
            C["ConfigManager"]
            L["Logging"]
        end

        subgraph "游戏逻辑模块 (组合)"
            P["PlayerManager"]
            LV["LevelManager"]
            A["AssetManager"]
            IN["InputManager"]
            AU["AudioManager"]
        end

        subgraph "主循环"
            RL["Run()"]
            UP["Update()"]
            RE["Render()"]
            SD["Shutdown()"]
        end
    end

    W & C & L -->|"使用"| UP
    W -->|"消息"| RL
    P & LV & A & IN & AU -->|"组合"| UP
    P & LV & A & IN & AU -->|"组合"| RE

    RL --> UP
    RL --> RE
    RL --> SD

    style W fill:#bbdefb,stroke:#1565c0
    style C fill:#bbdefb,stroke:#1565c0
    style L fill:#bbdefb,stroke:#1565c0
    style P fill:#c8e6c9,stroke:#2e7d32
    style LV fill:#c8e6c9,stroke:#2e7d32
    style A fill:#c8e6c9,stroke:#2e7d32
    style IN fill:#c8e6c9,stroke:#2e7d32
    style AU fill:#c8e6c9,stroke:#2e7d32
```

### 3.2 Game Loop 时序

```mermaid
sequenceDiagram
    participant Bootstrap
    participant Game
    participant Window
    participant InputMgr as InputManager
    participant Logic
    participant Renderer
    participant AudioMgr as AudioManager

    Bootstrap->>Game: new Game(window, config, logging)
    Note over Game: 初始化游戏逻辑模块

    Game->>Game: Run()
    loop 每帧
        Game->>Window: ProcessMessages()
        Window-->>Game: 消息处理完成

        Game->>InputMgr: ProcessInput()
        InputMgr-->>Game: 输入状态更新

        Game->>Logic: Update(deltaTime)
        Logic->>AudioMgr: Update()
        Logic-->>Game: 逻辑更新完成

        Game->>Renderer: Render()
        Renderer-->>Game: 渲染完成
    end

    Game->>Game: Shutdown()
    Note over Game: 清理游戏资源
    Game-->>Bootstrap: exitCode
```

### 3.3 生命周期流程

```mermaid
stateDiagram-v2
    [*] --> Bootstrap: Bootstrap 创建
    Bootstrap --> Game: new Game(注入能力)
    Game --> Initialized: 构造函数
    Initialized --> Running: Run()
    Running --> Running: Update()
    Running --> Running: Render()
    Running --> Shutdown: ShouldClose() == true
    Shutdown --> [*]: return exitCode

    state Bootstrap {
        [*] --> 创建 ConfigManager
        创建 ConfigManager --> 创建 Logging
        创建 Logging --> 创建 Window
        创建 Window --> 注入能力给 Game
    }

    state Game {
        [*] --> 初始化 Managers
        初始化 Managers --> 消息循环
        消息循环 --> Update
        Update --> Render
        Render --> 消息循环: 继续
    }
```

### 3.4 PlantUML 组件图

```mermaid
graph LR
    subgraph "基础子系统 (注入)"
        direction TB
        W["Window"]
        C["ConfigManager"]
        L["Logging"]
    end

    subgraph "游戏逻辑模块"
        direction TB
        IM["InputManager"]
        PM["PlayerManager"]
        LM["LevelManager"]
        AM["AssetManager"]
        AU["AudioManager"]
    end

    subgraph "Game Core"
        G["Game"]
        GL["GameLoop"]
    end

    %% 依赖关系：基础子系统 -> Game
    W -->|"ProcessMessages"| GL
    C -->|"提供配置"| G
    L -->|"记录日志"| G

    %% 组合关系：Game -> 游戏逻辑模块
    G -->|"组合"| IM
    G -->|"组合"| PM
    G -->|"组合"| LM
    G -->|"组合"| AM
    G -->|"组合"| AU

    %% 内部关系
    G -->|"Run()"| GL

    %% 样式定义 (对应原 PlantUML 颜色)
    style W fill:#lightblue,stroke:#333,stroke-width:1px
    style C fill:#lightblue,stroke:#333,stroke-width:1px
    style L fill:#lightblue,stroke:#333,stroke-width:1px
    
    style IM fill:#lightgreen,stroke:#333,stroke-width:1px
    style PM fill:#lightgreen,stroke:#333,stroke-width:1px
    style LM fill:#lightgreen,stroke:#333,stroke-width:1px
    style AM fill:#lightgreen,stroke:#333,stroke-width:1px
    style AU fill:#lightgreen,stroke:#333,stroke-width:1px
    
    style G fill:#lightcyan,stroke:#333,stroke-width:1px
    style GL fill:#lightcyan,stroke:#333,stroke-width:1px

    %% 注释说明 (Mermaid 中通常通过子图标题或节点文本体现，此处保留关键信息)
    click G "javascript:void(0)" "Game 职责: 接收注入、组合模块、管理生命周期" _blank
```


### 3.5 依赖关系总览

```mermaid
graph LR
    subgraph "Bootstrap 创建"
        BM["Bootstrap"]
        W["Window"]
        C["ConfigManager"]
        L["Logging"]
    end

    subgraph "注入 Game"
        G["Game"]
        IM["InputManager"]
        PM["PlayerManager"]
        LM["LevelManager"]
    end

    BM -->|"创建"| W
    BM -->|"创建"| C
    BM -->|"创建"| L
    BM -->|"注入"| G

    G -->|"使用"| W
    G -->|"使用"| C
    G -->|"使用"| L
    G -->|"组合"| IM
    G -->|"组合"| PM
    G -->|"组合"| LM

    style BM fill:#e8f5e9,stroke:#2e7d32
    style G fill:#e3f2fd,stroke:#1565c0
```

---

## 4. 代码示例

### 4.1 基础结构

```cpp
class Game {
private:
    // ── 注入的基础能力 ──
    Window*        m_Window;
    ConfigManager* m_ConfigManager;
    Logging*       m_Logging;

    // ── 游戏逻辑模块 ──
    std::unique_ptr<InputManager>    m_InputManager;
    std::unique_ptr<PlayerManager>    m_PlayerManager;
    std::unique_ptr<LevelManager>     m_LevelManager;
    std::unique_ptr<AssetManager>     m_AssetManager;
    std::unique_ptr<AudioManager>     m_AudioManager;

    // ── 运行状态 ──
    bool m_IsRunning;

public:
    // 构造函数接收注入的能力
    Game(Window* window, ConfigManager* config, Logging* logging)
        : m_Window(window)
        , m_ConfigManager(config)
        , m_Logging(logging)
        , m_IsRunning(true) {
        InitializeModules();
    }

    // 主循环
    int Run() {
        while (m_Window->ShouldClose() == false && m_IsRunning) {
            m_Window->ProcessMessages();
            Update();
            Render();
        }
        Shutdown();
        return 0;
    }

    void Update();    // 更新所有逻辑模块
    void Render();    // 渲染画面
    void Shutdown();  // 清理资源
};
```

### 4.2 模块组合示例

```cpp
void Game::Update() {
    float deltaTime = CalculateDeltaTime();

    // 更新基础能力
    m_InputManager->Update();

    // 组合游戏逻辑
    m_PlayerManager->Update(deltaTime);
    m_LevelManager->Update(deltaTime);
    m_AudioManager->Update(deltaTime);
}

void Game::Render() {
    // 使用注入的 Window 作为渲染目标
    m_Window->BeginFrame();

    // 渲染各个逻辑模块
    m_LevelManager->Render();
    m_PlayerManager->Render();

    m_Window->EndFrame();
}
```

---

## 5. 职责边界总结

| 能力/模块 | Bootstrap | Game |
|:----------|:---------:|:----:|
| 创建 Window | ✅ | ❌ (使用) |
| 创建 ConfigManager | ✅ | ❌ (使用) |
| 创建 Logging | ✅ | ❌ (使用) |
| 创建 InputManager | ❌ | ✅ |
| 创建 PlayerManager | ❌ | ✅ |
| 创建 LevelManager | ❌ | ✅ |
| 持有消息循环 | ❌ | ✅ |
| 管理运行时状态 | ❌ | ✅ |

---

## 6. 设计原则

| 原则 | 说明 |
|:-----|:-----|
| **依赖注入** | 基础能力由外部注入，不自行创建 |
| **组合优先** | 通过组合而非继承构建复杂逻辑 |
| **单一入口** | Run() 是唯一的执行入口 |
| **明确生命周期** | 构造函数初始化，Shutdown() 清理 |
