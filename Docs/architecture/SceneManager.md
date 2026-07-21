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
│  • 子场景模块管理              │            └──────────┬─────────────────┘
│  • 生命周期事件广播            │                       │
│  • 待处理队列                  │            ┌──────────────────────┐
└──────────────────────────────┘            │     子场景模块         │
                                            │  ┌──────────────────┐ │
                                            │  │  RenderScene     │ │
                                            │  ├──────────────────┤ │
                                            │  │  PhysicsScene    │ │
                                            │  ├──────────────────┤ │
                                            │  │  AudioScene      │ │
                                            │  ├──────────────────┤ │
                                            │  │  NavMeshScene    │ │
                                            │  └──────────────────┘ │
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

/// 实体变更类型（广播用）
enum class EntityChangeType {
    Added,
    Removed,
    ComponentChanged,
    TransformChanged,
};

/// 实体变更广播（同步，当前帧生效）
struct EntityChangeEvent {
    EntityChangeType type;
    uint64_t entity;  // EntityHandle（不暴露 ECS 内部类型）
    // 按 type 可附加更多信息
};

/// 场景生命周期事件（广播，当前帧生效）
struct SceneLifecycleEvent {
    SceneState oldState;
    SceneState newState;
    std::string sceneName;  // 当前场景名称
};

/// 场景子模块基类接口
class ISceneModule {
public:
    virtual ~ISceneModule() = default;
    virtual const char* GetModuleName() const = 0;
    
    /// 实体添加时调用（同步）
    virtual void OnEntityAdded(uint64_t entity) = 0;
    /// 实体移除时调用（同步）
    virtual void OnEntityRemoved(uint64_t entity) = 0;
    /// 实体组件变更时调用（同步）
    virtual void OnEntityComponentChanged(uint64_t entity, ComponentType type) = 0;
    
    /// 场景准备切换（卸载开始前）
    virtual void OnScenePreUnload() = 0;
    /// 场景切换完成（新场景已加载）
    virtual void OnScenePostLoad() = 0;
};

/// 场景管理器基类
///
/// 设计原则：
///   - ECS Registry 是内部实现，不对外暴露
///   - 外部通过 EntityHandle（uint64_t）操作实体，不接触 entt::entity
///   - 内部 System 和子场景模块通过 GetRegistryForInternalUse() 访问完整的 ECS 能力
///   - 实体创建/销毁强制走 SceneManager 接口，确保 Retain/Release + 子场景通知
class SceneManager {
public:
    SceneManager() = default;
    virtual ~SceneManager() = default;
    
    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;
    
    // ====================================================================
    // 初始化/销毁
    // ====================================================================
    
    /// 初始化（ECS Registry 由 SceneManager 内部创建）
    virtual void Initialize(Boot::GameContext* context);
    
    /// 销毁，清理所有实体和子场景
    virtual void Shutdown();
    
    // ====================================================================
    // 实体管理（核心）
    //
    // 设计原则：生成器（SceneConstructor、CharacterLoader 等）负责组件创建，
    // SceneManager 只负责管理（生命周期追踪、持久化、广播）。
    // 生成器是 Scene System 的一部分，通过 GetRegistryForInternalUse() 访问 ECS 完整能力。
    // ====================================================================
    
    /// 创建空实体（分配 entt::entity，返回 uint64_t handle）
    /// 生成器调用此方法获取实体 ID，然后通过 GetRegistryForInternalUse() 添加组件
    uint64_t CreateEntity();
    
    /// 批量创建空实体
    std::vector<uint64_t> CreateEntities(uint32_t count);
    
    /// 将已创建的实体注册到 SceneManager 的管理中
    /// 纳入追踪列表 + 广播 EntityChangeEvent::Added + 通知子场景模块
    /// 调用前，生成器应已完成组件的创建
    void RegisterEntity(uint64_t entity);
    void RegisterEntities(std::span<const uint64_t> entities);
    
    /// 移除实体（内部自动处理 Retain/Release + 子场景通知）
    void RemoveEntity(uint64_t entity);
    void RemoveEntities(std::span<const uint64_t> entities);
    
    /// 移除所有实体（场景全量切换时使用）
    void RemoveAllEntities();
    
    /// 获取当前场景的所有实体 Handle 列表（扁平，无父子层级）
    /// 父子层级是编辑器端概念，存储在 EditorStateFile 中
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
    
    /// 准备切换场景（由子类调用，或由加载器触发）
    /// 内部处理：广播 PreUnload → 保留 Persist 实体 → 清除其余实体 → 更新状态
    void PrepareSceneSwitch(const std::string& newSceneName, SceneTransition transition);
    
    // ====================================================================
    // 子场景模块管理
    // ====================================================================
    
    template<typename T>
    T* GetModule() {
        static_assert(std::is_base_of_v<ISceneModule, T>, "T must inherit ISceneModule");
        for (auto& mod : m_modules) {
            if (auto* casted = dynamic_cast<T*>(mod.get()))
                return casted;
        }
        return nullptr;
    }
    
    void RegisterModule(std::unique_ptr<ISceneModule> module);
    
    // ====================================================================
    // 广播监听（ISceneListener 接口）
    // ====================================================================
    
    class IListener {
    public:
        virtual ~IListener() = default;
        virtual void OnEntityChange(const EntityChangeEvent& evt) {}
        virtual void OnSceneLifecycle(const SceneLifecycleEvent& evt) {}
    };
    
    void AddListener(IListener* listener);
    void RemoveListener(IListener* listener);
    
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
    
    /// 内部 System 和子场景模块通过此方法访问完整的 ECS 能力
    /// 外部消费者（Editor、GameWorld、AssetBrowser）禁止调用
    ECS::Registry* GetRegistryForInternalUse() { return m_registry.get(); }
    
    // 子类可访问的状态
    Boot::GameContext* m_context = nullptr;
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
    
    // 子场景模块
    std::vector<std::unique_ptr<ISceneModule>> m_modules;
    
    // 监听器
    std::vector<IListener*> m_listeners;
    
    // 待处理队列（线程安全，后台加载器写入，主线程 ProcessPendingChanges 消费）
    // 使用锁或无锁队列
    ConcurrentQueue<PendingEntityChange> m_pendingChanges;
    
    // 内部方法：EntityHandle ←→ entt::entity 转换
    entt::entity ToInternal(uint64_t handle) const;
    uint64_t ToExternal(entt::entity e) const;
    
    // 广播实体变更
    void BroadcastEntityChange(const EntityChangeEvent& evt);
    void BroadcastSceneLifecycle(const SceneLifecycleEvent& evt);
    
    // 通知所有子场景模块
    void NotifyModulesEntityAdded(uint64_t entity);
    void NotifyModulesEntityRemoved(uint64_t entity);
    void NotifyModulesPreUnload();
    void NotifyModulesPostLoad();
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
    │     ├─ 广播 EntityChangeEvent::Added
    │     │     └─ 子场景模块同步响应
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

```
PrepareSceneSwitch("新场景", Immediate)
    │
    ├─ 广播 SceneLifecycleEvent(None → Unloading)
    ├─ 通知子场景模块 NotifyModulesPreUnload()
    │
    ├─ 遍历所有实体（m_entities）
    │     ├─ 如果 IsPersistent(entity) → 跳过，保留
    │     └─ 否则 → 移除实体
    │           ├─ 广播 EntityChangeEvent::Removed
    │           ├─ 通知子场景模块 OnEntityRemoved()
    │           └─ SceneManager 内部：m_registry->DestroyEntity()
    │
    ├─ 重建 m_entities 列表（仅保留 persistent 实体）
    ├─ m_sceneName = "新场景"
    ├─ m_state = Active
    │
    ├─ 通知子场景模块 NotifyModulesPostLoad()
    └─ 广播 SceneLifecycleEvent(Unloading → Active)
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

**为什么是 ProcessPendingChanges 而非直接回调**：

| 方式 | 问题 |
|:-----|:------|
| 直接在后台线程回调中 AddEntity | 后台线程 ≠ 主线程，ECS Registry 非线程安全 |
| 在回调中 PostEvent 事件驱动 | 事件传递延迟不可控，可能跨帧；且 ECS 组件的增删改频率高，事件驱动开销大 |
| **ProcessPendingChanges 统一消费** | 主线程同一位置、同一帧内批量处理，延迟确定，实体变更与 ECS System 执行之间无竞态 |

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
    
内部 System / 子场景模块
    │
    └─ SceneManager::GetRegistryForInternalUse()  ← 完整 enTT 能力
         ├─ auto view = reg->view<Transform, Mesh>();
         ├─ auto group = reg->group<Light>(entt::get<Transform>());
         └─ auto& ctx = reg->ctx().emplace<TimeOfDay>();
```

#### 动机

| 动机 | 说明 |
|:-----|:------|
| **生命周期强制** | 实体创建/销毁是唯一入口，Retain/Release、子场景通知强制执行，不会遗漏 |
| **ECS 可替换** | 可以从 enTT 迁移到自定义实体存储，不影响上层代码 |
| **接口稳定** | 上层代码只依赖 `SceneManager`，不依赖 enTT 的类型系统 |
| **跨平台/跨架构** | 未来需要网络同步、回放、确定性模拟时，可在 SceneManager 层统一拦截 |

#### 内部 System 不受影响

内部 System 和子场景模块通过 `GetRegistryForInternalUse()` 获得完整的 ECS 能力：

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

## 3. 子场景模块体系

### 3.1 设计动机

场景数据按领域拆分，每个子模块管理场景在该领域的表现，避免 SceneManager 变成一个巨大的上帝类。

```
SceneManager
    │
    ├── RenderScene
    │     ├── 渲染代理（RenderProxy）列表
    │     ├── 光照探针引用
    │     ├── 可见性信息
    │     └── 不负责渲染 Pass（渲染 Pass 由 Renderer 负责）
    │
    ├── PhysicsScene
    │     ├── 碰撞体列表
    │     ├── 物理材质
    │     ├── 关节约束
    │     └── 不负责物理模拟（PhysicsEngine 负责）
    │
    ├── AudioScene
    │     ├── 音频发射器列表
    │     ├── 声学区域
    │     ├── 混响设置
    │     └── 不负责音频播放（AudioEngine 负责）
    │
    └── NavMeshScene（后续）
          ├── 寻路网格
          ├── 阻挡区域
          └── 不负责寻路计算（Pathfinding 负责）
```

### 3.2 子模块职责边界

| 子模块 | 管理的数据 | 不做什么 |
|:-------|:-----------|:---------|
| **RenderScene** | 渲染代理、光照探针索引、可见性集合 | 不管理渲染 Pass、不管理 PSO、不管理描述符堆 |
| **PhysicsScene** | 碰撞体形状、物理材质、关节 | 不运行物理模拟（那是 PhysicsEngine 的迭代职责） |
| **AudioScene** | 音频发射器位置、声学区域、混响参数 | 不播放音频（那是 AudioEngine 的职责） |
| **NavMeshScene** | 导航网格、动态阻挡区域 | 不运行寻路算法 |

### 3.3 同步更新机制

子模块通过 ISceneModule 接口接收同步通知，而非事件驱动：

```cpp
// RenderScene 示例
// 注意：ISceneModule 接口使用 uint64_t 作为实体句柄，
// 内部实现中可以转换为 entt::entity 使用 ECS 能力
class RenderScene : public ISceneModule {
    void OnEntityAdded(uint64_t entity) override {
        auto e = static_cast<entt::entity>(entity);
        // 检查实体是否有 MeshComponent
        if (auto* mesh = m_registry->TryGet<MeshComponent>(e)) {
            // 创建渲染代理
            RenderProxy proxy;
            proxy.meshHandle = mesh->geometryHandle;
            proxy.materialHandle = mesh->materialHandle;
            proxy.transform = m_registry->Get<TransformComponent>(e).worldMatrix;
            m_proxies.push_back(proxy);
        }
        // 检查实体是否有 LightComponent
        if (auto* light = m_registry->TryGet<LightComponent>(e)) {
            m_lights.push_back(light->GetLightData());
        }
    }
    
    void OnEntityRemoved(uint64_t entity) override {
        // 移除对应的渲染代理
        auto it = std::remove_if(m_proxies.begin(), m_proxies.end(),
            [entity](const RenderProxy& p) { return p.entity == entity; });
        m_proxies.erase(it, m_proxies.end());
    }
};
```

### 3.4 为什么不走事件驱动

| 考量 | 事件驱动 | 同步广播 |
|:-----|:---------|:---------|
| 延迟 | 当前帧或下一帧才消费 | 同一帧立即生效 |
| 开销 | 入队/出队/调度 | 直接函数调用 |
| 代码复杂度 | 需要事件类型定义 + 事件注册 | 接口虚函数 |
| 适用场景 | 低频、跨线程、松耦合 | 高频、同线程、紧耦合 |

**结论**：实体/组件的增删改频率高（Transform 每帧可能变几千次），适合同步广播。异步资源加载完成后的事件（如 `GeneratorTaskCompleteEvent`）走事件驱动。

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
| **场景导出** | 从缓存的 EntityDesc 序列化为 `SceneDescription`（JSON 保存用） |
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
    └─ 保存时 ExportToDescription() 从 m_entityDescs 序列化
    
为什么 SceneManager 要持有 EntityDesc？
    - 场景文件导出时需要完整的实体描述（包括编辑后的属性）
    - ECS 组件是运行时高效表示，但缺少序列化所需的元数据
    - EntityDesc 是"可持久化的实体快照"，ECS 组件是"运行时活跃状态"
    - 两者双向同步：编辑 EntityDesc → 更新 ECS 组件；运行时变化 → 更新 EntityDesc（可选）
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
| **EditorScene** | 包装 SceneConstructor | 废弃，由 EditorSceneManager 替代 | 完全替换，旧文件待删除 |
| **EditorSceneManager** | 继承 SceneManager | 组合包装 SceneManager* | 取消继承，改为包装；移除 LoadDefaultScene/LoadSceneFile，不再负责加载编排 |
| **GameSceneManager** | 不存在 | 组合包装 SceneManager*（新建） | 与 EditorSceneManager 同模式，提供 RegisterSceneConstructSystem/LoadSceneAsync |
| **GameWorld** | 直接持有 SceneConstructor + Registry | 通过 GameSceneManager 访问场景 | 移除 m_sceneConstructor/m_asyncLoadDelay/m_asyncScenePath；m_registry 来自 GameSceneManager::GetRegistry() |
| **SceneConstructSystem** | 响应事件构造 ECS 实体 | 保留，但改为调用 SceneManager::CreateEntity + RegisterEntity | 逻辑不变，落点从直接 m_registry 改为 SceneManager 的受控流程 |
| **AssetBrowser** | 仅文件浏览 | 场景加载编排者 | 新增 LoadSceneDescription()，持有 SceneConstructor 生命周期 |
| **SharedDataStore** | 中转 SceneConstructData | 保留，但数据生命周期缩短 | 构造完成后不再保留，转入 SceneManager 的 EntityDesc 列表 |
| **ECS::Registry** | 实体存储，外部直接访问 | SceneManager 内部私有成员，不对外暴露 | 外部通过 SceneManager 的 EntityHandle API 操作实体；内部 System 通过 `GetRegistryForInternalUse()` 访问完整 ECS 能力 |
| **LightManager** | 管理光源 | 接入 RenderScene 子模块 | LightManager 的数据迁移到 RenderScene，或 LightManager 作为 RenderScene 的内部组件 |
| **GameContext** | 依赖注入容器，持有所有子系统指针 | 保留，但部分字段被高级抽象替代 | `Registry` 指针移除（归 SceneManager）；`FrameDriver`、`BackgroundExecutor` 等字段保留，但随 SceneManager 成熟逐步被其接口替代 |
| **SchedulerContext** | 线程局部上下文，持有 Registry + DeviceContext 等 | 废弃 | 实际无人使用（`CameraManager` 唯一的调用是绕路拿 deltaTime，可通过 `GameContext` 直接访问），随 SceneManager 落地后移除 |

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
          → SceneManager 内部追加到扁平实体列表 + 通知子场景模块
```

### 6.3 资源生命周期管理

场景切换时的 GPU 资源释放，由 SceneManager 驱动：

```
SceneManager::PrepareSceneSwitch("新场景", Immediate)
    │
    ├─ 遍历所有非持久化实体
    │     ├─ 收集实体引用的 GeometryHandle / MaterialHandle / TextureHandle
    │     └─ 记录到一个资源释放列表
    │
    ├─ 移除实体（ECS Destroy + 子场景通知）
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
| 子场景模块注册时机 | 编译期 vs 运行期 | **运行期 RegisterModule** | Game 和 Editor 注册不同的子场景模块组合 |
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
| **P1** | 实现 `RenderScene` 子模块（迁移 LightManager 的渲染代理数据） | P0 |
| **P1** | 场景加载职责分离：AssetBrowser 编排，EditorSceneManager 仅管理 | P0 |
| **P1** | 系统执行恢复：TaskGraphBuilder 中系统执行为空 lambda，需 SceneManager 重新调度 | P0 |
| **P2** | 场景切换的资源释放（GPU 资源回收） | P1 |
| **P2** | 实现 Undo/Redo 系统 | P1 EditorSceneManager |
| **P3** | 流式加载（StreamingLoader） | P2 GameSceneManager |
| **P3** | 多场景 Tab 编辑 | P2 EditorSceneManager |
| **P4** | `PhysicsScene` / `AudioScene` / `NavMeshScene` 子模块 | P1 子模块体系稳定 |