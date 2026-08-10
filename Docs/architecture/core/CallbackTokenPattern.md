# 回调注册令牌（RAII Token）模式与改造方案

> 状态：设计定案（文档先行，代码后续处理）
> 日期：2026-08-07
> 关联：`EventSystemAndDataLayer.md`、`RenderSlotCache`（§4.1b/c/d）、`AssetPreviewSystem.md`

## 1. 背景与问题本质

观察者模式的核心安全问题是**生命周期不一致**：

```
模块B（数据源）持有 -> 回调对象（捕获模块A的 this）
模块A（观察者）销毁 -> 模块B仍持有指向模块A的回调
后果：模块B触发回调 -> 访问已销毁的模块A -> use-after-free
```

回调的生命周期由数据源（B）管理，观察者（A）的生命周期由自己管理。两者必须通过某种机制对齐，否则任何一方先销毁都是崩溃点。

## 2. 方案对比（社区实践归纳）

| 方案 | 自动化 | 性能开销 | 侵入性 | 适用场景 |
| :--- | :--- | :--- | :--- | :--- |
| **RAII 注册令牌**（最推荐） | 高（析构自动注销） | 极低 | 低（持有一个 ID） | 通用，常驻模块间 |
| **弱引用包装**（weak_ptr + lock） | 最高（无需手动） | 中（每次 lock） | 高（需 shared_ptr 管理） | 低频、生命周期不可控 |
| **析构主动注销** | 中（依赖调用点正确） | 极低 | 低 | 简单系统 |

本引擎选型：**RAII 注册令牌**。理由：

- 引擎模块生命周期长、关系明确，"显式连接"优于"自动探测"
- 低频离散事件对 lock() 开销不敏感，但 `weak_ptr` 要求观察者对象全部 shared_ptr 托管，侵入性过大
- 令牌方案与既有计划（System 显式移除）语义一致，可复用同一套回调管理基座

### 令牌核心接口

```cpp
// 数据源侧（模块B）
class DataSource {
public:
    struct Token {                    // RAII：析构自动注销
        DataSource* owner = nullptr;
        uint64_t id = 0;
        bool valid = true;
        ~Token() { if (valid && owner) owner->Unregister(id); }
        Token(const Token&) = delete;
        Token& operator=(const Token&) = delete;
        Token(Token&& rhs) noexcept : owner(rhs.owner), id(rhs.id), valid(rhs.valid) {
            rhs.valid = false;        // 转移后原令牌不再注销
        }
    };
    Token Register(std::function<void()> cb);   // 返回令牌
    void Unregister(uint64_t id);               // 内部按 id 删除
private:
    std::unordered_map<uint64_t, std::function<void()>> m_callbacks;
    uint64_t m_nextId = 0;
};
```

## 3. 现状核查（2026-08-07 代码库实测）

### 3.1 三套通信机制的泄漏评估

| 机制 | 位置 | 是否持有回调 | 泄漏风险 |
| :--- | :--- | :--- | :--- |
| 消息池 | `Engine/Event/MessageDispatcher` + `MessageArena` | ❌ 纯数据 | 无（环形缓冲固定 32MB，帧末重置） |
| System 注册 | `Engine/Framework/SystemRegistry` | ✅ `std::function`（捕获裸 this） | 当前无（初始化一次 + Shutdown 时 Clear，生命周期配对） |
| 显式脏标记 | `RenderSlotCache::MarkDirty` | ❌ 无注册表 | 无（SceneManager CRUD 直接调用） |

**结论：当前代码库无实际泄漏。** 现有回调全部生命周期配对；消息池纯数据无回调面。

### 3.2 存在的防御性缺口

1. `SystemRegistry::Register` 无去重：同名/同消息重复注册会向 `s_messageToSystems` 累积重复 ID（当前靠"只注册一次"约定规避）
2. `SystemRegistry` 无 `RemoveSystem(id)`，只有全量 `Clear()`——未来动态 System（角色状态机）需要显式移除
3. `ConfigManager::Subscribe` 有注册无注销 API，且**全代码库无调用方**（休眠代码，启用前必须配套 token 或 Unsubscribe）

## 4. 改造点 A：预览回调（PreviewManager）

### 4.1 现状

- `Editor/EditorLib/Preview/PreviewManager.h:50` — `SetRenderCallback(PreviewRenderCallback)` 存储**单槽** `std::function<void(PreviewId, PreviewContext&)>`
- `Editor/EditorLib/Panels/AssetBrowser.cpp:393` — `RegisterPreviewRenderCallback()` 注入 `[this]` lambda（捕获 AssetBrowser 的 this）
- `PreviewManager.cpp:155` — `RenderPreviews()` 每帧对 needsRender 槽位直接调用 `m_renderCallback(slot.id, slot.ctx)`
- `Shutdown()`（PreviewManager.cpp:35）将 `m_renderCallback` 置 nullptr

### 4.2 问题

- **单槽覆盖**：`SetRenderCallback` 第二次调用静默覆盖第一次，无错误提示
- **无注销接口**：面板若可销毁（编辑器重建、面板动态开关），`m_renderCallback` 悬垂
- 当前 AssetBrowser 是 Editor 常驻成员，无实际泄漏；属防御性改造 + 为多订阅者铺路（AnimationViewportPanel 等未来预览消费方）

### 4.3 改造设计

```cpp
// PreviewManager.h
class PreviewManager {
public:
    struct RenderToken {  // 复用 §2 的 RAII 令牌
        PreviewManager* owner = nullptr;
        uint64_t id = 0;
        bool valid = true;
        ~RenderToken() { if (valid && owner) owner->UnregisterRenderCallback(id); }
        RenderToken(const RenderToken&) = delete;
        RenderToken& operator=(const RenderToken&) = delete;
        RenderToken(RenderToken&& rhs) noexcept { /* 转移语义同 §2 */ }
    };

    RenderToken RegisterRenderCallback(PreviewRenderCallback cb); // 取代 SetRenderCallback
    void UnregisterRenderCallback(uint64_t id);
    // RenderPreviews() 改为遍历 m_renderCallbacks（unordered_map<uint64_t, PreviewRenderCallback>）

private:
    std::unordered_map<uint64_t, PreviewRenderCallback> m_renderCallbacks;
    uint64_t m_nextCallbackId = 1;
};
```

- 单槽→多槽：语义升级为"渲染回调集合"，每帧遍历调用；单槽是集合大小为 1 的特例
- AssetBrowser 持有 `std::optional<PreviewManager::RenderToken> m_renderToken`（或 unique_ptr），`RegisterPreviewRenderCallback()` 改为 `m_renderToken = m_previewMgr->RegisterRenderCallback(...)`；面板析构自动注销
- `Shutdown()` 时 `m_renderCallbacks.clear()`（强制清空兜底）

### 4.4 迁移范围

- `PreviewManager.h/.cpp`：单槽改令牌多槽
- `AssetBrowser.cpp:393` `RegisterPreviewRenderCallback()`：持有令牌成员
- `PreviewRenderSystem`（Editor.cpp:976，SystemRegistry 注册，遍历预览上下文）无需改动——它只调用 `RenderPreviews()`

## 5. 改造点 B：ECS 数据源（SceneConstructor / SceneManager）

### 5.1 现状

- `Engine/Scene/SceneManager.cpp:50-91`：`CreateEntity / RemoveEntity / RemoveAllEntities` 中直接调用 `m_renderSlotCache->MarkDirty()`——**显式调用，无回调注册**
- `Engine/Scene/SceneConstructor.cpp:133`：`LoadBatch(assets, nullptr, [this]() { OnDependenciesLoaded(); })`——onAllComplete 回调**捕获裸 this**
- `SceneConstructor.h:129`：`Callback m_onComplete`——一次性完成回调

### 5.2 评估结论：按子项拆分处理

| 子项 | 形态 | 是否需要令牌 | 理由 |
| :--- | :--- | :--- | :--- |
| SceneManager CRUD → MarkDirty | 显式调用 | ❌ **保持现状** | 无回调注册即无泄漏；改造反而引入不必要间接层 |
| SceneConstructor::LoadBatch onAllComplete | 捕获裸 this 的一次性回调 | ✅ 防御性改造 | 若场景切换时旧 SceneConstructor 未完成加载即被销毁，回调悬垂 |

### 5.3 SceneConstructor 改造设计（cancel 语义，轻量令牌）

SceneConstructor 由 GameWorld/Editor 常驻持有（代码注释已声明"生命周期足够长"），实际触发概率低；采用**轻量有效性标志**而非完整令牌：

```cpp
// SceneConstructor.h
class SceneConstructor {
public:
    void CancelPendingLoad();   // 场景切换/析构时调用：m_loadValid = false

private:
    bool m_loadValid = true;    // 当前加载批次是否仍有效
    uint64_t m_loadGeneration = 0; // 可选：代数递增，防旧回调晚到
};

// LoadScene 中：
auto generation = m_loadGeneration;
m_batch = AssetManager::GetInstance().LoadBatch(assets, nullptr, [this, generation]() {
    if (generation != m_loadGeneration) return;  // 旧批次回调晚到：丢弃
    OnDependenciesLoaded();
});
```

- 捕获 `this` + generation（而非仅 `this`）：代数校验可区分"同一实例的新旧批次"，比单纯 bool 更稳
- `CancelPendingLoad()` 由场景切换路径（`EditorSceneManager::ClearScene` / Game 关卡切换）调用
- `m_onComplete` 同理：若实例销毁前未触发，析构时置空回调即可（`~SceneConstructor` 中 `m_onComplete = nullptr`、`m_batch.reset()`）

### 5.4 不做的事（明确边界）

- **不改造** SceneManager 的显式 MarkDirty 链路
- **不引入** ECS 生命周期事件系统（当前"变更方显式调用"是定案，见 RenderSlotCache 头文件注释"无 ECS 生命周期事件系统"）
- **不引入** 全局事件总线

## 6. 落地计划（后续处理，非本次）

1. **阶段 1（预览）**：PreviewManager 单槽 → 令牌多槽；AssetBrowser 持有令牌；验证 PreviewRenderSystem 正常
2. **阶段 2（ECS 数据源）**：SceneConstructor 增加 `m_loadGeneration` + `CancelPendingLoad()`；场景切换路径接入
3. **阶段 3（顺带防御）**：SystemRegistry 增加 `RemoveSystem(SystemId)`（三表同步清理：s_systems / s_nameToId / s_messageToSystems）+ Register 同名去重；为未来动态 System（角色状态机显式移除）铺路
4. **阶段 4（休眠代码）**：ConfigManager::Subscribe 增加 Unsubscribe 或标注"未启用"

## 7. 关键结论

- 当前代码库**无实际回调泄漏**；令牌改造是防御性 + 面向未来的多订阅者/动态 System 能力
- 令牌模式落地顺序：预览（单槽→多槽）→ ECS 数据源（generation 校验）→ SystemRegistry 显式移除 → ConfigManager 休眠代码
- 原则重申：关系明确的常驻模块间用显式连接（令牌/直接调用），**不引入全局事件总线**
