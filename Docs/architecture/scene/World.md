# World — ECS 绝对源头

> 日期：2026-07-23
> 状态：📋 新设计
> 关联：`SceneManager.md`、`EngineOverview.md §8.7`、`ECS/Registry.h`
> 后续：`ECS/World.h`（接口定义）、`Bootstrap` 装配、各端接入

---

## 1. 核心概念

### 1.1 什么是 World

**World 是 ECS 的绝对源头和唯一容器。** 所有实体都必须属于某个 World，所有组件查询都通过 World 的 Registry 进行。World 本身不感知任何逻辑分组，不提供任何业务语义——它只是"实体存在的地方"。

```
World（绝对 ECS 源头）
  ├─ 持有 ECS::Registry（unique_ptr，唯一所有权）
  ├─ 实体生命周期入口（CreateEntity / DestroyEntity）
  ├─ 组件读写（GetComponent / AddComponent / RemoveComponent）
  └─ 不感知任何逻辑分组，不提供任何业务语义
```

### 1.2 设计原则

| 原则 | 说明 |
|:-----|:------|
| **绝对源头** | 所有实体必须属于某个 World，不存在游离于 World 之外的实体 |
| **单一职责** | World 只做"实体存在"这件事，不管理场景、不管理预览、不管理调试 |
| **不分区** | World 本身不分区，不感知 SceneTag/PreviewTag/DebugTag——这些是 Manager 的视角 |
| **轻量** | World 是一个薄层，核心逻辑委托给 ECS::Registry |
| **可扩展** | 任何模块都可以通过 World 创建实体，不需要经过 SceneManager |

### 1.3 与 ECS::Registry 的关系

```
World 是 ECS::Registry 的所有者和封装层：

  World
    ├─ 持有 unique_ptr<ECS::Registry>
    ├─ 控制实体创建/销毁的入口
    ├─ 提供受控的组件访问（与 SceneManager 当前模式一致）
    └─ 内部 System 通过 GetRegistry() 访问完整 enTT 能力

  ECS::Registry
    ├─ enTT 的运行时容器
    ├─ view/group/ctx 等高级特性保留给内部 System
    └─ 不对外暴露（通过 World 的 API 操作）
```

---

## 2. 接口设计

### 2.1 核心接口

```cpp
// Engine/ECS/World.h — Engine CORE

namespace DX12Engine::ECS {

/// World — ECS 绝对源头
///
/// 设计原则：
///   - 所有实体必须属于某个 World
///   - World 本身不分区，不感知任何逻辑分组
///   - 逻辑分区由 Manager 的"视角"提供（SceneManager、PreviewManager 等）
///   - 内部 System 通过 GetRegistry() 访问完整 ECS 能力
class World {
public:
    World() = default;
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // ====================================================================
    // 初始化/销毁
    // ====================================================================

    void Initialize();
    void Shutdown();

    // ====================================================================
    // 实体生命周期（唯一入口）
    // ====================================================================

    /// 创建空实体，返回 Entity handle
    Entity CreateEntity();

    /// 批量创建空实体
    std::vector<Entity> CreateEntities(uint32_t count);

    /// 销毁实体
    void DestroyEntity(Entity entity);

    /// 销毁所有实体
    void RemoveAllEntities();

    /// 检查实体是否有效（未被销毁）
    bool IsValid(Entity entity) const;

    // ====================================================================
    // 组件访问（受控）
    // ====================================================================

    template<typename T>
    T* GetComponent(Entity entity) {
        return m_registry->TryGetComponent<T>(entity);
    }

    template<typename T, typename... Args>
    T& AddComponent(Entity entity, Args&&... args) {
        return m_registry->AddComponent<T>(entity, std::forward<Args>(args)...);
    }

    template<typename T>
    void RemoveComponent(Entity entity) {
        m_registry->RemoveComponent<T>(entity);
    }

    template<typename T>
    bool HasComponent(Entity entity) const {
        return m_registry->HasComponent<T>(entity);
    }

    // ====================================================================
    // 内部 System 访问
    // ====================================================================

    /// 内部 System 通过此方法访问完整的 ECS 能力（view/group/ctx 等）
    /// 外部消费者禁止直接调用
    Registry* GetRegistry() { return m_registry.get(); }

protected:
    std::unique_ptr<Registry> m_registry;
};

} // namespace DX12Engine::ECS
```

### 2.2 与当前 SceneManager 接口的映射

| 当前 SceneManager 方法 | 迁移后归属 | 说明 |
|:-----------------------|:-----------|:------|
| `CreateEntity()` | → `World::CreateEntity()` | 实体创建归 World |
| `CreateEntities()` | → `World::CreateEntities()` | 批量创建归 World |
| `RegisterEntity()` | 已移除 | World 创建即管理，不再需要注册步骤 |
| `RemoveEntity()` | → `World::DestroyEntity()` | 实体销毁归 World |
| `RemoveAllEntities()` | → `World::RemoveAllEntities()` | 全量清理归 World |
| `GetComponent<T>()` | → `World::GetComponent<T>()` | 组件访问归 World |
| `GetRegistry()` | → `World::GetRegistry()` | 内部 System 访问 |
| `PersistEntity()` | 已移除 | Editor 端用 SceneTagComponent 替代 |
| `GetAllEntities()` | 已移除 | 清单由 EditorSceneManager 按 Tag 查询维护 |
| `SetSkybox()` | → `SceneManager::SetSkybox()` | 环境状态仍归 SceneManager |
| `PrepareSceneSwitch()` | → `SceneManager::PrepareSceneSwitch()` | 场景切换由 SceneManager 编排 |

---

## 3. 单一 World + 逻辑分区

### 3.1 架构

```
Engine Core（绝对 ECS 源头）：
  World（单一实例）
    ├─ 持有 ECS::Registry
    ├─ 所有实体都必须属于这个 World
    ├─ 实体生命周期入口（CreateEntity / DestroyEntity）
    └─ World 本身不分区，不感知任何逻辑分组

逻辑分区（由 Manager 的解释器视角提供）：
  ├─ SceneManager 的视角：
  │     └─ 查询所有带 SceneTagComponent 的实体
  │     └─ 按 sceneId 分组 → 多 Tab 场景管理
  │     └─ 负责这些实体的序列化/反序列化
  │
  ├─ PreviewManager 的视角：
  │     └─ 查询所有带 PreviewTag 的实体
  │     └─ 管理预览实体的创建/销毁
  │     └─ 不参与场景保存
  │
  ├─ DebugManager 的视角：
  │     └─ 查询所有带 DebugTag 的实体
  │     └─ 管理调试可视化实体的创建/销毁
  │     └─ 不参与场景保存
  │
  └─ 其他 Manager 同理
        └─ 每个 Manager 通过 TagComponent 查询自己的"逻辑分区"
```

### 3.2 逻辑分区 vs 多 World 实例

| 对比 | 单一 World + 逻辑分区（选择） | 多 World 实例（排除） |
|:-----|:-----------------------------|:---------------------|
| **Registry 数量** | 1 个 | N 个 |
| **跨分区交互** | 直接（同一 Registry，直接 Entity 交互） | 需要跨 Registry 桥接 |
| **选中任意实体** | 直接选中，不关心来自哪个分区 | 需跨 World 查询 |
| **Builder 渲染** | 一次性遍历所有实体 | 需合并多个 World 的渲染项 |
| **渲染管线** | 不需要区分"来自哪个 World" | 每个 World 需独立遍历 |
| **Game 端形态** | 无分区概念，所有实体都是游戏内容 | 单一 World，无区别 |
| **实现复杂度** | 低（Registry 不变，Tag 区分） | 高（多实例管理、跨 World 交互） |

**选择单一 World 的理由：**

1. **跨分区交互简单** — 选中预览实体 = 选中同一 World 的实体，不需要跨 World 查询
2. **Builder 不需要合并** — 所有实体在同一 Registry，一次性 view 遍历即可
3. **渲染管线不需要区分"来自哪个 World"** — 同一管线处理所有实体
4. **Game 端不需要任何分区概念** — 所有实体都是"游戏内容"，不需要 Tag 区分
5. **SceneTagComponent 已实现** — 当前 Editor 端已经用 SceneTagComponent 做多 Tab 过滤，模式一致

### 3.3 Game 端 vs Editor 端

```
Game 端：
  └─ 单一 World，无逻辑分区
  └─ 所有实体都是"游戏内容"，不需要 Tag 区分
  └─ SceneManager 只做关卡加载/卸载
  └─ 不需要 PreviewTag/DebugTag 等

Editor 端：
  └─ 单一 World，多个 Manager 提供多个逻辑分区
  └─ SceneManager → SceneTagComponent 视角
  └─ PreviewSystem → PreviewTag 视角
  └─ DebugSystem → DebugTag 视角
  └─ 各 Manager 通过 TagComponent 查询自己的"逻辑分区"
  └─ 跨分区交互（如选中预览实体）直接通过 Entity handle 完成
```

---

## 4. World 与 SceneManager 的关系

### 4.1 组合关系

```
World（ECS 源头，Engine Core）          SceneManager（场景序列化器，Engine Core）
  ├─ 持有 Registry          ├─ 持有 World* 引用
  ├─ CreateEntity           ├─ LoadScene(path) → world.CreateEntity()
  ├─ DestroyEntity          ├─ SaveScene(path) → 查询 → 写入
  └─ GetRegistry            ├─ 环境状态：Skybox/Environment
                            └─ 场景切换：PrepareSceneSwitch

Editor 端特化：
  EditorSceneManager（逻辑分区 + 多 Tab 管理）
    ├─ 包装 SceneManager
    ├─ 通过 SceneTagComponent 查询场景实体集合
    ├─ 多 Tab 管理
    └─ SceneSnapshot 快照

Game 端特化：
  GameSceneManager（关卡加载/切换）
    ├─ 包装 SceneManager
    ├─ 单场景加载/卸载
    └─ 不需要 SceneTagComponent
```

### 4.2 职责边界

| 职责 | 归属 | 说明 |
|:-----|:------|:------|
| **实体创建/销毁** | World | 唯一入口，所有模块通过 World 创建实体 |
| **组件读写** | World | 受控访问，内部 System 通过 GetRegistry() 获取完整能力 |
| **场景序列化** | SceneManager | JSON ↔ ECS 的桥梁，从 World 查询实体后写入文件 |
| **环境状态** | SceneManager | 天空盒、环境光、光照等场景级参数 |
| **场景生命周期** | SceneManager | 加载、卸载、切换、叠加 |
| **逻辑分区**（Editor 端） | EditorSceneManager | 通过 SceneTagComponent 提供"场景实体"的视角 |
| **多 Tab 管理**（Editor 端） | EditorSceneManager | 多个场景文件同时打开，Tab 切换切换 sceneId |
| **非场景实体** | 各 Manager 自行管理 | 预览实体、调试实体直接在 World 创建/销毁 |

---

## 5. 数据流

### 5.1 场景加载

```
SceneManager::LoadScene(path)
  │
  ├─ SceneLoader::LoadFromFile → SceneDescription
  ├─ AssetManager::LoadBatch 加载依赖
  ├─ 遍历 entityDesc:
  │     ├─ Entity entity = World::CreateEntity()
  │     ├─ SceneConstructor::ConstructEntity(entity, desc, ...)
  │     └─ 添加 SceneTagComponent(entity, sceneId)
  │
  └─ 设置环境状态（Skybox/Environment）
```

### 5.2 非场景实体创建（预览系统）

```
PreviewSystem::CreatePreviewEntity()
  │
  ├─ Entity entity = World::CreateEntity()
  ├─ registry->AddComponent<MeshComponent>(entity, ...)
  ├─ registry->AddComponent<PreviewTag>(entity)
  └─ 不经过 SceneManager
```

### 5.3 逻辑分区查询

```
// SceneManager 的视角：查询当前场景的所有实体
auto view = world.GetRegistry()->view<SceneTagComponent>();
for (auto entity : view) {
    auto &tag = view.get<SceneTagComponent>(entity);
    if (tag.sceneId == currentSceneId) {
        // 属于当前场景的实体
    }
}

// PreviewManager 的视角：查询所有预览实体
auto previewView = world.GetRegistry()->view<PreviewTag>();
for (auto entity : previewView) {
    // 预览实体
}
```

---

## 6. 迁移路径

| 步骤 | 内容 | 影响 |
|:----:|:------|:------|
| 1 | 从 Engine Core 中提取 `World` 类，持有 `ECS::Registry` | 新增，不破坏现有代码 |
| 2 | `SceneManager` 移除 `m_registry`，改为持有 `World*` | SceneManager.h/.cpp 修改 |
| 3 | `SceneManager::CreateEntity`/`RemoveEntity` 委托给 `World` | 接口兼容，内部重定向 |
| 4 | `Bootstrap` 创建 `World` 实例，传给 `SceneManager` | Bootstrap.cpp 修改 |
| 5 | 逐步替换 `SceneManager::GetRegistry()` 调用为 `World::GetRegistry()` | 各调用点替换 |
| 6 | `SceneManager` 降级为场景序列化器 + 逻辑分区解释器 | 职责收窄，代码重构 |
| 7 | Editor 端接入 `World`，各 Manager 通过 TagComponent 提供逻辑分区 | Editor 端渐进 |

### 6.1 向后兼容策略

迁移过程中保持接口兼容：

```cpp
// 过渡期：SceneManager 同时提供旧接口和新接口
class SceneManager {
    // 旧接口（内部委托给 World）
    uint64_t CreateEntity() { return m_world->CreateEntity(); }
    void RemoveEntity(uint64_t entity) { m_world->DestroyEntity(static_cast<Entity>(entity)); }

    // 新接口
    World* GetWorld() const { return m_world.get(); }
};
```

---

## 7. 设计决策记录

| 决策 | 选项 | 选择 | 理由 |
|:-----|:-----|:-----|:------|
| **World 数量** | 单一 World vs 多 World 实例 | **单一 World + 逻辑分区** | 跨分区交互简单，Builder 不需要合并多个 Registry，渲染管线统一 |
| **World 归属** | Engine Core vs Editor/Game | **Engine Core** | 所有实体都需要 World，必须放在核心层 |
| **Registry 所有权** | World 持有 vs 共享指针 | **World 持有 unique_ptr** | 唯一所有权，World 销毁时 Registry 自动销毁 |
| **实体创建入口** | World 创建 vs 模块自行创建 | **World 统一创建** | 生命周期可追踪，未来可加拦截器（网络同步、回放） |
| **逻辑分区实现** | World 内部维护分区 vs Manager 自行查询 | **Manager 通过 TagComponent 自行查询** | World 不耦合分区逻辑，分区策略由各 Manager 决定 |