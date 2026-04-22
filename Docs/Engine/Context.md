# Context (上下文层)

## 1. 概述

Context 是游戏引擎的**中间层**，作为 Bootstrap 和 Game 之间的**数据容器和接口契约**。

| 职责定位 | 说明 |
|:--------|:-----|
| **做什么** | 持有能力指针（Window、Renderer、Logging 等），定义接口契约 |
| **不做什么** | 不创建对象、不执行业务逻辑、不持有状态 |

**设计哲学**：Context 是"容器 + 接口"，它定义了 Game 需要使用哪些能力，但不关心这些能力如何创建。它解耦了"使用者"（Game）和"提供者"（Bootstrap）。

---

## 2. 核心概念

### 2.1 为什么需要 Context？

```
直接注入的问题：
Bootstrap ──→ Game
  ❌ Game 必须知道所有具体类型
  ❌ 每次新增能力都要修改 Game 构造函数
  ❌ 违反开闭原则

使用 Context 的优势：
Bootstrap ──→ Context ──→ Game
  ✅ Game 只需知道 Context 接口
  ✅ 新增能力只需扩展 Context
  ✅ 关注点分离
```

### 2.2 Context vs 具体能力

| 维度 | Context | 具体能力 (如 IRenderer) |
|:-----|:--------|:------------------------|
| **本质** | 容器（Container） | 服务接口（Service Interface） |
| **职责** | 持有指针、定义变量 | 定义"能做什么"（Draw、Clear） |
| **生命周期** | 创建由 Bootstrap 管理 | 创建由 Bootstrap 管理 |
| **使用方式** | 被 Game 持有并访问 | Game 通过 Context 间接使用 |

### 2.3 能力流动

```
┌─────────────┐
│  Bootstrap  │  ← 创建具体实现（DX12Renderer、FileSystem 等）
└──────┬──────┘
       │
       │ 填充能力
       ▼
┌─────────────┐
│   Context   │  ← 持有能力指针（仅接口类型）
│  (数据容器)   │
└──────┬──────┘
       │
       │ 注入
       ▼
┌─────────────┐
│    Game     │  ← 通过 Context 访问能力
└─────────────┘
```

---

## 3. Context 的设计

### 3.1 核心结构

```cpp
// Context 是一个纯数据容器，不包含任何业务逻辑
class GameContext {
public:
    // ── 基础子系统指针 ──
    IWindow*      Window      = nullptr;   // 窗口管理
    IRenderer*    Renderer    = nullptr;   // 渲染器
    IConfig*      Config      = nullptr;   // 配置管理
    ILogging*     Logging     = nullptr;   // 日志系统
    IFileSystem*  FileSystem  = nullptr;   // 文件系统

    // ── 游戏模块指针 ──
    IInputSystem*    InputSystem   = nullptr;
    IAudioSystem*    AudioSystem   = nullptr;
    IPhysicsSystem*  PhysicsSystem = nullptr;

    // ── 便捷访问方法 ──
    bool IsValid() const;
    void Release();  // 释放所有指针（但 Context 本身不负责销毁对象）
};

// 接口示例
class IRenderer {
public:
    virtual void Clear() = 0;
    virtual void Draw() = 0;
    virtual ~IRenderer() = default;
};
```

### 3.2 使用示例

```cpp
// Bootstrap 负责创建和填充 Context
class Bootstrap {
public:
    GameContext* CreateContext() {
        auto ctx = new GameContext();
        
        // 创建具体实现
        ctx->Window    = new DX12Window();
        ctx->Renderer  = new DX12Renderer();
        ctx->Logging    = new FileLogging();
        ctx->Config     = new JsonConfigManager();
        
        return ctx;
    }
};

// Game 只依赖 Context 接口
class Game {
private:
    GameContext* m_Context;  // 单一的注入点

public:
    Game(GameContext* ctx) : m_Context(ctx) {}

    void Render() {
        // 通过 Context 访问能力
        m_Context->Renderer->Clear();
        m_Context->Renderer->Draw();
    }
};
```

---

## 4. 架构图表

### 4.1 三层架构全景

```mermaid
graph TB
    subgraph "启动层 (Bootstrap)"
        B["Bootstrap"]
        B1["创建 Context"]
        B2["填充能力"]
        B3["异常处理"]
    end

    subgraph "中间层 (Context)"
        C["GameContext"]
        C1["Window*"]
        C2["Renderer*"]
        C3["Logging*"]
        C4["Config*"]
    end

    subgraph "运行层 (Game)"
        G["Game"]
        G1["Run()"]
        G2["Update()"]
        G3["Render()"]
        G4["Shutdown()"]
    end

    B -->|"创建"| C
    B -->|"填充"| C1
    B -->|"填充"| C2
    B -->|"填充"| C3
    B -->|"填充"| C4
    C -->|"注入"| G
    G -->|"使用"| C1
    G -->|"使用"| C2
    G -->|"使用"| C3
    G -->|"使用"| C4

    style B fill:#e8f5e9,stroke:#2e7d32
    style C fill:#fff3e0,stroke:#e65100
    style G fill:#e3f2fd,stroke:#1565c0
    style C1 fill:#fff,stroke:#ccc
    style C2 fill:#fff,stroke:#ccc
    style C3 fill:#fff,stroke:#ccc
    style C4 fill:#fff,stroke:#ccc
```

### 4.2 数据流时序图

```mermaid
sequenceDiagram
    participant main
    participant Bootstrap
    participant Context as GameContext
    participant Game

    main->>Bootstrap: Initialize()
    Activate Bootstrap

    Bootstrap->>Context: new GameContext()
    Note over Context: 创建空的上下文容器

    Bootstrap->>Context: Context->Window = new DX12Window()
    Bootstrap->>Context: Context->Renderer = new DX12Renderer()
    Bootstrap->>Context: Context->Logging = new FileLogging()
    Bootstrap->>Context: Context->Config = new JsonConfig()
    Note over Context: 填充所有能力

    Bootstrap->>Game: new Game(Context)
    Context-->>Game: Context 指针
    Deactivate Bootstrap

    Game->>Game: Run()
    loop Game Loop
        Game->>Context: Context->Window->ProcessMessages()
        Game->>Context: Context->Renderer->Clear()
        Game->>Context: Context->Renderer->Draw()
        Game->>Context: Context->Logging->Log("Frame rendered")
    end

    Game->>Game: Shutdown()
    Game-->>main: exitCode
```

### 4.3 依赖关系图

```mermaid
graph LR
    subgraph "启动层"
        Bootstrap["Bootstrap"]
    end

    subgraph "中间层"
        Context["GameContext"]
        subgraph "持有的指针"
            Window["Window*"]
            Renderer["Renderer*"]
            Logging["Logging*"]
            Config["Config*"]
        end
    end

    subgraph "运行层"
        Game["Game"]
        GameLoop["GameLoop"]
    end

    Bootstrap -->|"new"| Context
    Bootstrap -->|"填充"| Window
    Bootstrap -->|"填充"| Renderer
    Bootstrap -->|"填充"| Logging
    Bootstrap -->|"填充"| Config

    Context -->|"注入"| Game
    Window -->|"使用"| GameLoop
    Renderer -->|"使用"| GameLoop
    Game -->|"拥有"| GameLoop

    style Bootstrap fill:#e8f5e9,stroke:#2e7d32
    style Context fill:#fff3e0,stroke:#e65100
    style Game fill:#e3f2fd,stroke:#1565c0
```

### 4.4 扩展性示意

```mermaid
graph TB
    subgraph "基础 Context"
        C1["Window*"]
        C2["Renderer*"]
    end

    subgraph "扩展 Context"
        C3["Logging*"]
        C4["Config*"]
        C5["FileSystem*"]
    end

    subgraph "高级 Context"
        C6["InputSystem*"]
        C7["AudioSystem*"]
        C8["PhysicsSystem*"]
    end

    C1 & C2 --> C3 & C4 & C5
    C3 & C4 & C5 --> C6 & C7 & C8

    style C1 fill:#bbdefb,stroke:#1565c0
    style C2 fill:#bbdefb,stroke:#1565c0
    style C3 fill:#c8e6c9,stroke:#2e7d32
    style C4 fill:#c8e6c9,stroke:#2e7d32
    style C5 fill:#c8e6c9,stroke:#2e7d32
    style C6 fill:#ffe0b2,stroke:#e65100
    style C7 fill:#ffe0b2,stroke:#e65100
    style C8 fill:#ffe0b2,stroke:#e65100
```

---

## 5. Context 的设计原则

| 原则 | 说明 |
|:-----|:-----|
| **纯数据容器** | 只包含指针和 getter/setter，不包含业务逻辑 |
| **单一注入点** | Game 通过一个 Context 对象获取所有能力 |
| **接口契约** | Context 中只声明接口类型（IRenderer*），不声明具体类型（DX12Renderer*） |
| **所有权分离** | Context 持有指针但不负责销毁，由创建者（Bootstrap）负责生命周期 |
| **可扩展性** | 新增能力只需在 Context 中添加新指针，无需修改 Game |
| **可测试性** | 可以创建 MockContext 注入测试环境 |

---

## 6. 与其他模块的关系

### 6.1 职责边界

| 操作 | Bootstrap | Context | Game |
|:-----|:---------:|:-------:|:----:|
| 创建具体对象 | ✅ | ❌ | ❌ |
| 持有能力指针 | ❌ | ✅ | ❌ |
| 使用能力 | ❌ | ❌ | ✅ |
| 管理生命周期 | 创建 | 不负责 | 触发释放 |

### 6.2 类型可见性

```cpp
// Bootstrap (可见所有类型)
class Bootstrap {
    void CreateGame() {
        auto ctx = new GameContext();
        ctx->Renderer = new DX12Renderer();  // ✅ 可见具体类型
        ctx->Window   = new DX12Window();
    }
};

// Context (只可见接口)
class GameContext {
    IRenderer* Renderer;  // ✅ 接口类型
    IWindow*   Window;    // ✅ 接口类型
};

// Game (只可见 Context)
class Game {
    GameContext* m_Context;
    void Render() {
        m_Context->Renderer->Draw();  // ✅ 通过接口使用
    }
};
```

---

## 7. 未来扩展

随着引擎发展，Context 可逐步添加：

| 阶段 | 新增能力 | 说明 |
|:----:|:--------|:-----|
| Phase 1 | MemoryAllocator* | 内存管理器指针 |
| Phase 1 | FileSystem* | 文件系统指针 |
| Phase 2 | InputSystem* | 输入系统指针 |
| Phase 2 | AudioSystem* | 音频系统指针 |
| Phase 3 | PhysicsSystem* | 物理系统指针 |
| Phase 3 | AISystem* | AI 系统指针 |

所有新增能力遵循相同模式：在 Context 中添加接口指针 → Bootstrap 填充 → Game 使用。




## todo

计时器：基础设施服务
你说得对，计时器不应该属于 SceneManager。场景管理器管的是“空间”，计时器管的是“时间”。
定位： 它是 GameContext 的直接成员，或者是一个独立的 TimeService。
生命周期： 由 Bootstrap 创建，注入 GameContext。
多计时器设计： 虽然游戏逻辑主要用一个主时钟，但为了扩展性（比如暂停菜单时的UI动画时间、网络延迟统计时间），我们可以预留接口。
临时方案： GameContext 持有一个 MainTimer。
未来方案： GameContext 持有一个 TimeManager，里面管理着 GameTime, UITime, NetworkTime 等。


相机：场景/世界的一部分
相机本质上是一个“观察者”。它必须依附于某个空间（世界坐标系）。
定位： SceneManager 的成员。
理由： 相机有位置、旋转，它需要知道自己在世界哪里。它和灯光、模型一样，都是场景图的一部分。
多相机支持： 正如你提到的，未来会有“主摄像机”（透视投影）和“UI摄像机”（正交投影）。SceneManager 维护一个相机列表，并提供 GetActiveCamera() 接口，这是非常标准的做法。


1. 暂时没有场景管理器，我们先将计时器和相机组合到context作为单个成员来完成