# Game (游戏主逻辑)

## 1. 概述

Game 是游戏引擎的**运行层**，负责：

- 接收 Context 注入的基础能力
- 持有并运行主循环 (Game Loop)
- 组合游戏逻辑模块
- 管理整个游戏的生命周期

| 职责定位 | 说明 |
|:--------|:-----|
| **做什么** | 运行主循环、组合游戏逻辑、管理 Update/Render |
| **不做什么** | 不创建基础设施（由 Bootstrap 创建并填充到 Context） |

**设计哲学**：Game 是"运行者 + 组合者"，通过 Context 获取基础能力，并将游戏逻辑模块组合成完整的游戏体验。

---

## 2. 核心职责

### 2.1 接收 Context

Game 不创建基础子系统，而是接收注入的 GameContext：

| 能力 | 来源 (Context) | 用途 |
|:-----|:--------------|:-----|
| Window | Context->Window | 窗口消息处理、渲染目标 |
| Renderer | Context->Renderer | 渲染画面 |
| Config | Context->Config | 读取游戏配置 |
| Logging | Context->Logging | 记录游戏日志 |

### 2.2 运行主循环

```cpp
void Game::Run() {
    while (!m_Context->Window->ShouldClose()) {
        m_Context->Window->ProcessMessages();  // 消息循环
        Update();                       // 更新逻辑
        Render();                       // 渲染画面
    }
    Shutdown();
}
```

### 2.3 组合游戏逻辑模块

Game 负责组合和协调各个游戏逻辑子系统：

| 模块 | 职责 | Game 的角色 |
|:-----|:-----|:------------|
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
        subgraph "注入的 Context"
            C["GameContext"]
            C1["Window*"]
            C2["Renderer*"]
            C3["Logging*"]
            C4["Config*"]
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

    C1 & C2 & C3 & C4 -->|"使用"| UP
    C1 -->|"消息"| RL
    P & LV & A & IN & AU -->|"组合"| UP
    P & LV & A & IN & AU -->|"组合"| RE

    RL --> UP
    RL --> RE
    RL --> SD

    style C fill:#fff3e0,stroke:#e65100
    style C1 fill:#fff,stroke:#ccc
    style C2 fill:#fff,stroke:#ccc
    style C3 fill:#fff,stroke:#ccc
    style C4 fill:#fff,stroke:#ccc
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
    participant Context as GameContext
    participant Game
    participant Window
    participant InputMgr as InputManager
    participant Logic
    participant Renderer
    participant AudioMgr as AudioManager

    Bootstrap->>Context: new GameContext()
    Bootstrap->>Context: 填充能力
    Bootstrap->>Game: new Game(Context)
    Note over Game: 初始化游戏逻辑模块

    Game->>Game: Run()
    loop 每帧
        Game->>Context: Context->Window->ProcessMessages()
        Context->>Window: ProcessMessages()
        Window-->>Context: 消息处理完成
        Context-->>Game: 完成

        Game->>InputMgr: ProcessInput()
        InputMgr-->>Game: 输入状态更新

        Game->>Logic: Update(deltaTime)
        Logic->>AudioMgr: Update()
        Logic-->>Game: 逻辑更新完成

        Game->>Context: Context->Renderer->Clear()
        Context->>Renderer: Clear()
        Game->>Context: Context->Renderer->Draw()
        Context->>Renderer: Draw()
        Renderer-->>Context: 渲染完成
        Context-->>Game: 完成
    end

    Game->>Game: Shutdown()
    Note over Game: 清理游戏资源
    Game-->>Bootstrap: exitCode
```

### 3.3 生命周期流程

```mermaid
stateDiagram-v2
    [*] --> Bootstrap: Bootstrap 创建

    Bootstrap --> 创建Context: new GameContext()
    创建Context --> 填充能力: 填充 Window/Renderer/Logging/Config

    填充能力 --> 创建Game: new Game(Context)
    创建Game --> Initialized: 构造函数

    Initialized --> Running: Run()
    Running --> Running: Update() / Render()
    Running --> Shutdown: ShouldClose() == true
    Shutdown --> [*]: return exitCode

    state Bootstrap {
        [*] --> 创建 ConfigManager
        创建 ConfigManager --> 创建 Logging
        创建 Logging --> 创建 Window
        创建 Window --> 创建 Renderer
        创建 Renderer --> 填充到 Context
    }

    state Game {
        [*] --> 初始化 Managers
        初始化 Managers --> 消息循环
        消息循环 --> Update
        Update --> Render
        Render --> 消息循环: 继续
    }
```

### 3.4 依赖关系（通过 Context）

```mermaid
graph LR
    subgraph "Bootstrap 创建"
        BM["Bootstrap"]
    end

    subgraph "Context (持有接口)"
        CTX["GameContext"]
        CTX1["Window*"]
        CTX2["Renderer*"]
        CTX3["Logging*"]
    end

    subgraph "Game 组合"
        G["Game"]
        IM["InputManager"]
        PM["PlayerManager"]
        LM["LevelManager"]
    end

    BM -->|"创建"| CTX1
    BM -->|"创建"| CTX2
    BM -->|"创建"| CTX3
    BM -->|"填充"| CTX
    CTX1 & CTX2 & CTX3 -->|"持有"| CTX

    CTX -->|"注入"| G

    G -->|"组合"| IM
    G -->|"组合"| PM
    G -->|"组合"| LM

    G -->|"使用"| CTX1
    G -->|"使用"| CTX2
    G -->|"使用"| CTX3

    style BM fill:#e8f5e9,stroke:#2e7d32
    style CTX fill:#fff3e0,stroke:#e65100
    style G fill:#e3f2fd,stroke:#1565c0
```

### 3.5 完整架构图

```mermaid
graph TB
    subgraph "启动层"
        B["Bootstrap"]
    end

    subgraph "中间层"
        C["GameContext"]
        subgraph "基础能力"
            W["Window*"]
            R["Renderer*"]
            L["Logging*"]
            CF["Config*"]
        end
    end

    subgraph "基础子系统实现"
        W_impl["DX12Window"]
        R_impl["DX12Renderer"]
        L_impl["FileLogging"]
        CF_impl["JsonConfig"]
    end

    subgraph "运行层"
        G["Game"]
        subgraph "游戏模块"
            IM["InputManager"]
            PM["PlayerManager"]
            LM["LevelManager"]
            AM["AssetManager"]
            AU["AudioManager"]
        end
        GL["GameLoop"]
    end

    %% Bootstrap 创建具体实现
    B -->|"new"| W_impl
    B -->|"new"| R_impl
    B -->|"new"| L_impl
    B -->|"new"| CF_impl

    %% 具体实现填充到 Context
    W_impl -->|"填充"| W
    R_impl -->|"填充"| R
    L_impl -->|"填充"| L
    CF_impl -->|"填充"| CF

    W & R & L & CF -->|"持有"| C

    %% Context 注入 Game
    C -->|"注入"| G

    %% Game 组合模块
    G -->|"组合"| IM
    G -->|"组合"| PM
    G -->|"组合"| LM
    G -->|"组合"| AM
    G -->|"组合"| AU

    %% Game 使用 Context 能力
    G -->|"Run()"| GL
    GL -->|"使用"| W
    GL -->|"使用"| R
    GL -->|"使用"| L

    %% 模块使用 Context
    IM & PM & LM & AM & AU -->|"使用"| R

    style B fill:#e8f5e9,stroke:#2e7d32
    style C fill:#fff3e0,stroke:#e65100
    style G fill:#e3f2fd,stroke:#1565c0
    style GL fill:#e3f2fd,stroke:#1565c0
```

---

## 4. 代码示例

### 4.1 基础结构

```cpp
class Game {
private:
    // ── 注入的 Context (单一注入点) ──
    GameContext* m_Context;

    // ── 游戏逻辑模块 (组合) ──
    std::unique_ptr<InputManager>    m_InputManager;
    std::unique_ptr<PlayerManager>    m_PlayerManager;
    std::unique_ptr<LevelManager>     m_LevelManager;
    std::unique_ptr<AssetManager>     m_AssetManager;
    std::unique_ptr<AudioManager>     m_AudioManager;

    // ── 运行状态 ──
    bool m_IsRunning;

public:
    // 构造函数接收 Context
    Game(GameContext* context)
        : m_Context(context)
        , m_IsRunning(true) {
        InitializeModules();
    }

    // 主循环
    int Run() {
        while (m_Context->Window->ShouldClose() == false && m_IsRunning) {
            m_Context->Window->ProcessMessages();
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

    // 通过 Context 访问基础能力
    m_Context->Logging->Log("Updating frame...");

    // 更新基础能力
    m_InputManager->Update();

    // 组合游戏逻辑
    m_PlayerManager->Update(deltaTime);
    m_LevelManager->Update(deltaTime);
    m_AudioManager->Update(deltaTime);
}

void Game::Render() {
    // 使用 Context 中的 Window 和 Renderer
    m_Context->Window->BeginFrame();
    m_Context->Renderer->Clear();

    // 渲染各个逻辑模块
    m_LevelManager->Render();
    m_PlayerManager->Render();

    m_Context->Renderer->Present();
    m_Context->Window->EndFrame();
}
```

### 4.3 模块注册到 Context

```cpp
void Game::InitializeModules() {
    // 通过 Config 读取配置
    auto& config = m_Context->Config->GetSection("Game");

    // 创建并初始化游戏逻辑模块
    m_InputManager = std::make_unique<InputManager>(m_Context);
    m_AssetManager = std::make_unique<AssetManager>(
        m_Context->FileSystem,
        m_Context->Logging
    );
    m_LevelManager = std::make_unique<LevelManager>(
        m_AssetManager.get(),
        m_Context->Logging
    );
    m_PlayerManager = std::make_unique<PlayerManager>(
        m_InputManager.get(),
        config.GetInt("MaxPlayers")
    );
    m_AudioManager = std::make_unique<AudioManager>(
        m_Context->FileSystem
    );

    // 通过 Logging 记录初始化
    m_Context->Logging->Log("All game modules initialized");
}
```

---

## 5. 职责边界总结

| 能力/模块 | Bootstrap | Context | Game |
|:----------|:---------:|:-------:|:----:|
| 创建 Window | ✅ | ❌ (持有指针) | ❌ (使用) |
| 创建 Renderer | ✅ | ❌ (持有指针) | ❌ (使用) |
| 创建 Config | ✅ | ❌ (持有指针) | ❌ (使用) |
| 创建 Logging | ✅ | ❌ (持有指针) | ❌ (使用) |
| 持有能力指针 | ❌ | ✅ | ❌ |
| 创建 InputManager | ❌ | ❌ | ✅ |
| 创建 PlayerManager | ❌ | ❌ | ✅ |
| 创建 LevelManager | ❌ | ❌ | ✅ |
| 持有消息循环 | ❌ | ❌ | ✅ |
| 管理运行时状态 | ❌ | ❌ | ✅ |

---

## 6. 设计原则

| 原则 | 说明 |
|:-----|:-----|
| **依赖注入** | 基础能力通过 Context 注入，不自行创建 |
| **单一注入点** | 所有基础能力通过一个 Context 对象获取 |
| **组合优先** | 通过组合而非继承构建复杂逻辑 |
| **明确生命周期** | 构造函数初始化，Shutdown() 清理 |
| **接口编程** | 通过 Context 访问接口，不直接依赖实现 |
