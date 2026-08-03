# 事件系统与数据层架构

> 日期：2026-07-06
> 关联：`EventSystem.md`（blog）、`AssetDataManager`、`ECS/Core`

---

## 一、四层架构总览

```
┌─────────────────────────────────────────────────────────┐
│  L4 应用层 — Systems（你写的业务逻辑）                    │
│  状态机、ECS Systems、GameWorld                          │
├─────────────────────────────────────────────────────────┤
│  L3 调度层 — DAG + 任务桶                                │
│  依赖图、优先级调度、协程挂起-唤醒                         │
├─────────────────────────────────────────────────────────┤
│  L2 数据层                                              │
│  ├── EnTT Registry（ECS = 游戏世界数据层）                │
│  │   实体、组件（Transform、Health、MeshComponent……）     │
│  └── SharedDataStore（事件系统数据层）                    │
│      跨线程数据中转站（后台线程 ⇄ 主线程）                 │
├─────────────────────────────────────────────────────────┤
│  L1 通信层 — Message Arena                               │
│  SoA 流（Type/Sender/Payload(64bit)/Timestamp）          │
│  无锁队列、优先级桶                                      │
└─────────────────────────────────────────────────────────┘
```

### 核心原则

- **ECS（EnTT Registry）** = 游戏世界的数据层，存储实体和组件（位置、血量、MeshHandle 等）
- **SharedDataStore**（原 `AssetDataManager`）= 事件系统的数据层，是跨线程数据中转站
- **Message Arena**（64-bit Payload）= 通信载体，高位标识数据类型，低位存句柄/key

---

## 二、数据流：异步加载 → 事件通知 → ECS

```
后台线程                         主线程
  │                                │
  ├─ CPU 加载文件                   │
  ├─ 创建 GPU 资源                  │
  ├─ 录制 COPY+DIRECT 命令          │
  │                                │
  ├─ StoreTypedData<T>(key, data)   │
  │   (写入 SharedDataStore)        │
  │                                │
  ├─ PostEvent(LoadedEvent) ───────→  Message Arena
  │      payload.high = type        │
  │      payload.low  = key         │
  │                                │
  │                    Tick() 收集事件
  │                    │
  │                    ├─ GetTypedData<T>(key)
  │                    │   (从 SharedDataStore 读取)
  │                    ├─ 创建/更新 ECS 组件
  │                    └─ RemoveTypedData(key)
```

### 64-bit Payload 布局

```
Payload (uint64_t)
├── 高位 (32 bit): 数据类型标识 / 事件子类型
└── 低位 (32 bit): SharedDataStore 的 key / CpuResourceHandle
```

### 关键约束

| 层次 | 职责 | 不做什么 |
|:-----|:------|:---------|
| **ECS** | 存储游戏世界的稳定数据 | 不存跨线程临时数据 |
| **SharedDataStore** | 跨线程传递加载中的临时数据 | 不存游戏逻辑数据 |
| **Message Arena** | 通知事件发生 | 不存完整数据体，只存 64-bit 引用 |
| **GpuResourceManager** | 管理 GPU 资源生命周期 | 不管理 CPU 端临时数据 |

### SharedDataStore 的存储语义

SharedDataStore 是一个**临时 LRU 缓存**，不是永久存储。它的核心语义是：

| 属性 | 说明 |
|:-----|:------|
| **存储内容** | 不限。CPU 数据、GPU 句柄、网络包、System 间大对象均可存放 |
| **生命周期** | 临时。写入 → 消费方取走 → 淘汰（LRU）或取出时清理 |
| **适用场景** | 事件系统传递大对象（64-bit payload 放不下）、网络系统接收大对象、异步加载中转 |
| **淘汰策略** | LRU 优先（支持多个 System 依赖同一份数据），也可取出时清理 |

不应当存入的数据只有一种：**ECS 的游戏世界数据**（组件数据应存在于 EnTT Registry 中，不经过中转站）。

#### 数据流示例

```
后台线程                            主线程
  │                                   │
  ├─ cpuWork: 加载文件 + 创建 GPU      │
  │   StoreTypedData<Result>(key, data)│  ← 存入中转站（临时）
  ├─ gpuWork: 录制命令                 │
  │   PostEvent(Ready, payload=key) ──→│
  │                                   │   System 响应事件
  │                                   ├─ GetTypedData<Result>(key)
  │                                   ├─ 用数据写 ECS 组件 / 注册到渲染系统
  │                                   └─ 数据被 LRU 淘汰或手动清除
```

当前 `TerrainLoadTask` 将 `TerrainGPUResult` 存入中转站是合理的——因为没有高层消费者（SceneConstructor 等）尚未就绪，暂时放在这里，后续接入消费系统后自然被取走淘汰。

---

## 三、当前实现对照

| 架构概念 | 当前实现 | 状态 |
|:---------|:---------|:----:|
| L1 通信层 | `MessageDispatcher` + `EventRegistry` | ✅ 基础实现 |
| L2 数据层 — ECS | `entt::registry` + ECS Components | ✅ 已拆分 |
| L2 数据层 — 中转站 | `AssetDataManager`（需改名） | ✅ 已有 `StoreTypedData<T>` / `GetTypedData<T>` |
| 异步加载三段式 | `LoadTask` + `BackgroundExecutor::SubmitLoadTask` | ✅ 方向 A+C 完成 |
| 方向 B 改造 | 地形加载改用 `StoreTypedData` 替代 `shared_ptr` lambda capture | ❌ 待实施 |
| 方向 D 改造 | `AssetManager` 统一加载管线 | ◐ 骨架 |

### 方向 B 的具体改造点

当前 `TerrainLoadTask::gpuWork` 中通过 `shared_ptr<TerrainReadyState>` lambda capture 传递结果：

```cpp
// 当前（shared_ptr lambda capture）
auto readyState = std::make_shared<TerrainReadyState>();
// ... 填充 readyState ...
return item;  // GpuWorkItem.onComplete 中持有 readyState
```

改造后走 SharedDataStore：

```cpp
// 改造后（SharedDataStore 中转）
auto key = "terrain_" + std::to_string(requestId);
AssetDataManager::GetInstance().StoreTypedData<TerrainGPUResult>(key, result);

// onComplete 中 PostEvent，payload 携带 key
PostEvent(TerrainReady, {.payload = keyHash});

// 主线程 System 响应事件
auto data = AssetDataManager::GetInstance().GetTypedData<TerrainGPUResult>(key);
// 创建 ECS 组件
// RemoveTypedData(key)
```

---

## 四、改名分析：`AssetDataManager` → ?

### 问题

当前名称 `AssetDataManager` 暗示它是"资产管理器"，但它的原始设计目的是**事件系统的多线程安全数据中转站**，并非专门的资产管理器：

| 实际用途 | 名称暗示 | 矛盾 |
|:---------|:---------|:-----|
| 跨线程传递加载结果 | 资产管理 | 网络通信也用，与资产无关 |
| StoreTypedData\<T\> 泛型存储 | 特定于资产 | 模板泛型，不限资产类型 |
| 配合事件系统 64-bit payload | 数据管理 | 它不管理资产生命周期（那是 GpuResourceManager 的职责） |

### 候选名称

| 名称 | 理由 | 风险 |
|:-----|:-----|:-----|
| **`SharedDataStore`** | 准确描述"线程安全共享数据存储" | 略长 |
| **`DataExchange`** | 强调数据在线程间交换 | 可能让人联想到网络数据交换 |
| **`DataHub`** | 简洁，数据中心 | 语义较泛 |
| **`TransientStore`** | 强调数据是临时的（处理后移入 ECS） | 不够直观 |

### 建议

方向：`AssetDataManager` → `SharedDataStore`，清晰表达"多线程安全的键值数据存储"这一核心职责。
