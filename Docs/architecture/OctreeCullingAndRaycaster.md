# 八叉树空间划分 + 剔除管线 + Raycaster 统一架构

> 日期：2026-07-25
> 状态：设计方案，待实施

---

## 一、当前架构的问题

### 1.1 剔除系统不真正剔除实体

`CullingSystem` 目前只计算视锥体（Frustum）的数学参数，**不遍历实体做实际的剔除测试**：

```
CullingSystem::SetCamera()
    → 构建 m_cullFrustum（宽远平面）
    → 构建 m_renderFrustum（紧远平面）
    ← 结束，不做任何实体级别的测试
```

真正剔除的是各 `OpaqueRenderItemBuilder` 内部的视锥测试（`Frustum::Contains`），这是分散在每个 builder 内的重复劳动。

### 1.2 Builder 层的场景过滤重复

Editor 端通过 `OpaqueRenderItemBuilder::SetEntityFilter` 传入一个 lambda，按 `SceneTagComponent::sceneId` 过滤实体。**每个 builder 都做一次**：

```
Editor.cpp 每帧：
  m_opaqueBuilder->SetEntityFilter([activeSceneId](Entity e) {
      return tag && tag->sceneId == activeSceneId;
  });
  m_skinnedBuilder->SetEntityFilter([...](Entity e) { ... });  // 重复
  m_terrainBuilder->SetEntityFilter([...](Entity e) { ... });  // 重复
  m_waterBuilder->SetEntityFilter([...](Entity e) { ... });    // 重复
```

### 1.3 VisibleRaycaster 无场景过滤 + 暴力遍历

`VisibleRaycaster::CollectHits`：
- 遍历 **全部** `PickingComponent + TransformComponent` 实体
- 不做场景过滤（Editor 端会选中非活跃 Tab 的实体）
- 无空间加速结构（O(n) 暴力遍历）

### 1.4 FrameDriver 预留了阶段

```cpp
// PreCulling / PostCulling：保留阶段钩子供后续八叉树、遮挡查询等使用
ExecutePhase(TaskPhase::PreCulling);
ExecutePhase(TaskPhase::PostCulling);
```

这两个阶段目前为空，正是为八叉树剔除准备的。

---

## 二、提议架构

### 2.1 数据处理管线

```
PreCulling 阶段:
  八叉树空间划分（预计算，回退或增量更新）
    ↓
  从八叉树中提取"视锥相交"的候选实体集
    ├── Editor: 候选集已含 SceneTagComponent 信息
    │            只保留 activeSceneId 匹配的实体
    └── Game:   候选集全局，无场景过滤

PostCulling 阶段:
  视锥剔除（在当前候选集上做精确 frustum-sphere/AABB 测试）
    ↓
  可见集（CulledSet）:
    ├── 供给 Builder（直接消费，不再需要 SetEntityFilter）
    ├── 供给 Raycaster（在可见集上做射线相交测试）
    └── 供给 LODSystem / OcclusionSystem 等
```

### 2.2 组件关系

```
                ┌──────────────────────┐
                │    OctreeSystem      │  ← 新增，PreCulling
                │  八叉树空间划分       │
                └──────────┬───────────┘
                           │ 候选实体列表（含 sceneId）
                           ▼
                ┌──────────────────────┐
                │   CullingSystem      │  ← 扩展，PostCulling
                │  视锥剔除 + 场景过滤  │
                └──────────┬───────────┘
                           │ 可见集（最终）
                           ▼
          ┌────────────────┴────────────────┐
          │                                 │
          ▼                                 ▼
  ┌─────────────────┐             ┌──────────────────┐
  │   Builder 层     │             │ VisibleRaycaster │
  │ 不用 SetEntityFilter│          │ 从可见集做射线测试│
  │ 不用 SceneTag过滤 │             │ 不再遍历 Registry│
  └─────────────────┘             └──────────────────┘
```

### 2.3 与现有架构的兼容

| 现有组件 | 改动 |
|:---------|:------|
| `CullingSystem` | 扩展：接收 OctreeSystem 的候选集，做精确视锥剔除 + 场景过滤，输出可见集 |
| `VisibleRaycaster` | 改造：不再遍历 Registry，改为从可见集/候选集做射线测试 |
| `OpaqueRenderItemBuilder` | 移除 `m_entityFilter`/`SetEntityFilter`，直接消费可见集 |
| `SkinnedRenderItemBuilder` | 同上 |
| `TerrainRenderItemBuilder` | 同上 |
| `WaterRenderItemBuilder` | 同上 |
| `TransparentRenderItemBuilder` | 同上 |
| `Editor.cpp` | 移除 4 处 `SetEntityFilter` 调用 |
| `SceneTagComponent` | 保留（用于 Tab 生命周期），但 Builder 不再感知 |
| `FrameDriver` | PreCulling 阶段插入 OctreeSystem |

---

## 三、影响范围评估

### 3.1 需新增

| 文件 | 估算工作量 |
|:-----|:-----------|
| `Engine/Renderer/Core/OctreeSystem.h/.cpp` | 中等（八叉树构建 + 更新 + 查询 + 候选集输出） |

### 3.2 需修改

| 文件 | 改动量 | 说明 |
|:-----|:-------|:------|
| `Engine/Renderer/Core/CullingSystem.h/.cpp` | 中 | 增加候选集输入、可见集输出、场景过滤逻辑 |
| `Engine/Renderer/Core/VisibleRaycaster.h/.cpp` | 小 | 增加一个 `SetCulledSet(CulledSet*)` 接口，`CollectHits` 改为从候选集遍历 |
| `Engine/Renderer/RenderItemBuilder/OpaqueRenderItemBuilder.h/.cpp` | 小 | 移除 `m_entityFilter`/`SetEntityFilter` 相关代码（2 处遍历 + 1 个成员变量） |
| `Engine/Renderer/RenderItemBuilder/TransparentRenderItemBuilder.h/.cpp` | 小 | 同上（需确认是否也有 filter） |
| `Engine/Renderer/RenderItemBuilder/SkinnedRenderItemBuilder.h/.cpp` | 小 | 同上 |
| `Engine/Renderer/RenderItemBuilder/TerrainRenderItemBuilder.h/.cpp` | 小 | 同上 |
| `Engine/Renderer/RenderItemBuilder/WaterRenderItemBuilder.h/.cpp` | 小 | 同上 |
| `Editor/EditorLib/Core/Editor.cpp` | 小 | 移除 ~20 行 `SetEntityFilter` 回调代码 |
| `Engine/Scheduler/FrameDriver.cpp` | 小 | PreCulling 阶段注册 OctreeSystem |

### 3.3 需确认

| 项目 | 说明 |
|:-----|:------|
| 八叉树增量更新 | 实体移动时需增量更新，而非每帧重建。与 ECS `OnUpdate` 事件结合 |
| PickingComponent 处理 | 当前 `VisibleRaycaster` 依赖 `PickingComponent` 过滤。候选集是否也需携带此信息？ |
| 可见集的数据结构 | 是 `std::vector<Entity>` + sceneId 映射，还是更复杂的结构 |
| 双端差异 | Editor 端按 sceneId 过滤 + 视锥剔除；Game 端仅视锥剔除 |
| 构建器并行化 | 各 builder 当前可并行构建，改为消费共享可见集是否影响并行度 |

---

## 四、实施建议

### 阶段一：八叉树 + 可见集（基础）

1. 实现 `OctreeSystem`（PreCulling）：构建静态八叉树 + 视锥查询 → 输出候选集
2. 扩展 `CullingSystem`（PostCulling）：对候选集做精确剔除 → 输出可见集
3. 保留 `SetEntityFilter` 作为兼容接口，逐步迁移

### 阶段二：Builder 层清理

1. 逐个 builder 移除 `m_entityFilter`，改为直接消费 `CullingSystem` 的可见集
2. 从 `Editor.cpp` 移除 `SetEntityFilter` 回调

### 阶段三：Raycaster 迁移

1. 改造 `VisibleRaycaster`：增加候选集输入接口
2. 添加 `SceneTagComponent` 过滤（Editor 端在候选集中已过滤，raycaster 不再重复）
3. 测试 Editor 端视口点击选中实体

---

## 五、相关文件清单

```
Engine/Renderer/Core/
  ├── OctreeSystem.h/.cpp        [新增]
  ├── CullingSystem.h/.cpp       [修改]
  ├── VisibleRaycaster.h/.cpp    [修改]
  └── CulledSet.h/.cpp           [新增，可见集数据结构]

Engine/Renderer/RenderItemBuilder/
  ├── OpaqueRenderItemBuilder.h/.cpp       [修改：移除 m_entityFilter]
  ├── TransparentRenderItemBuilder.h/.cpp  [修改：移除 m_entityFilter]
  ├── SkinnedRenderItemBuilder.h/.cpp      [修改：移除 m_entityFilter]
  ├── TerrainRenderItemBuilder.h/.cpp      [修改：移除 m_entityFilter]
  └── WaterRenderItemBuilder.h/.cpp        [修改：移除 m_entityFilter]

Engine/Scheduler/
  └── FrameDriver.cpp             [修改：PreCulling 注册]

Editor/EditorLib/Core/
  └── Editor.cpp                  [修改：移除 SetEntityFilter]

Docs/
  └── architecture/OctreeCullingAndRaycaster.md  [本文]
```

---

## 六、大型引擎参考

### 6.1 Unreal Engine 5

**八叉树归属**：`FPrimitiveOctreeSemantics` — 八叉树由渲染线程（`FScene`）拥有，**不是游戏线程**。游戏线程以 1 帧延迟将实体位置写入渲染线程。

**剔除管线**：
```
CPU 粗剔除（PrePass）
  ├── 视锥剔除 — 遍历 PrimitiveOctree，提取与视锥相交的节点
  ├── 遮挡查询 — Hierarchical Z-Buffer（HZB）预计算
  └── 输出 Primitive 可见集

GPU 细剔除（Nanite）
  └── Cluster hierarchy BVH8 → Persistent Threads → 逐 cluster 视锥+遮挡测试
```

**选中/Picking**：渲染线程通过 `FScene` 的八叉树做 HitProxy 渲染（将选中 ID 编码为颜色值渲染到 off-screen buffer，然后读回像素），**不走 CPU 射线检测**。

**与我们的差异**：
- 八叉树所有权归属渲染线程，非共享。我们的架构中剔除在 PreRender 阶段，渲染在 Render 阶段，天然有 1 帧延迟，与 Unreal 的分离模式一致。
- 选中方案：编辑器端可考虑未来用 GPU picking（渲染选中 ID 到 off-screen buffer），当前先用 CPU raycaster。

### 6.2 Unity

**空间划分**：`CullingGroup API` — 基于 bounding sphere 数组，由引擎统一管理剔除。开发者只需提供 sphere 数组 + 相机，引擎在相机剔除时自动计算可见性和距离区间。对开发者不暴露八叉树细节。

**选中/Picking**：`HandleUtility.RegisterRenderPickingCallback` — 注册回调，在 Unity 渲染 picking pass 时写入 `selectionId` 到 picking render texture。同样是 **GPU 方案**。

**ECS 下的场景管理**：`SceneSystem` — 场景是独立的 `SceneData`，加载时流式读入。场景实体共存在一个 World 中，但通过 `SceneTag` 区分。与我们的 `SceneTagComponent` + 多 Tab 架构一致。

### 6.3 对我们的启示

| 引擎实践 | 我们的对应 | 对齐度 |
|:---------|:-----------|:-------|
| 渲染线程独享八叉树 | FrameDriver 的 PreCulling/PostCulling + 1 帧延迟 | ✅ 一致 |
| SceneTag 区分多场景实体 | `SceneTagComponent` + `sceneId` | ✅ 一致 |
| GPU picking 替代 CPU 射线 | 当前 CPU `VisibleRaycaster`，未来可扩展 | ⚠️ 当前合理 |
| 八叉树统一服务渲染+选中+物理 | 当前各系统各自遍历 Registry | ❌ 需改进，正是本文方向 |
| 候选集在剔除层过滤场景 | 当前 Builder 层 `SetEntityFilter` 重复过滤 | ❌ 需改进 |

**核心结论**：我们的架构方向与大型引擎一致——八叉树+剔除管线+可见集作为共享数据源，各系统（builder、raycaster）消费同一份数据，不再各自遍历 Registry。大型引擎也没有"SceneManager 包含 raycaster"的模式，两者是协作关系。

---

## 七、线程化方案可行性分析

### 7.1 目标

八叉树的构建（`Build`/`RebuildAsync`）不阻塞主线程，在后台线程执行。

### 7.2 双缓冲模型

```
后台线程（BackgroundExecutor/Job）       主线程（FrameDriver Tick）
─────────────────────                   ─────────────────────
TreeB.Build(entities)                   TreeA.QueryFrustum(frustum)
  │ (完全隔离，不碰 TreeA)                │ (只读遍历 TreeA)
  ▼                                      ▼
TreeB 构建完成                            TreeA 仍在查询
  │                                      │
  ▼                                      ▼
atomic 指针交换 (TreeA ↔ TreeB)  O(1)
  │                                      │
  ▼                                      ▼
旧 TreeA → 延迟释放队列                   新 TreeA.QueryFrustum(...)
（下一帧开始前销毁，确保旧查询已结束）
```

### 7.3 安全条件

| 条件 | 状态 | 说明 |
|:-----|:------|:------|
| 查询只读 | ✅ | `QueryFrustum` 遍历节点不修改 |
| 构建隔离 | ✅ | 后台构建的是独立的根节点，不碰主线程的树 |
| 指针交换原子 | ✅ | `std::atomic<OctreeNode*>` 双槽轮换 |
| 旧树延迟释放 | ✅ | `DeferredDeletionQueue` 推迟到下一帧 |
| 实体增删一致性 | ⚠️ 可接受 | 后台构建期间新增/删除的实体，下一帧构建自然对齐 |

### 7.4 与现有调度器架构的契合

FrameDriver 已有 `ExecutePhase(TaskPhase::PreCulling)`，且剔除结果天然延迟一帧。这意味着八叉树有**一整帧的时间**在后台构建，不会影响主线程：

```
帧 N: PreCulling → 提交异步 Build 任务
帧 N: 主线程继续 Update → PreRender → Render（使用帧 N-1 构建的树）
帧 N+1: PreCulling → 后台 Build 已完成 → atomic_swap → 使用新树查询
```

### 7.5 粒度优化方向（"大空间盒"）

参考 `cull.md` 的"网格划分 + 八叉树"混合方案：

```
世界 → 粗分网格（Grid，16m³ ~ 64m³）
    ├── 静态网格单元 → 预计算八叉树（不变时无需重建）
    └── 动态网格单元 → 运行时松散八叉树（每帧增量更新）
```

这样动静分离后，后台构建只处理动态单元，静态单元复用上一次的结果，大幅降低构建频率。

### 7.6 Raycaster 抽象层

`VisibleRaycaster` 将改造为可见集驱动，不再直接遍历 `ECS::Registry`：

```
Raycaster 抽象层（容器）
  ├── 数据源：CulledSet（来自剔除管线）
  ├── ScreenToRay：不变
  ├── RaycastOnSet(culledSet, ray)：在可见集上做精确相交测试
  └── 不负责：场景过滤、输入状态检测

Game 端：传入 GameWorld 的可见集
Editor 端：传入 EditorViewport 的可见集（已按 sceneId 过滤）
```

这样双端共享同一套射线检测逻辑，数据源由调用方（Game/Editor）各自提供。
