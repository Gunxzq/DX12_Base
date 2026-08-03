# 场景管理器（SceneManager）架构

> 日期：2026-07-17
> 状态：📋 新设计
> 关联：`SceneFileAndLoading.md`、`SceneConstructor`、`GameWorld`、`EditorScene`
> 后续：`ECS/SceneModule.md`（各子模块详细设计）

---

## 1. 概述

### 1.1 问题背景

当前场景管理存在以下问题：

| 问题 | 表现 | 后果 |
|:-----|:-----|:------|
| **无中心权威** | `SceneConstructor` 被 Editor 和 Game 各自持有一个实例，`EditorScene` 和 `GameWorld` 各自管理自己的场景加载 | 场景状态分散，无法统一查询 |
| **场景生命周期缺失** | 场景只加载一次，从不卸载；无切换、无过渡、无叠加 | 无法支持运行时场景切换 |
| **消费者各自为政** | Asset Browser、Outliner、ViewPort 各自独立获取场景信息 | 数据冗余，接口不统一 |
| **场景资源泄漏** | 场景卸载时没有集中释放 GPU 资源的机制 | 场景切换必然泄漏（已记录于 `SceneFileAndLoading.md §6.3`） |

### 1.2 核心设计目标

1. **SceneManager 是运行时场景的中心权威** — 持有当前场景的实体清单、环境状态、生命周期状态
2. **Scene 是运行时的活体状态** — 不是 JSON 文件，也不是中间产物 `SceneDescription`，而是当前世界可见的全部内容（ECS 实体 + 环境 + 光照 + 天空盒 ...）
3. **加载器 ≠ SceneManager** — `SceneConstructor`、`CharacterLoader`、`StreamingLoader` 都是加载器，它们通过 SceneManager 的接口向场景填充内容
4. **Game 端和 Editor 端共享核心，各自特化** — Engine CORE 提供 `SceneManager` 基类，Game 端和 Editor 端各自继承扩展
5. **子模块化** — 场景管理器内部按领域拆分为 `PhysicsScene`、`AudioScene`、`RenderScene` 等子模块，各自管理场景在该领域的数据

### 1.3 关键概念澄清

```
场景文件（.scene.json）    SceneDescription（中间数据）    运行时场景（Runtime Scene）
      │                           │                              │
  SceneLoader               SceneConstructor               SceneManager
  文件→内存的解析器          资产加载→ECS 构造的编排器        运行时场景的中心管理器
```

| 概念 | 本质 | 生命周期 | 消费者 |
|:-----|:-----|:---------|:-------|
| **场景文件** | 磁盘上的 JSON 或二进制文件 | 持久化存储 | 编辑器读写、发布包打包 |
| **SceneDescription** | 内存中的反序列化结构体 | 解析后存在，加载完成后可丢弃 | Game 端不需要，Editor 端用于序列化导出 |
| **运行时场景** | ECS 实体 + 环境状态 + 子场景数据 | 从加载到卸载，贯穿运行期 | 所有子系统：渲染、物理、音频、编辑器 UI |

---

## 2. SceneManager 核心设计

### 2.1 位置与归属

```
Engine CORE                          Game 端                          Editor 端
┌──────────────────────────────┐     ┌──────────────────────┐     ┌──────────────────────────┐
│      SceneManager            │     │   GameSceneManager    │     │    EditorSceneManager     │
│  （基类，通用能力）            │ ←── │  （游戏运行时特化）    │     │  （编辑器编辑/序列化特化） │
│                              │     │                      │     │                          │
│  ┌────────────────────────┐  │     │  • 关卡加载/切换      │     │  • SceneDescription 导出  │
│  │  ECS（内部实现，不暴露） │  │     │  • 流式加载调度       │     │  • 实体编辑(Undo/Redo)    │
│  │  enTT Registry          │  │     │  • 游戏规则绑定       │     │  • 场景文件管理           │
│  │  view/group/ctx 全保留  │  │     │  • DontDestroyOnLoad  │     │  • 多场景编辑Tab          │
│  └────────────────────────┘  │     │  • 过渡动画           │     │  • 差异更新(Merge)        │
│                              │     │                      │     │                          │
│  • 实体 CRUD（公开 API）      │     └──────────────────────┘     └──────────────────────────┘
│  • 环境状态管理               │            │                            │
│  • RenderScene 渲染上下文      │            └──────────┬─────────────────┘
│  • 生命周期事件（MessageDispatcher）│                  │
│  • 待处理队列                  │            ┌──────────────────────┐
└──────────────────────────────┘            │   RenderScene         │
                                            │  （渲染上下文容器）     │
                                            │  • LightManager*       │
                                            │  • ReflectionProbeMgr* │
                                            │  • AmbientOcclusionMgr*│
                                            │  • DescriptorHeaps*    │
                                            │  • DeviceContext*      │
                                            └──────────────────────┘
```

### 2.2 核心接口

```cpp
// Engine/Scene/SceneManager.h — Engine CORE

namespace DX12Engine::Scene {

/// 场景生命周期状态
enum class SceneState {
    None,        // 无场景
    Loading,     // 正在异步加载
    Active,      // 实体已构造，完全运行中
    Unloading,   // 正在卸载
};

/// 场景切换模式
enum class SceneTransition {
    Immediate,   // 立即切换（旧场景卸载 → 新场景加载）
    Additive,    // 叠加（不卸载旧场景，新实体追加到世界）
    // 后续可扩展：FadeOutIn, LoadingScreen 等
};

/// 实体变更类型（事件数据）
enum class EntityChangeType {
    Added,
    Removed,
    ComponentChanged,
    TransformChanged,
};

/// 实体变更事件（通过 MessageDispatcher 异步分发）
struct EntityChangeEvent {
    EntityChangeType type;
    uint64_t entity;  // EntityHandle（不暴露 ECS 内部类型）
};

/// 场景生命周期事件（通过 MessageDispatcher 异步分发）
struct SceneLifecycleEvent {
    SceneState oldState;
    SceneState newState;
    std::string sceneName;  // 当前场景名称
};

/// 场景管理器基类
///
/// 设计原则：
///   - ECS Registry 是内部实现，不对外暴露
///   - 外部通过 EntityHandle（uint64_t）操作实体，不接触 entt::entity
///   - 内部 System 通过 GetRegistryForInternalUse() 访问完整的 ECS 能力
///   - 实体创建/销毁强制走 SceneManager 接口，确保生命周期管理
///   - 实体变更/场景生命周期事件通过 MessageDispatcher 异步分发，不再维护同步回调
class SceneManager {
public:
    SceneManager() = default;
    virtual ~SceneManager() = default;
    
    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;
    
    // ====================================================================
    // 初始化/销毁
    // ====================================================================
    
    /// 初始化（ECS Registry 由 SceneManager 内部创建；同时创建 RenderScene 渲染上下文）
    virtual void Initialize();
    
    /// 销毁，清理所有实体
    virtual void Shutdown();
    
    // ====================================================================
    // 实体管理（核心）
    //
    // 设计原则：生成器（SceneConstructor、CharacterLoader 等）负责组件创建，
    // SceneManager 只负责管理（生命周期追踪、持久化）。
    // 生成器是 Scene System 的一部分，通过 GetRegistryForInternalUse() 访问 ECS 完整能力。
    // ====================================================================
    
    /// 创建空实体（分配 entt::entity，返回 uint64_t handle）
    /// 生成器调用此方法获取实体 ID，然后通过 GetRegistryForInternalUse() 添加组件
    uint64_t CreateEntity();
    
    /// 批量创建空实体
    std::vector<uint64_t> CreateEntities(uint32_t count);
    
    /// 将已创建的实体注册到 SceneManager 的管理中
    /// 调用前，生成器应已完成组件的创建
    void RegisterEntity(uint64_t entity);
    void RegisterEntities(std::span<const uint64_t> entities);
    
    /// 移除实体（内部自动处理 Retain/Release）
    void RemoveEntity(uint64_t entity);
    void RemoveEntities(std::span<const uint64_t> entities);
    
    /// 移除所有实体（场景全量切换时使用）
    void RemoveAllEntities();
    
    /// 获取当前场景的所有实体 Handle 列表（扁平，无父子层级）
    const std::vector<uint64_t>& GetAllEntities() const;
    
    /// 获取实体组件（受控的组件访问）
    template<typename T>
    T* GetComponent(uint64_t entity) {
        return m_registry->TryGet<T>(static_cast<entt::entity>(entity));
    }
    
    // ====================================================================
    // 实体持久化（跨场景）
    // ====================================================================
    
    /// 标记实体在场景切换时保留（DontDestroyOnLoad 模式）
    void PersistEntity(uint64_t entity);
    
    /// 取消实体的持久化标记
    void UnpersistEntity(uint64_t entity);
    
    /// 检查实体是否被标记为持久化
    bool IsPersistent(uint64_t entity) const;
    
    // ====================================================================
    // 环境状态
    // ====================================================================
    
    void SetSkybox(const Resource::SkyboxDesc& skybox);
    void SetEnvironment(const Resource::EnvironmentDesc& env);
    
    const Resource::SkyboxDesc& GetSkybox() const;
    const Resource::EnvironmentDesc& GetEnvironment() const;
    
    // ====================================================================
    // 场景生命周期
    // ====================================================================
    
    SceneState GetState() const { return m_state; }
    const std::string& GetCurrentSceneName() const { return m_sceneName; }
    
    /// 准备切换场景
    /// 内部处理：保留 Persist 实体 → 清除其余实体 → 更新状态
    void PrepareSceneSwitch(const std::string& newSceneName, SceneTransition transition);
    
    // ====================================================================
    // RenderScene 渲染上下文
    // ====================================================================
    
    /// 获取 RenderScene 渲染上下文容器
    RenderScene* GetRenderScene() const { return m_renderScene.get(); }
    
    /// 获取内部 Registry 指针（供 Builder 系统获取 ECS 上下文）
    ECS::Registry* GetRegistry() const { return m_registry.get(); }
    
    // ====================================================================
    // 待处理队列处理（主线程每帧调用）
    // ====================================================================
    
    /// 处理所有待处理的变更（主线程统一入口）
    /// 在 BackgroundExecutor::Tick() 之后、ECS System 执行之前调用
    void ProcessPendingChanges();

protected:
    // ====================================================================
    // 内部 System 访问（不对外暴露）
    // ====================================================================
    
    /// 内部 System 通过此方法访问完整的 ECS 能力
    /// 外部消费者（Editor、GameWorld、AssetBrowser）禁止调用
    ECS::Registry* GetRegistryForInternalUse() { return m_registry.get(); }
    
    // 子类可访问的状态
    SceneState m_state = SceneState::None;
    std::string m_sceneName;
    
    // ECS 是内部实现，不对外暴露
    // 内部 System 通过 GetRegistryForInternalUse() 访问
    std::unique_ptr<ECS::Registry> m_registry;
    
    // 实体清单（扁平列表，无父子层级）
    // 使用 uint64_t 而非 ECS::Entity，避免外部依赖 entt 类型
    std::vector<uint64_t> m_entities;
    
    // 持久化实体集合
    std::unordered_set<uint64_t> m_persistentEntities;
    
    // 环境状态
    Resource::SkyboxDesc m_skybox;
    Resource::EnvironmentDesc m_environment;
    
    // 渲染上下文容器（直接持有）
    std::unique_ptr<RenderScene> m_renderScene;
    
    // 待处理队列（线程安全，后台加载器写入，主线程 ProcessPendingChanges 消费）
    ConcurrentQueue<PendingEntityChange> m_pendingChanges;
    
    // 内部方法：EntityHandle ←→ entt::entity 转换
    entt::entity ToInternal(uint64_t handle) const;
    uint64_t ToExternal(entt::entity e) const;
};

} // namespace DX12Engine::Scene
```

### 2.3 实体创建流程（两步走）

生成器（SceneConstructor / CharacterLoader 等）负责组件创建，SceneManager 只负责管理。

```
Step 1: 生成器创建实体 + 组件
    │
    ├─ SceneManager::CreateEntity()          ← 分配 entt::entity，返回 uint64_t handle
    ├─ 生成器通过 GetRegistryForInternalUse() 获取 Registry
    │     ├─ registry->emplace<TransformComponent>(entity, ...)
    │     ├─ registry->emplace<MeshComponent>(entity, ...)
    │     ├─ registry->emplace<LightComponent>(entity, ...)
    │     └─ ... 生成器按需自由组合组件
    │
    ▼
Step 2: 注册到 SceneManager
    │
    ├─ SceneManager::RegisterEntity(handle)
    │     ├─ 追加到扁平实体列表（m_entities）
    │     └─ SceneManager 持有了实体 Handle 的引用
```

为什么分离两步：

| 原因 | 说明 |
|:-----|:------|
| **管理器不耦合组件类型** | SceneManager 不知道 TransformComponent、MeshComponent 等类型，组件创建完全由生成器负责 |
| **生成器自由组合组件** | 不同生成器可以创建不同的组件组合，SceneManager 不需要了解 |
| **统一入口保证** | `CreateEntity` + `RegisterEntity` 配对，确保实体生命周期受 SceneManager 控制 |
| **ECS 内部 System 不受影响** | 内部 System 仍通过 `GetRegistryForInternalUse()` 访问完整 ECS 能力 |

### 2.4 场景切换流程

> ⚠️ **当前设计决策**：编辑器端不再支持"切换即销毁"模式。
> 场景切换应通过多 Tab 机制实现（见 §10），旧场景的 ECS Registry 和 GPU 资源保持驻留，
> 切换 Tab 只切换活跃指针。资源释放仅在 Tab 关闭时执行。
> 下方 `PrepareSceneSwitch` 保留为 Game 端使用（关卡切换），
> Editor 端通过 `EditorSceneManager::SwitchScene` 内部调用。

```
PrepareSceneSwitch("新场景", Immediate)
    │
    ├─ 遍历所有实体（m_entities）
    │     ├─ 如果 IsPersistent(entity) → 跳过，保留
    │     └─ 否则 → 移除实体
    │           └─ SceneManager 内部：m_registry->DestroyEntity()
    │
    ├─ 重建 m_entities 列表（仅保留 persistent 实体）
    ├─ m_sceneName = "新场景"
    └─ m_state = Active
```

### 2.5 待处理队列（ProcessPendingChanges）

```
后台加载器（BackgroundExecutor 线程）
    │
    ├─ LoadTask 完成
    │     └─ 回调中往 SceneManager::m_pendingChanges 写入：
    │           PendingEntityChange{ type: Add, entityDesc: EntityDesc }
    │
    ▼
主线程每帧：
    BackgroundExecutor::Tick()  ← 触发回调，写入待处理队列
        │
        ▼
    SceneManager::ProcessPendingChanges()
        │
        ├─ 消费队列中的所有 PendingEntityChange
        ├─ 对每个 Add：
        │     ├─ CreateEntity()                          ← 分配实体 ID
        │     ├─ 生成器通过 Registry 创建组件（已在回调中完成）
        │     └─ RegisterEntity(handle)                  ← 纳入管理
        ├─ 对每个 Remove：调用 RemoveEntity
        └─ 队列清空

**待处理队列与事件分发的关系**：

ProcessPendingChanges 只负责实体创建/销毁的异步操作调度（线程安全队列消费），
实体变更后的通知（EntityChangeEvent、SceneLifecycleEvent）通过 MessageDispatcher 事件系统分发，
由 FrameDriver 在统一的事件处理阶段派发到各消费者（RenderScene、Outliner 等）。

**为什么走 ProcessPendingChanges + MessageDispatcher 而非直接回调**：

| 方式 | 问题 |
|:-----|:------|
| 直接在后台线程回调中 AddEntity | 后台线程 ≠ 主线程，ECS Registry 非线程安全 |
| 在回调中直接 ECS 操作 | 没有生命周期控制，无法确保 RegisterEntity 配对 |
| **ProcessPendingChanges 统一消费 + 事件事后分发** | 主线程同一位置批量处理，延迟确定；实体变更通过 MessageDispatcher 解耦，不增加 SceneManager 的耦合度 |

---

### 2.6 ECS 是内部实现，不对外暴露

#### 设计原则

`ECS::Registry` 是 `SceneManager` 的**私有内部成员**，不通过任何公开 API 暴露给外部消费者（Editor、GameWorld、AssetBrowser 等）。

```
外部消费者（Editor / GameWorld / AssetBrowser）
    │
    ├─ SceneManager::CreateEntity()              ← 分配实体 ID
    ├─ (生成器通过 Registry 添加组件)
    ├─ SceneManager::RegisterEntity(handle)       ← 纳入管理
    ├─ SceneManager::RemoveEntity(handle)         ← 自动处理 Retain/Release
    ├─ SceneManager::GetAllEntities()             ← 只读查询
    ├─ SceneManager::GetComponent<T>(handle)      ← 受控的组件访问
    └─ ❌ 不能直接访问 ECS::Registry
    
内部 System
    │
    └─ SceneManager::GetRegistryForInternalUse()  ← 完整 enTT 能力
         ├─ auto view = reg->view<Transform, Mesh>();
         ├─ auto group = reg->group<Light>(entt::get<Transform>());
         └─ auto& ctx = reg->ctx().emplace<TimeOfDay>();
```

#### 动机

| 动机 | 说明 |
|:-----|:------|
| **生命周期强制** | 实体创建/销毁是唯一入口，Retain/Release 强制执行，不会遗漏 |
| **ECS 可替换** | 可以从 enTT 迁移到自定义实体存储，不影响上层代码 |
| **接口稳定** | 上层代码只依赖 `SceneManager`，不依赖 enTT 的类型系统 |
| **跨平台/跨架构** | 未来需要网络同步、回放、确定性模拟时，可在 SceneManager 层统一拦截 |

#### 内部 System 不受影响

内部 System 通过 `GetRegistryForInternalUse()` 获得完整的 ECS 能力：

```cpp
// 内部 System（如 AnimationAdvancer），通过 SceneManager 传入 Registry
// 该 Registry 指针来自 SceneManager::GetRegistryForInternalUse()
void AnimationAdvancer::Update(ECS::Registry* reg) {
    // 所有 enTT 高级特性正常使用
    auto view = reg->view<AnimComponent, BoneBuffer>();
    for (auto [entity, anim, bone] : view.each()) {
        // ...
    }
}
```

**包装层不限制查询能力，只控制变更入口**。enTT 的 view/group/ctx 等高级特性在内部系统中完全保留。

---

## 3. RenderScene 渲染上下文容器

### 3.1 设计动机

`RenderScene` 不是"子场景模块"，也不是通过 `ISceneModule` 接口注册的。它是 `SceneManager` 的**直接成员**，作用是将渲染相关的管理器引用和共享基础设施指针聚合到一个上下文中，避免各消费者各自持有分散的指针。

```
RenderScene = 渲染上下文 Scope

本质：
  RenderScene 是管理器的引用聚合层，不是管理层
  各管理器保持单例不变（LightManager、ReflectionProbeManager 等）
  RenderScene 只负责将它们聚合到一个渲染上下文中
```

**关键原则：**

| 原则 | 说明 |
|:-----|:------|
| **管理器保持单例** | LightManager、ReflectionProbeManager 等仍是全局单例，不按场景实例化 |
| **RenderScene 是引用聚合** | 持有管理器单例的引用（非 ownership），场景切换时驱动差异化更新（Clear + Rebuild） |
| **共享基础设施下沉** | 描述符堆集合、设备上下文等频繁传递的指针由 RenderScene 统一持有，消费者不再各自保存 |
| **场景切换 = 差异化更新** | 切换时调用 `LightManager::Clear()` 清除旧场景数据，新场景加载后增量注册 |

```
SceneManager
    │
    └── RenderScene（渲染上下文 Scope）
          ├── LightManager*              ← 引用单例，非 ownership
          ├── ReflectionProbeManager*    ← 同上
          ├── AmbientOcclusionManager*   ← 同上
          ├── DescriptorHeaps*           ← 共享基础设施指针
          ├── DeviceContext*             ← 同上
          └── 不负责渲染 Pass（渲染 Pass 由 Renderer 负责）
```

### 3.2 容器模式：创建与访问

RenderScene 由 `SceneManager::Initialize()` 内部创建为直接成员，无需各端手动注册。消费者通过 `GetRenderScene()` 获取 Scope：

```cpp
// SceneManager::Initialize() 内部自动创建
// Bootstrap::CreateContext() 中配置指针
auto *rs = m_sceneManager.GetRenderScene();
rs->SetLightManager(&LightManager::GetInstance());
rs->SetDescriptorHeaps(context->DescriptorHeaps);
rs->SetDeviceContext(context->DeviceContext);

// 消费方通过 SceneManager 获取 Scope
auto *rs = sceneMgr->GetRenderScene();
rs->GetLightManager()->UpdateAndUpload(fence, camera);
rs->GetDescriptorHeaps()->Allocate(...);
```

**与直接使用单例相比，这种模式的好处：**

| 对比 | 直接 `LightManager::GetInstance()` | 通过 RenderScene 访问 |
|:-----|:-----------------------------------|:----------------------|
| 依赖可见性 | 全局可见，任何地方都能调用 | 限制在场景上下文内 |
| 基础设施传递 | 每个消费者各自保存 DescriptorHeaps 指针 | RenderScene 统一持有，一处注入 |
| 场景切换 | 手动调用 Clear() | RenderScene 通过事件监听驱动 |
| 测试性 | 难以替换 | 可注入 Mock 管理器 |

### 3.3 事件驱动的通知机制

实体变更和场景生命周期通知不再走同步回调（ISceneModule），而是通过 `MessageDispatcher` 事件系统异步分发：

```
SceneManager 实体变更或场景切换
    │
    ├─ SceneManager 完成实体创建/移除/场景切换
    ├─ 通过 MessageDispatcher PostEvent 分发事件
    │     ├─ EntityChangeEvent（实体添加/移除/变更）
    │     └─ SceneLifecycleEvent（场景切换开始/完成）
    │
    ▼
FrameDriver 统一事件处理阶段
    │
    ├─ 按注册顺序依次派发事件
    │     ├─ RenderScene::OnEntityAdded   → 创建渲染代理
    │     ├─ RenderScene::OnScenePreUnload → LightManager::Clear()
    │     ├─ Outliner::OnEntityChange      → 更新 UI 列表
    │     └─ ...
    │
    └─ 所有消费者在同一帧内收到通知，状态一致
```

**为什么用事件代替 ISceneModule 同步回调：**

| 考量 | ISceneModule 同步回调 | MessageDispatcher 事件 |
|:-----|:----------------------|:----------------------|
| **耦合度** | SceneManager 持有 m_modules 列表，直接调用虚函数 | SceneManager 只 PostEvent，不知道谁在消费 |
| **线程安全** | 回调在调用者线程执行，必须确保主线程调用 | 事件入队，主线程统一消费，天然安全 |
| **机制重复** | SceneManager 自建一套回调体系，与 MessageDispatcher 并存 | 复用已有事件系统，无重复 |
| **帧控制** | 回调随时触发，FrameDriver 无法控制 | 事件在 FrameDriver 调度的时间点处理，确定性强 |
| **适用场景** | 实体增删改都是低频操作（场景加载/卸载/编辑器编辑），高频操作走 System | 与实际需求一致 |

**核心观察**：实体/组件的增删改是低频操作（场景加载时触发、编辑器编辑时触发），真正高频的 Transform 变化由 ECS System 处理，不经过事件系统。因此事件驱动的延迟完全可以接受，且带来了更好的解耦和线程安全特性。

---

## 4. GameSceneManager — Game 端特化

---

## 4. GameSceneManager — Game 端特化

### 4.1 位置

```
Game/Game/Scene/GameSceneManager.h
Game/Game/Scene/GameSceneManager.cpp
```

### 4.2 设计模式：组合包装

`GameSceneManager` **不继承** `SceneManager`，而是**组合包装** `SceneManager*`。与 `EditorSceneManager` 采用相同模式。

```
GameSceneManager
  ├─ SceneManager* m_sceneMgr  ← 被包装的 Bootstrap SceneManager
  ├─ 包装方法：CreateEntity() / RegisterEntity() / GetRegistry()
  ├─ RegisterSceneConstructSystem()  ← 注册场景构造系统
  └─ LoadSceneAsync()               ← 异步场景加载编排
```

### 4.3 特化职责

| 职责 | 说明 |
|:-----|:------|
| **注册 SceneConstructSystem** | 响应 `GeneratorTaskCompleteEvent`，通过 `CreateEntity` + `RegisterEntity` 构造实体 |
| **异步场景加载** | 编排 `SceneLoader::LoadFromFile` → `SceneConstructor::LoadScene` |
| **关卡加载/切换**（后续） | 从资源包加载关卡，驱动 SceneManager 的场景切换流程 |
| **流式加载调度**（后续） | 根据玩家位置动态加载/卸载区域 |

### 4.4 接口示例

```cpp
// Game/Game/Scene/GameSceneManager.h

class GameSceneManager {
public:
    void Initialize(DX12Engine::Scene::SceneManager* sceneMgr,
                    DX12Engine::Boot::GameContext* context);

    // 包装方法
    DX12Engine::Scene::SceneManager* GetSceneManager() const { return m_sceneMgr; }
    uint64_t CreateEntity() { return m_sceneMgr ? m_sceneMgr->CreateEntity() : UINT64_MAX; }
    void RegisterEntity(uint64_t entity) { if (m_sceneMgr) m_sceneMgr->RegisterEntity(entity); }
    DX12Engine::ECS::Registry* GetRegistry() const { return m_sceneMgr ? m_sceneMgr->GetRegistry() : nullptr; }

    // 场景构造系统注册
    void RegisterSceneConstructSystem();

    // 异步场景加载
    void LoadSceneAsync(const std::string& filePath);
    bool IsLoading() const;

private:
    void OnSceneConstructReady(const DX12Engine::Scene::SceneConstructData& sceneData);

    DX12Engine::Scene::SceneManager* m_sceneMgr = nullptr;
    DX12Engine::Boot::GameContext* m_context = nullptr;
    std::unique_ptr<DX12Engine::Scene::SceneConstructor> m_sceneCtor;
    bool m_initialized = false;
};
```

### 4.5 Game 端不需要 SceneDescription

Game 端场景加载走打包数据（资源包、二进制关卡文件），不经过 JSON 解析。`SceneDescription` 是 Editor 端的中间产物，Game 端不需要关心。

```
Game 端加载路径：
    SceneLoader::LoadFromFile → SceneConstructor::LoadScene → GeneratorTaskCompleteEvent
    → GameSceneManager::OnSceneConstructReady()
    → CreateEntity() + RegisterEntity()

Editor 端加载路径：
    EditorSceneManager::GetDefaultSceneDescription() → AssetBrowser::LoadSceneDescription()
    → SceneConstructor::LoadScene → GeneratorTaskCompleteEvent
    → EditorSceneManager::OnSceneConstructReady()
    → CreateEntity() + RegisterEntity()
```

---

## 5. EditorSceneManager — 编辑器端特化

### 5.1 位置

```
Editor/EditorLib/Scene/EditorSceneManager.h
Editor/EditorLib/Scene/EditorSceneManager.cpp
```

### 5.2 设计模式：组合包装

`EditorSceneManager` **不继承** `SceneManager`，而是**组合包装** `SceneManager*`。与 `GameSceneManager` 采用相同模式。

```
EditorSceneManager
  ├─ SceneManager* m_sceneMgr  ← 被包装的 Bootstrap SceneManager
  ├─ 包装方法：CreateEntity() / RegisterEntity() / GetRegistry() / GetSceneManager()
  ├─ RegisterSceneConstructSystem()  ← 注册场景构造系统
  ├─ GetDefaultSceneDescription()    ← 默认场景描述（静态方法）
  ├─ EntityDesc 缓存（用于导出）
  └─ 场景文件管理：NewScene / SaveScene / ExportToDescription
```

### 5.3 特化职责

| 职责 | 说明 |
|:-----|:------|
| **注册 SceneConstructSystem** | 响应 `GeneratorTaskCompleteEvent`，通过 `CreateEntity` + `RegisterEntity` 构造实体 |
| **EntityDesc 缓存** | 维护 `EntityDesc` 列表，支持 Outliner 编辑后同步 ECS 组件 |
| **场景导出** | `ExportToDescription()` 遍历 ECS Registry 实时读取组件数据，`m_entityDescs` 作为路径查找表（geometry/material key）回退，变换等动态数据以 ECS 为准 |
| **默认场景描述** | 提供 `GetDefaultSceneDescription()` 静态方法，返回标准天空盒场景 |
| **场景文件管理** | 新建/保存场景文件（加载编排由 AssetBrowser 负责） |

### 5.4 接口示例

```cpp
// Editor/EditorLib/Scene/EditorSceneManager.h

class EditorSceneManager {
public:
    // 初始化
    void Initialize(DX12Engine::Scene::SceneManager* sceneMgr,
                    DX12Engine::Boot::GameContext* context);

    // 包装方法
    DX12Engine::Scene::SceneManager* GetSceneManager() const { return m_sceneMgr; }
    uint64_t CreateEntity() { return m_sceneMgr ? m_sceneMgr->CreateEntity() : UINT64_MAX; }
    void RegisterEntity(uint64_t entity) { if (m_sceneMgr) m_sceneMgr->RegisterEntity(entity); }
    DX12Engine::ECS::Registry* GetRegistry() const { return m_sceneMgr ? m_sceneMgr->GetRegistry() : nullptr; }

    // 场景构造系统注册
    void RegisterSceneConstructSystem();

    // 场景文件管理
    void NewScene(const std::string& name);
    void SaveScene();
    void SaveSceneAs(const std::filesystem::path& filePath);
    const std::filesystem::path& GetSceneFilePath() const { return m_sceneFilePath; }
    bool IsDirty() const { return m_dirty; }
    void MarkDirty() { m_dirty = true; }
    void ClearDirty() { m_dirty = false; }

    // EntityDesc 编辑
    DX12Engine::Resource::EntityDesc* GetMutableEntityDesc(uint64_t entity);
    void UpdateEntityDesc(uint64_t entity, const DX12Engine::Resource::EntityDesc& newDesc);

    // 导出
    DX12Engine::Resource::SceneDescription ExportToDescription() const;

    // 默认场景描述
    static DX12Engine::Resource::SceneDescription GetDefaultSceneDescription();

private:
    void OnSceneConstructReady(const DX12Engine::Scene::SceneConstructData& sceneData);

    DX12Engine::Scene::SceneManager* m_sceneMgr = nullptr;
    DX12Engine::Boot::GameContext* m_context = nullptr;
    std::filesystem::path m_sceneFilePath;
    bool m_dirty = false;
    std::unordered_map<uint64_t, DX12Engine::Resource::EntityDesc> m_entityDescs;
    bool m_initialized = false;
};
```

### 5.5 EntityDesc 可编辑设计

```
Outliner 编辑实体属性
    │
    ├─ EditorSceneManager::UpdateEntityDesc(entity, newDesc)
    │     ├─ 更新 m_entityDescs[entity]（持久化存储，用于导出）
    │     ├─ 更新 ECS 组件（TransformComponent, MeshComponent, ...）
    │     ├─ 广播 EntityChangeEvent::ComponentChanged
    │     │     └─ 子场景模块同步更新（RenderScene 更新渲染代理等）
    │     ├─ PushUndoState("Edit Entity")
    │     └─ MarkDirty()
    │
    └─ 保存时 ExportToDescription() 遍历 ECS Registry 实时读取组件数据，
        m_entityDescs 仅作为路径查找表（geometry/material key）回退使用
    
为什么 SceneManager 要持有 EntityDesc？
    - ECS 组件存储运行时 GPU 句柄（GeometryHandle/MaterialHandle），不含源文件路径
    - m_entityDescs 作为"路径查找缓存"保存几何体/材质的 source key
    - 导出时变换等动态数据以 ECS 为权威源，路径从 m_entityDescs 缓存读取
    - 两者关系：ECS 是运行时状态，m_entityDescs 是序列化元数据的缓存
```

### 5.5 差异合并（Merge）

```
EditorSceneManager::MergeScene("另一个场景.json", MergeMode::KeepExisting)
    │
    ├─ SceneLoader::LoadFromFile → SceneDescription other
    ├─ 遍历 other.entities:
    │     ├─ 如果 name 与当前场景冲突：
    │     │     ├─ KeepExisting: 跳过，保留当前
    │     │     ├─ Overwrite: 替换当前实体
    │     │     └─ Rename: 重命名后添加
    │     └─ 如果不冲突：直接添加
    ├─ AddEntities(new entities)  （通过 CreateEntity + RegisterEntity 流程）
    └─ MarkDirty()
```

---

## 6. 与现有系统的关系

### 6.1 现有组件的演变

| 现有组件 | 当前角色 | 迁移后的角色 | 变更 |
|:---------|:---------|:-------------|:-----|
| **SceneConstructor** | 场景加载 + ECS 构造 | 场景文件的加载器（一种加载器） | 职责收窄：只负责文件解析→依赖加载→ECS 构造，不再管理场景生命周期 |
| **EditorScene** | 包装 SceneConstructor | 废弃，由 EditorSceneManager 替代 | 完全替换，旧文件已删除 |
| **EditorSceneManager** | 继承 SceneManager | 组合包装 SceneManager* | 取消继承，改为包装；移除 LoadDefaultScene/LoadSceneFile，不再负责加载编排；新增 SetupDefaultCamera() |
| **GameSceneManager** | 不存在 | 组合包装 SceneManager*（新建） | 与 EditorSceneManager 同模式，提供 RegisterSceneConstructSystem/LoadSceneAsync |
| **GameWorld** | 直接持有 SceneConstructor + Registry | 通过 GameSceneManager 访问场景 | 移除 m_sceneConstructor/m_asyncLoadDelay/m_asyncScenePath；m_registry 来自 GameSceneManager::GetRegistry() |
| **SceneConstructSystem** | 响应事件构造 ECS 实体 | 保留，但改为调用 SceneManager::CreateEntity + RegisterEntity | 逻辑不变，落点从直接 m_registry 改为 SceneManager 的受控流程 |
| **AssetBrowser** | 仅文件浏览 | 场景加载编排者 | 新增 LoadSceneDescription()，持有 SceneConstructor 生命周期 |
| **SharedDataStore** | 中转 SceneConstructData | 保留，但数据生命周期缩短 | 构造完成后不再保留，转入 SceneManager 的 EntityDesc 列表 |
| **ECS::Registry** | 实体存储，外部直接访问 | SceneManager 内部私有成员，不对外暴露 | 外部通过 SceneManager 的 EntityHandle API 操作实体；内部 System 通过 `GetRegistryForInternalUse()` 访问完整 ECS 能力 |
| **LightManager** | 全局单例，管理光源 | 保持单例，通过 RenderScene 上下文访问 | RenderScene 持有引用，场景切换时驱动 Clear()；消费者改为 `sceneMgr->GetRenderScene()->GetLightManager()` |
| **ReflectionProbeManager** | 全局单例 | 保持单例，通过 RenderScene 上下文访问 | 同 LightManager 模式 |
| **AmbientOcclusionManager** | 全局单例 | 保持单例，通过 RenderScene 上下文访问 | 同 LightManager 模式 |
| **CameraManager** | Bootstrap 初始化 | 各端自行初始化（Editor/Game 各自管理） | 已移出 Bootstrap；Editor 端通过 EditorSceneManager::SetupDefaultCamera() 初始化 |
| **SchedulerContext** | 线程局部上下文 | 废弃 | 已移除（`GetSchedulerContext` 等全部删除，CameraManager 改直接访问 GameContext） |
| **DescriptorHeaps** | 基础设施，各消费者各自持有指针 | RenderScene 统一持有，一处注入 | RenderScene 持有 DescriptorHeaps*，消费者通过 RenderScene 获取 |
| **GameContext** | 依赖注入容器，持有所有子系统指针 | 保留，但部分字段被高级抽象替代 | `Registry` 指针移除（归 SceneManager）；`CameraMgr` 不再由 Bootstrap 初始化；`FrameDriver`、`BackgroundExecutor` 等字段保留 |
| **ISceneModule / IListener** | 同步虚函数回调 + 广播监听 | 废弃，由 MessageDispatcher 事件替代 | 实体变更/场景生命周期通知通过 MessageDispatcher 异步分发，不再维护 SceneManager 内部的 m_modules/m_listeners 列表 |

### 6.2 与 SceneConstructor 的协作关系

```
SceneConstructor（场景文件加载器）
    │
    ├─ 1. SceneLoader::LoadFromFile → SceneDescription
    ├─ 2. AssetManager::LoadBatch 加载依赖
    ├─ 3. OnDependenciesLoaded：材质注册、buffer 上传
    ├─ 4. 构造 SceneConstructData
    │
    ├─ 路径 A（当前）：PostEvent(GeneratorTaskCompleteEvent) → SceneConstructSystem → ECS 构造
    │
    └─ 路径 B（SceneManager 接入后）：PostEvent(GeneratorTaskCompleteEvent) 
          → SceneConstructSystem 
          → EditorSceneManager::CreateEntity + RegisterEntity  ← 统一入口
          → SceneManager 内部追加到扁平实体列表
```

### 6.3 资源生命周期管理

> ⚠️ **当前设计决策**：编辑器端场景切换不再触发 GPU 资源释放。
> 资源释放仅在场景 Tab 关闭时执行（见 §10.3）。
> 以下 `PrepareSceneSwitch` 的资源释放流程保留为 Game 端参考，
> Editor 端改用 Tab 关闭时的统一释放路径。

场景切换时的 GPU 资源释放，由 SceneManager 驱动：

```
SceneManager::PrepareSceneSwitch("新场景", Immediate)
    │
    ├─ 遍历所有非持久化实体
    │     ├─ 收集实体引用的 GeometryHandle / MaterialHandle / TextureHandle
    │     └─ 记录到一个资源释放列表
    │
    ├─ 移除实体（ECS Destroy）
    │
    ├─ 请求 GpuResourceManager::Release 释放 GPU 资源（延迟到 GPU 空闲）
    └─ 请求 GeometryResourceManager::Release 释放槽位
```

具体实现见 `SceneFileAndLoading.md §6.3` 和 `SceneFileAndLoading.md §7`（场景大堆方案）。

### 6.4 GameContext 的演变

#### 现状

`GameContext` 是 Bootstrap 阶段的依赖注入容器，持有一个扁平的指针集合：

```
GameContext
  ├─ 基础设施：ProjectConfig, Window, Config, Logging, MainTimer, Dispatcher
  ├─ 调度层：  FrameDriver, BackgroundExecutor, Registry
  ├─ 渲染层：  DeviceContext, FrameResourceManager, DescriptorHeaps,
  │            MaterialMgr, TextureMgr, GeometryResourceManager, ...
  └─ 输入/工具：InputManager, CullingSystem, LODSystem, ...
```

#### 问题

| 问题 | 表现 |
|:-----|:------|
| **语义模糊** | `GameContext` 同时持有基础设施（Timer/Logging）和业务系统（Registry/LightManager），没有领域边界 |
| **僵化** | 每个新子系统都往 GameContext 加一个指针，没有收敛机制 |
| **绕过风险** | System 持有 `m_context` 可以绕开 SceneManager 直接访问 Registry |

#### 演变方向

SceneManager 作为运行时中心出现后，GameContext 中部分字段逐步被高级抽象替代：

```
Bootstrap 阶段：                   运行时：
  GameContext 作为组装容器            SceneManager 作为场景中心
  ├─ Registry ──────────────────→    SceneManager 内部私有
  ├─ FrameDriver                    FrameDriver 独立
  ├─ BackgroundExecutor             独立
  ├─ DeviceContext ──────────────→   Renderer 直接持有
  ├─ MaterialManager                独立
  ├─ CameraManager                  独立
  ├─ Timer, Logging, Config         保留在 GameContext
  └─ ...                            ...
```

GameContext 中**只保留真正的基础设施**（Timer、Logging、Config、Window、Dispatcher），**业务系统指针**（Registry、各 Manager）逐步迁移到 SceneManager 或各子系统的自有访问路径。

#### 长期形态

```
GameContext（轻量）              SceneManager（场景中心）
  ├─ ProjectConfig                  ├─ Registry（内部私有）
  ├─ Window                         ├─ 子场景模块（RenderScene, PhysicsScene...）
  ├─ ConfigManager                  ├─ 实体生命周期（Retain/Release）
  ├─ Logger                         ├─ 环境状态（天空盒、环境光）
  ├─ MainTimer                      └─ 待处理队列
  ├─ MessageDispatcher
  ├─ BackgroundExecutor
  └─ FrameDriver
```

注意：`BackgroundExecutor` 和 `FrameDriver` 是调度层的核心基础设施，与场景无关，即使 SceneManager 完全成熟后它们也保留在 GameContext 中——SceneManager **使用**它们，但不 **管理** 它们。

#### 迁移路径

| 步骤 | 内容 | 影响 |
|:----:|:-----|:-----|
| 1 | SceneManager 内部创建并持有 `Registry`，不再从 GameContext 获取 | 移除 `GameContext::Registry` |
| 2 | SceneManager 替代 `EditorScene` 和 `GameWorld` 中的场景管理职责 | 移除 `GameWorld::m_sceneConstructor` |
| 3 | 逐步将各 Manager 的访问收敛到 SceneManager 的子场景模块 | 按需，不强制 |
| 4 | `SchedulerContext` 整体移除（已无人使用） | 移除 `FrameDriver.h` 中的定义和 `Bootstrap.cpp` 中的调用 |

---

## 7. 加载器体系

### 7.1 加载器注册表

```
SceneManager 不直接管理加载器，但提供统一的实体填充接口：

    ┌───────────────────────────────────────────────────┐
    │                  SceneManager                      │
    │    CreateEntity + RegisterEntity(EntityDesc)  ← 统一入口               │
    └───────────────────────────────────────────────────┘
                ▲           ▲           ▲
                │           │           │
    ┌───────────┐  ┌───────────┐  ┌───────────┐
    │ Scene     │  │Character  │  │Streaming  │  ← 各种加载器
    │Constructor│  │Loader     │  │Loader     │
    └───────────┘  └───────────┘  └───────────┘
```

| 加载器 | 输入 | 输出 | 触发方式 |
|:-------|:-----|:-----|:---------|
| **SceneConstructor** | `.scene.json` 文件路径 | `EntityDesc` 列表 | 编辑器打开场景、Game 加载关卡 |
| **CharacterLoader** | 角色模板 ID | `EntityDesc`（含 SkinnedComponent） | 角色生成请求 |
| **StreamingLoader** | 区域 ID | `EntityDesc` 列表 | 玩家移动触发 |
| **PrefabLoader** | 预制体模板 | `EntityDesc`（含子节点） | 编辑器放置、游戏运行时实例化 |

### 7.2 加载器通用模式

```cpp
// 所有加载器遵循相同的模式：
class ILoader {
    virtual ~ILoader() = default;
    
    /// 启动加载（异步）
    virtual void StartLoad() = 0;
    
    /// 是否加载完成
    virtual bool IsComplete() const = 0;
    
    /// 获取加载结果（完成时调用）
    /// 返回 EntityDesc 列表，供生成器调用 SceneManager::CreateEntity + RegisterEntity 消费
    virtual std::vector<Resource::EntityDesc> GetResult() = 0;
};
```

---

## 8. 设计决策记录

| 决策 | 选项 | 选择 | 理由 |
|:-----|:-----|:-----|:------|
| Entity 创建机制 | AddEntity 内部创建组件 vs CreateEntity + 生成器创建组件 | **两步走（CreateEntity + 生成器创建组件 + RegisterEntity）** | SceneManager 不耦合组件类型，生成器自由组合组件；`CreateEntity` + `RegisterEntity` 配对保证生命周期受控 |
| 异步加载结果接入 | 直接回调 vs 事件 vs 队列 | **待处理队列（ProcessPendingChanges）** | 主线程统一位置消费，延迟确定，避免竞态 |
| SceneManager 是否持有 Registry 引用 | 持有 vs 不持有 | **内部持有 unique_ptr，不对外暴露** | 外部通过 EntityHandle API 操作，不依赖 entt 类型；内部 System 通过 `GetRegistryForInternalUse()` 保留完整 ECS 能力 |
| EntityDesc 是否在运行时保留 | 仅 ECS 组件 vs 保留 EntityDesc | **Editor 端保留，Game 端不保留** | Editor 需要导出 JSON，Game 端不需要序列化元数据 |
| 实体父子层级归属 | 核心 SceneManager vs 编辑器端 | **编辑器端 EditorStateFile** | 场景文件是扁平格式，父子层级只在编辑时构建，存储在 EditorStateFile 缓存中；Game 端不需要 |
| 场景切换策略 | 全量清除 vs 差异化更新 | **两者都支持** | `RemoveAll` + `PersistEntity` 可组合出全量/增量/叠加 |
| 子场景模块通知机制 | 同步回调（ISceneModule）vs 事件驱动（MessageDispatcher） | **MessageDispatcher 事件驱动** | 实体增删改是低频操作，事件驱动足够；解耦、线程安全、复用已有事件系统，减少 SceneManager 耦合度 |
| **EditorSceneManager 设计模式** | 继承 SceneManager vs 组合包装 | **组合包装 SceneManager*** | 基类设计不是为派生而生的；组合模式不违反 LSP；编辑器功能是横向扩展而非纵向特化 |
| **GameSceneManager 设计模式** | 继承 SceneManager vs 组合包装 | **组合包装 SceneManager***（同 Editor） | 与 Editor 一致，统一模式 |
| **场景加载职责归属** | SceneManager 负责 vs AssetBrowser 编排 | **AssetBrowser 编排** | 加载是业务流程，不是管理能力；SceneManager 只负责实体生命周期 |

---

## 9. 路线图

| 阶段 | 内容 | 前置条件 |
|:----:|:------|:---------|
| **P0 ✅** | 文档定稿 + 接口定义 | — |
| **P0 ✅** | 实现 `SceneManager` 基类核心接口（实体管理、生命周期、环境状态） | P0 文档 |
| **P0 ✅** | 实现 `EditorSceneManager` 组合包装（替代 `EditorScene`，支持导出） | P0 |
| **P0 ✅** | 实现 `GameSceneManager` 组合包装（场景加载、系统注册） | P0 |
| **P0 ✅** | 实现 `RenderScene` 渲染上下文容器（聚合管理器引用 + 共享基础设施指针） | P0 |
| **P0 ✅** | 场景加载职责分离：AssetBrowser 编排，EditorSceneManager 仅管理 | P0 |
| **P0 ✅** | 移除 ISceneModule/IListener，改为 MessageDispatcher 事件驱动 | P0 |
| **P1** | 场景切换的资源释放（GPU 资源回收） | P0 |
| **P2** | 实现 Undo/Redo 系统 | P1 EditorSceneManager |
| **P3** | 流式加载（StreamingLoader） | P2 GameSceneManager |
| **P3** | 多场景 Tab 编辑 | P2 EditorSceneManager |
| **P4** | 物理/音频/寻路等按需扩展（通过 MessageDispatcher 事件接入，非 ISceneModule） | P1 事件体系稳定 |

---

## 10. 编辑器多 Tab 场景架构

### 10.1 设计原则

多 Tab 架构遵循以下原则：

| 原则 | 说明 |
|:-----|:------|
| **单一 ECS Registry** | 所有场景共享同一个 ECS Registry，切换场景时清除旧实体、加载新实体，而非多 Registry 并存 |
| **数据驱动切换** | 切换不修改各管理器（LightManager 等），只替换 ECS 数据，管理器通过 EntityChangeEvent 自然响应 |
| **ECS 数据与编辑器状态分离** | 场景 JSON（扁平，为 Game 设计）与 EditorStateFile（层级，为编辑器友好设计）通过 persistentId 关联 |
| **Tab 关闭时释放资源** | 关闭 Tab 时释放 GPU 资源引用并写入磁盘，切换 Tab 时只操作内存 |

### 10.2 Tab 管理

#### 10.2.1 SceneTab 结构

`SceneTab` 是轻量追踪结构，不持有独立的 ECS Registry：

```cpp
struct SceneTab {
    std::string name;               // 场景显示名称
    std::filesystem::path filePath; // 场景文件路径
    bool dirty = false;             // 是否有未保存修改
};
```

#### 10.2.2 初始状态与默认场景

编辑器启动时 **不创建默认场景 Tab**，Viewport 初始为空，提示用户打开或创建场景。

```
编辑器启动时：
  m_openTabs = []                          ← 空，无任何 Tab
  Viewport 显示空白或提示"打开场景文件"

双击 forest.json 后：
  m_openTabs = [
    { name: "ForestScene", filePath: "Content/Scenes/forest.json", sceneId: 1 }
  ]
  m_activeTabIndex = 0

双击 desert.json 后：
  m_openTabs = [
    { name: "ForestScene", ... },
    { name: "DesertScene", filePath: "Content/Scenes/desert.json", sceneId: 2 }
  ]
  m_activeTabIndex = 1

关闭最后一个 Tab 后：
  m_openTabs = []                          ← 回到空状态
  Viewport 显示空白
```

**关键行为**：
- 无隐式 Tab，所有 Tab 都对应真实场景文件
- `m_openTabs` 允许为空，`DrawTabBar` 在空时不渲染
- 关闭最后一个 Tab 时清除实体，回到空状态
- 启动时 Editor 负责显示提示（"打开场景文件"或"新建场景"）

#### 10.2.3 ImGui Tab 栏渲染

Tab 栏采用**与视口并列**的布局，非嵌入视口内部。TabBar 渲染为独立薄条，紧贴视口窗口上方，类似于 VS2022 的 dock tab 风格：

```
Dock 区域布局（垂直方向）：
  ┌─────────────────────────────────────┐
  │ [ForestScene] [DesertScene]  [×]    │ ← TabBar 薄条（独立窗口，不占视口空间）
  ├─────────────────────────────────────┤
  │                                     │
  │         视口渲染图像                 │ ← Viewport 窗口，完整渲染区域
  │                                     │
  │         网格比例尺滑条               │
  └─────────────────────────────────────┘
```

**实现方式**：

- TabBar 是一个独立窗口，通过 `EditorLayout` 的 `SetViewportTabBarCallback` 注册
- 渲染位置在 `EditorLayout::DrawViewport()` 与 `DrawDockSpace()` 之间
- 使用 `ImGui::BeginTabBar`/`BeginTabItem` 渲染，不包含内容区，仅显示 Tab 标签
- 视口窗口不包含 TabBar，渲染区域最大化

**无 Tab 时（默认场景）**：TabBar 窗口不渲染，视口完整显示。

### 10.3 场景切换流程

#### 首次加载（双击场景文件）

```
AssetBrowser::OnFileDoubleClick("forest.json")
  │
  ├─ m_onSwitchScene → SwitchScene("forest", "Content/Scenes/forest.json")
  │     ├─ 保存当前编辑器状态（相机、层级、选中）
  │     ├─ 创建新 Tab（分配 sceneId）
  │     └─ 更新 m_activeScenePath
  │
  └─ LoadSceneFromFile("forest.json")
        └─ SceneLoader::LoadFromFile 解析 JSON
        └─ m_sceneCtor.LoadScene(desc, ...)  ← 异步加载
              │
              ▼
        OnSceneConstructReady(sceneData)
              ├─ 捕获 m_sceneSwitchId（检测过期回调）
              ├─ 创建实体 + 添加 SceneTagComponent(sceneId)
              ├─ 缓存到 m_tabEntities / m_tabEntityDescs
              ├─ 检查 m_sceneSwitchId 是否变化
              │     ├─ 变化 → 丢弃实体（过期回调）
              │     └─ 未变 → 继续
              └─ 恢复编辑器状态
```

#### Tab 切换（点击已有 Tab）

```
Tab 点击 → DrawTabBar → m_pendingSwitchTab = i
  │
  ▼
ProcessPendingTabSwitch()  ← FrameDriver::Tick() 后执行
  │
  ├─ 保存当前编辑器状态（相机、层级、选中）
  ├─ 更新 m_activeTabIndex（不清除实体，SceneTagComponent 过滤由 Builder 负责）
  └─ 恢复编辑器状态
```

- 实体已在 Registry 中，不需要重新加载
- Builder 每帧通过 `SetEntityFilter` 按 `activeSceneId` 过滤
- Outliner 通过 `GetActiveEntities()` 获取活跃 Tab 的实体列表

### 10.4 关闭 Tab

```
CloseTab(index)
  │
  ├─ 如果关闭的是当前活跃 Tab 且存在其他 Tab
  │     ├─ 保存当前编辑器状态
  │     └─ 更新 m_activeTabIndex 到上一个 Tab（不清除实体）
  │
  ├─ 从 m_openTabs 移除
  ├─ 清理该 Tab 的缓存数据（m_tabEntities, m_tabEntityDescs, ...）
  │
  └─ 如果是最后一个 Tab
        ├─ RemoveAllEntities()  ← 清除所有实体
        ├─ 清空 m_activeScenePath
        └─ Viewport 显示提示 "Open a scene file to start editing"
```

关闭 Tab 时，GPU 资源引用计数通过 `GpuResourceManager::Update()` 在 fence 回调中自然递减，不需要显式释放。

### 10.5 异步加载竞态保护

使用场景切换序列号 `m_sceneSwitchId` 检测过期回调：

```
SwitchScene → m_sceneSwitchId++

OnSceneConstructReady:
  captureSwitchId = m_sceneSwitchId    ← 捕获当前值
  // ...创建实体...
  if (m_sceneSwitchId != captureSwitchId) {
    // 用户已切换到另一个场景，丢弃本次结果
    RemoveAllEntities()
    return
  }
```

### 10.6 与 EditorStateFile 的关系

每个场景的编辑器状态（相机、层级、选中）存储在独立文件中：

```
Content/Cache/Editor/
  ├─ forest.scene.json         ← 相机 + hierarchy + selection
  ├─ desert.scene.json
  └─ ...

场景 JSON（扁平，Game 端使用）：
  Content/Scenes/forest.scene.json
  Content/Scenes/desert.scene.json
```

两者通过场景文件名关联，各自独立存储，互不包含。

### 10.7 文件位置

```
<ProjectRoot>/Content/Cache/Editor/     ← 编辑器缓存
  ├─ forest.scene.json                  ← 场景编辑器状态
  ├─ desert.scene.json
  ├─ ...
  └─ （thumbnails.thumb 在 Content/Cache/Thumbnails/ 下）
```

---

## 11. SceneManager 职责边界

> 2026-07-23 补充：明确 SceneManager 与非场景实体的关系，以及组件值修改的归属。

### 11.1 ECS Registry 是唯一的数据容器

**ECS Registry 是全局数据容器，不是 SceneManager 的私有财产。** 任何模块都可以创建/销毁实体和组件：

```
ECS Registry（全局数据容器）
  │
  ├─ SceneManager —— 场景实体
  │     ├─ LoadScene / SaveScene（场景文件 ↔ ECS 的序列化桥梁）
  │     ├─ CreateEntity / RemoveEntity（增删）
  │     ├─ GetEntity / QueryEntities（查）
  │     └─ ❌ 不负责组件值的修改（改）
  │
  ├─ PreviewSystem —— 预览实体
  │     ├─ 资产预览（Mesh、Material 缩略图）
  │     └─ 这些实体不在任何场景中
  │
  ├─ DebugSystem —— 调试实体
  │     ├─ 调试球体、射线、碰撞体可视化
  │     └─ 运行时创建，不持久化
  │
  └─ 运行时系统（未来）
        ├─ 角色、动画、弹道、血量等游戏逻辑实体
        └─ 由 Gameplay 系统创建，不经过 SceneManager
```

### 11.2 SceneManager 的职责

| 职责 | 说明 |
|:-----|:------|
| **场景文件 ↔ ECS 的序列化桥梁** | LoadScene 加载 JSON → 创建实体；SaveScene 读取实体 → 写入 JSON |
| **场景实体 CRUD** | CreateEntity / RemoveEntity / GetEntity / QueryEntities |
| **场景生命周期管理** | 加载、卸载、切换、叠加场景 |
| **环境状态管理** | 天空盒、光照、水面等环境参数 |
| **场景实体集合维护** | 标记哪些实体属于哪个场景（通过 SceneTagComponent） |

### 11.3 SceneManager 不负责的

| 不属于 SceneManager 的职责 | 归属 |
|:---------------------------|:------|
| **组件值的修改** | 各 System 自行负责（GizmoSystem 改 Transform、AnimationSystem 改骨骼） |
| **选中实体** | SelectionService（独立） |
| **组件属性编辑** | ComponentEditorRegistry（注册制） |
| **非场景实体的创建/销毁** | 预览系统、调试系统、运行时游戏系统 |
| **输入路由** | InputSystem + 各 System 的输入回调 |

### 11.4 组件值修改的归属

```
组件值修改 ── 谁触发、谁负责，不经过 SceneManager
  │
  ├─ GizmoSystem
  │     └─ 拖拽 Gizmo → 直接写 TransformComponent.position
  │
  ├─ ComponentEditorRegistry（属性卡）
  │     └─ 用户在属性卡中修改 → 直接写对应组件字段
  │
  ├─ AnimationSystem
  │     └─ 每帧更新骨骼矩阵 → 直接写 SkinnedComponent
  │
  ├─ PhysicsSystem
  │     └─ 物理模拟 → 直接写 TransformComponent.position
  │
  └─ ... 其他系统
```

SceneManager 只负责**增删查**，**改**由各系统自行负责。这使得 SceneManager 保持薄层，不成为所有修改的瓶颈。

### 11.5 选中实体与 SceneManager 的解耦

**选中实体不依赖 SceneManager。** 无论实体来自场景、预览系统还是调试系统，都可以被选中：

```
EditorSelection（独立服务）
  ├─ SetSelectedEntity(entity) → 广播 OnSelectionChanged
  ├─ GetSelectedEntity() → Entity
  └─ 不关心实体来自哪个场景/系统

OutlinerPanel → 点击 → SetSelectedEntity
Viewport → 射线检测 → SetSelectedEntity
Properties → 监听 OnSelectionChanged → 读取组件 → 绘制
Viewport Gizmo → 监听 OnSelectionChanged → 更新 Gizmo 目标
```

### 11.6 设计原则

1. **ECS Registry 是唯一的全局数据容器**，不是 SceneManager 的私有财产
2. **SceneManager 是场景实体的权威管理者**，但不是所有实体的唯一源头
3. **组件值的修改归属各 System**，SceneManager 只做 CRUD 中的 C、R、D（创建、读取、删除），不做 U（更新）
4. **选中实体是全局状态**，独立为 SelectionService，不依赖 SceneManager
5. **场景实体通过 SceneTagComponent 标记**，SceneManager 维护实体 ↔ 场景的映射关系

---

## 12. World 提取与 SceneManager 降级

> 2026-07-23 补充：World 作为 ECS 绝对源头从 SceneManager 中提取，SceneManager 降级为场景序列化器 + 环境状态容器。

### 12.1 动机

当前 `SceneManager` 职责过重：它既是 ECS 实体管理容器，又是场景文件加载器，又被 Editor 和 Game 各自特化。但 `ECS::Registry` 作为全局数据容器并不属于 `SceneManager`——预览系统、调试系统都在往 Registry 里写实体，`SceneManager` 没有能力也不应该管理它们。

**核心问题：SceneManager 混淆了"场景管理"和"ECS 容器"两个角色。**

### 12.2 新架构：World + SceneManager 分离

```
World（ECS 绝对源头，Engine Core）      SceneManager（场景序列化器，Engine Core）
  ├─ 持有 ECS::Registry                    ├─ 持有 World* 引用
  ├─ CreateEntity / DestroyEntity          ├─ LoadScene(path) → world.CreateEntity()
  ├─ 所有实体必须属于这个 World             ├─ SaveScene(path) → 查询 → 写入
  ├─ World 本身不分区                       ├─ 环境状态：Skybox/Environment
  └─ 不感知任何逻辑分组                     ├─ 场景生命周期：切换/卸载/叠加
                                           └─ 不再持有 Registry

  Editor 端特化：
    EditorSceneManager（逻辑分区 + 多 Tab 管理）
      ├─ 继承/包装 SceneManager
      ├─ 通过 SceneTagComponent 查询场景实体集合
      ├─ 多 Tab 管理（多个场景文件同时打开）
      ├─ SceneSnapshot 快照
      └─ ApplyTabState 恢复全局管理器状态

  Game 端特化：
    GameSceneManager（关卡加载/切换）
      ├─ 包装 SceneManager
      ├─ 单场景加载/卸载
      ├─ 关卡切换过渡
      └─ 不需要 SceneTagComponent（所有实体都是游戏内容）
```

### 12.3 角色对比

| 角色 | 类 | 归属 | 数据持有 | 序列化 | 逻辑分区 |
|:-----|:---|:------|:---------|:-------|:---------|
| **ECS 源头** | `World` | Engine Core | `ECS::Registry` | ❌ | ❌ 不感知 |
| **场景序列化器** | `SceneManager` | Engine Core | `World*`、环境状态 | ✅ JSON ↔ ECS | ❌ 不感知 |
| **编辑器场景管理器** | `EditorSceneManager` | Editor | `SceneManager*`、`SceneSnapshot` | ✅ 委托给 SceneManager | ✅ SceneTagComponent |
| **游戏场景管理器** | `GameSceneManager` | Game | `SceneManager*` | ❌ 委托给 SceneManager | ❌ 不需要 |
| **预览管理器** | `PreviewManager` | Editor | `World*` | ❌ | ✅ PreviewTag（Editor 端） |
| **调试管理器** | `DebugManager` | Editor | `World*` | ❌ | ✅ DebugTag（Editor 端） |

### 12.4 SceneManager 的降级

#### 保留的职责（Engine Core）

| 职责 | 说明 |
|:-----|:------|
| **场景序列化** | LoadScene 从 JSON 创建实体到 World；SaveScene 从 World 查询实体写入 JSON |
| **环境状态管理** | 天空盒、环境光、光照等场景级参数 |
| **场景生命周期** | 加载、卸载、切换、叠加场景 |

#### Editor 端特化职责（EditorSceneManager）

| 职责 | 说明 |
|:-----|:------|
| **逻辑分区** | 通过 SceneTagComponent 查询当前场景的实体集合 |
| **多 Tab 管理** | 多个场景文件同时打开，Tab 切换切换 sceneId 查询条件 |
| **异步加载竞态保护** | 场景切换序列号检测过期回调 |
| **SceneSnapshot 快照** | per-tab 完整状态聚合，Tab 切换时恢复 |
| **ApplyTabState** | 切换 Tab 时 Clear + Rebuild 全局管理器状态 |

#### 移除的职责

| 职责 | 迁往 | 说明 |
|:-----|:------|:------|
| **持有 ECS::Registry** | → `World` | World 成为 Registry 的唯一所有者 |
| **实体生命周期管理** | → `World` | CreateEntity/DestroyEntity 委托给 World |
| **实体注册** | 废弃 | World 创建即管理，不再需要 RegisterEntity 步骤 |
| **实体清单维护** | 废弃 | 按 SceneTagComponent 动态查询（Editor 端），不再维护扁平列表 |
| **实体持久化** | 废弃 | PersistEntity/DontDestroyOnLoad 机制已移除，Editor 端用 SceneTagComponent 替代 |
| **待处理队列** | 废弃 | ProcessPendingChanges 骨架已移除，异步加载通过 BackgroundExecutor 回调直接完成 |
| **预移除回调** | 废弃 | RegisterEntityPreRemoveCallback 已移除，GPU 资源释放由 EditorSceneManager 的 CloseTab 流程处理 |

### 12.5 最终 SceneManager 接口

```cpp
// 当前 SceneManager（清理后）：
class SceneManager {
    World* m_world;                               // 引用 World，非拥有
    // 不再持有 Registry
    // 不再持有实体清单
    // 不再持有持久化集合

    // 初始化/销毁
    void Initialize(World* world);
    void Shutdown();

    // 委托给 World
    uint64_t CreateEntity() { return m_world->CreateEntity(); }
    void RemoveEntity(uint64_t entity) { m_world->DestroyEntity(static_cast<Entity>(entity)); }
    void RemoveAllEntities() { m_world->RemoveAllEntities(); }

    // 环境状态
    void SetSkybox(const Resource::SkyboxDesc& skybox);
    void SetEnvironment(const Resource::EnvironmentDesc& env);

    // 场景生命周期
    void PrepareSceneSwitch(const std::string& newSceneName, SceneTransition transition);

    // 子场景模块
    RenderScene* GetRenderScene();
    ECS::Registry* GetRegistry() { return m_world->GetRegistry(); }
};
```

### 12.6 逻辑分区：Editor 端特化

逻辑分区不再属于引擎 CORE 的 SceneManager，而是 Editor 端 EditorSceneManager 的特化行为：

```
// Editor 端——通过 SceneTagComponent 查询"场景实体"的视角
EditorSceneManager::GetSceneEntities(sceneId)
    └─ world->GetRegistry()->view<SceneTagComponent>()
          └─ filter: tag.sceneId == sceneId
          └─ 返回当前场景的实体列表

// 引擎 CORE——SceneManager 不感知 SceneTagComponent
// SceneManager 只做 LoadScene/SaveScene 的序列化工作
```

### 12.7 与现有设计的关系

| 现有组件 | 迁移前 | 迁移后 |
|:---------|:-------|:-------|
| **SceneManager** | 持有 Registry，管理实体生命周期 | 持有 World*，序列化器 + 解释器 |
| **World** | 不存在 | 新增，ECS 绝对源头 |
| **GameWorld** | 直接持有 Registry 指针 | 通过 World 访问 Registry |
| **EditorSceneManager** | 包装 SceneManager* | 包装 SceneManager*（接口不变，内部重定向） |
| **PreviewSystem** | 通过 SceneManager 的 Registry 创建实体 | 直接通过 World 创建实体 |
| **SceneTagComponent** | SceneManager 维护 | 保留，SceneManager 通过 Tag 查询逻辑分区 |

### 12.8 迁移路径

| 步骤 | 内容 | 影响 |
|:----:|:-----|:------|
| 1 | 从 Engine Core 中提取 `World` 类，持有 `ECS::Registry` | 新增，不破坏现有代码 |
| 2 | `SceneManager` 移除 `m_registry`，改为持有 `World*` | SceneManager 内部重构 |
| 3 | `SceneManager::CreateEntity`/`RemoveEntity` 委托给 `World` | 接口兼容，内部重定向 |
| 4 | `Bootstrap` 创建 `World` 实例，传给 `SceneManager` | Bootstrap 装配顺序调整 |
| 5 | 逐步替换 `SceneManager::GetRegistry()` 调用为 `World::GetRegistry()` | 各调用点替换 |
| 6 | Editor 端各 Manager 通过 TagComponent 提供逻辑分区视角 | 渐进式 |

> 详细设计见 `Docs/architecture/scene/World.md`。

---

## 13. SceneManager 与 SceneEnvironment 的关系

> 2026-07-28 补充：`SceneEnvironment` 作为 Manager 全局数据与 ECS 实体数据的显式语义分隔。

### 13.1 SceneManager 持有 SceneEnvironment

`SceneManager` 作为场景序列化器，是 `SceneEnvironment`（管理器全局场景数据）的**持有者和序列化入口**：

```
SceneManager
  ├─ SetEnvironment(EnvironmentDesc)  ← 设置环境光（→ LightManager::SetAmbientLight）
  ├─ GetEnvironment() → EnvironmentDesc
  ├─ SetSkybox(SkyboxDesc)            ← 设置天空盒（→ SkyboxManager::SetSkybox）
  ├─ GetSkybox() → optional<SkyboxDesc>
  │
  └─ 序列化时：
        ExportToDescription → desc.sceneEnvironment.ambient = GetEnvironment()
                           → desc.sceneEnvironment.skybox  = GetSkybox()
```

### 13.2 数据流向

```
JSON 文件                              SceneConstructor                          SceneManager
sceneEnvironment.ambient  ──→  SceneManager::SetEnvironment()  ──→  m_environment
sceneEnvironment.skybox   ──→  SkyboxManager::SetSkybox()      ──→  m_skybox
(不进入 ECS Registry)
```

### 13.3 与 ECS 实体数据的互斥关系

| 类别 | 存储位置 | 写入时机 | 读取时机 |
|:-----|:---------|:---------|:---------|
| `sceneEnvironment` | `SceneManager` 成员变量 | 场景加载时，由 `SceneConstructor` 直接设置 | 编辑器导出时，由 `ExportToDescription` 读取 |
| `entities` | `ECS::Registry` | 场景加载时，由 `SceneConstructSystem` 创建 ECS 实体 | Manager 每帧通过 ECS view 收集 |

SceneManager **不管理** ECS 实体数据——实体数据由 `World` 统一管理，SceneManager 只负责序列化时从 World 查询。`sceneEnvironment` 是 SceneManager 持有的唯一运行时数据。

### 13.4 JSON 中的位置

```
{
  "version": 1,
  "sceneEnvironment": {          ← SceneManager 持有，对应 GetEnvironment/GetSkybox
    "ambient": { ... },
    "skybox": { ... }
  },
  "dependencies": {...},         ← SceneConstructor 中间数据
  "materials": {...},            ← SceneConstructor 中间数据
  "entities": [...]              ← World（ECS Registry）持有，SceneManager 查询
}
```

`dependencies` 和 `materials` 是场景序列化的辅助元数据，不属于 SceneManager 或 World 的运行时状态——它们只存在于 `SceneSnapshot` 缓存中，用于 `ExportToDescription` 时重建原始路径和材质定义。