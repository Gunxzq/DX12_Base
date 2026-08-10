# 渲染项收集与绑定架构推进快照 (2026-08-03)

> 缓存分桶落地 + 频闪根因修复（调度器依赖缺陷）+ 渲染项统一定案 + 槽位全声明 + Constants32/CBV 数据源判别
> 关联：`Docs/architecture/rendering/RendererDataDriven.md`（§2.2 槽位全声明 / §2.2a 根签名数据驱动 / §4.1b/c/d 缓存分桶 / §4.2a1 渲染项统一 / §6 路线图）、`Docs/architecture/culling/OctreeCullingAndRaycaster.md`（CulledSet 粗筛）、`Docs/architecture/scene/SceneStateMachine.md`（§7.11 缓存属地）
> 待办：#22/#23/#24（材质槽链路前置，已完成）

---

## 一、渲染项收集架构：缓存表 + 分桶（§4.1b/c/d 落地）

### 1.1 设计定案（演进：舍弃材质组件模式）

| 层 | 2026-08-02（L1.5 材质组件化，§4.1a） | 2026-08-03（缓存分桶，§4.1b/c/d） |
|:---|:---|:---|
| 组件 | PBR/Skinned/Transparent 三材质组件（组件类型 = pass） | 通用 `RenderSlotComponent`（`Slot.shaderType` = 渲染器标记，字段值非组件类型） |
| 收集 | 各 Builder `view<MeshComponent + 自己的材质组件>` | `RenderSlotCache`：缓存表（实体→槽位，CRUD 驱动）+ 桶（shaderType→可见条目，每帧由 CulledSet × 缓存表派生） |
| 可见性 | Builder 内 FrustumCull | CulledSet = 八叉树粗筛候选；Builder 消费桶时**仍精筛**（FrustumCull + LOD），无 visible 标志 |

### 1.2 已落地改动

| 文件 | 改动 |
|:---|:---|
| `ECS/Core/Components/Render.h` | 移除三材质组件 + `materialSlots[]`；新增 `RenderSlot`（material + subMeshRanges + shaderType）+ `RenderSlotComponent` |
| `Scene/SceneConstructor.cpp` | 填充单个 `RenderSlotComponent`（shaderType = 渲染器标记）；实体 CRUD → `RenderSlotCache::MarkDirty()` |
| `Renderer/Core/RenderSlotCache.h/.cpp`（新增） | 缓存表（驻留，CRUD 驱动）+ 桶（每帧派生）；`Rebuild/MarkDirty/IsDirty/Dispatch/DispatchAll/ForEachBucket`；Entry 无 visible 标志 |
| `Boot/GameContext.h` / `Bootstrap.h/.cpp` | 挂 `RenderSlotCache` 单例；`SceneManager::SetRenderSlotCache` |
| `Scene/SceneManager.h/.cpp` | CreateEntity/RemoveEntity/RemoveAllEntities → MarkDirty |
| `Properties/Editors/MeshEditor.cpp` | 改材质槽 → MarkDirty |
| `Opaque/SkinnedRenderItemBuilder` | 消费缓存桶（子集），粗筛基础上精筛；移除 `m_entityFilter`（过滤保留到分发前） |
| `Editor.cpp/h`、`GameRenderPipeline.cpp/h` | BuilderUpload 每帧 `IsDirty→Rebuild` + `Dispatch`（Editor 用八叉树粗筛候选）/`DispatchAll`（Game 兜底） |
| `Transparent/Probe/MeshEditor` | 三个 `materialSlots` 消费者迁移到 `RenderSlotComponent` |

### 1.3 设计要点

- **缓存表与桶分离**：缓存表只在 ECS 实体增删查改时重建（MarkDirty → 每帧检查脏才 Rebuild）；桶每帧由 CulledSet × 缓存表派生（桶内容即粗筛可见）
- **CulledSet 仅八叉树粗筛**：Builder 消费桶时仍做精确筛选（FrustumCull + LOD），`m_frustum` 保留、`m_entityFilter` 移除
- **过滤保留到分发前**：编辑器端 `SceneTagComponent` 场景过滤在 `RenderSlotCache::Dispatch` 时执行（按 sceneId），Builder 保持无过滤

---

## 二、频闪根因与修复：调度器依赖缺陷（2026-08-03）

### 2.1 症状

RenderDoc 确认：相机不动时某些帧渲染实体为空（频闪）。

### 2.2 根因（双层缺陷）

| 缺陷 | 位置 | 后果 |
|:---|:---|:---|
| **依赖建立只认 `ThreadType::Any`** | `TaskExecutor::Execute/ExecutePhase` | Editor/Game 的分发、构建系统全为 `Worker`，`DependsOn` 完全失效 → 分发/构建并行执行 |
| **依赖数据从未到达执行器** | `TaskGraph::AddDependency` 写 `Node.dependencies`（set），`TaskExecutor` 却读 `Task::dependencies`（vector，从未填充） | 即使放宽线程类型，`succeed` 也从未建立 |

并行结果：`EditorBuildOpaque` 读到 `RenderSlotCache::Dispatch` 清空后/填充中途的桶 → 空帧 → 频闪。

### 2.3 修复

| 位置 | 改动 |
|:---|:---|
| `Scheduler/TaskGraph.h/.cpp` | 新增 `GetDependencies(TaskId)`（读 `Node.dependencies`） |
| `Scheduler/TaskExecutor.cpp` | `Execute` 与 `ExecutePhase` 两处依赖建立：放宽到 Any/Worker（跳过 Main/Render 专用队列）+ 改用 `graph.GetDependencies(id)` |

### 2.4 验证

日志确认：修复后 `afterDispatch`/`BuildTyped entry`/`afterBuild` 恒为 10（一致），不再出现 Build 读到 0/3/7 的空帧。临时 `[Diag]` 日志已移除。

---

## 三、渲染项统一定案：全可能大渲染项 + bindings 模式（§4.2a1，2026-08-03）

### 3.1 演进

§4.2a（2026-08-02"类型隔离"）→ §4.2a1（2026-08-03"全可能大渲染项"）：渲染项 = 资源的投影（ECS 组件 → 管理器资源 → 渲染项字段），**不必为每个 PSO/渲染器定义渲染项类型**。

### 3.2 核心模型

```cpp
// 全可能大渲染项 —— 取代 Opaque/Skinned/Transparent 类型隔离
struct RenderItem : RenderItemCommon {
    uint32_t probeIndex = UINT32_MAX;                // ReflectionConsumerComponent → 有效（无=无效）
    D3D12_GPU_VIRTUAL_ADDRESS boneBufferAddress = 0; // SkinnedComponent → 有效（无=无效）
    // ... 未来差异字段按需追加，均为"无效值缺省"
};
```

- **Builder 推断**：`TryGetComponent<SkinnedComponent/ReflectionConsumerComponent>` → 填字段；组件存续 = JSON 键存在
- **渲染器消费**：`ApplyBindList` 按字段有效性选择性绑定（**字段无效即不绑定**）
- **组合约束**：从编译期类型隔离移到注册期 `RendererPairing`（渲染项 ↔ 渲染器 ↔ 消费槽位清单）+ `itemKind` 兜底
- **对照 UE**：`FMeshDrawCommand`（统一结构）+ `FMeshDrawShaderBindings`（只记录实际绑定）

---

## 四、槽位全声明 + Constants32 + CBV 数据源判别（§2.2，2026-08-03）

### 4.1 槽位集 = 渲染项字段集

renderer.json `rootSignature.params` 应**全声明**可能槽位（**不限于 PassConstants**：LightConstants/WaterCB/BoneBuffer/BillboardTexture…），渲染器按字段有效性选择性消费（未声明槽位 `ApplyBindList` 跳过）。

### 4.2 字段 → 绑定形态三分类

| 字段性质 | 绑定形态 | 例 |
|:---|:---|:---|
| 资源句柄/地址（ECS 组件推断） | DescriptorTable / RootSRV / CBV | vb/ib/instanceBuffer/boneBuffer/materialBuffer/textureHeap |
| 结构化小数据（多个相关标量） | CBV（ObjectCB 等） | 世界矩阵、光源参数（LightManager） |
| 单标量/标志（≤4×32 位） | **Constants32 根常量** | materialIndex、probeIndex、receivesShadow、LOD 档位 |

### 4.3 Constants32 链路现状

`BindType::Constants32` + `consts[4]` + `numConstants` 已定义；PSO 工厂 JSON 解析 `"Constants32"` + `InitAsConstants` 已支持；**`ApplyBindList` 的 `case Constants32` 仍为 `break;`（待实现）**——实施时按物理索引查 `numConstants` 调用 `SetGraphicsRoot32BitConstants(phys, numConstants, consts, 0)`。

### 4.4 CBV 数据源判别（按渲染器需要选择，不强制归属）

- **随场景/资源状态变化** → 管理器提供（FrameResourceManager/LightManager/SkeletonManager/WaterManager），渲染器只消费——**管理器 PSO 的着色器已消费管理器参数（现状实证）**
- **渲染器/着色器专属配置**（后处理强度/阈值等）→ 管理器不介入：编译期固定走 renderer.json；每帧小参数走 Constants32 根常量；结构化临时数据走渲染器自持 CBV（FrameScratchAllocator）

---

## 五、路线图状态（§6）

| 档位 | 状态 |
|:---|:---|
| L1 材质路由 / L1.5 缓存分桶 | ✅ 完成 |
| L2 试点数据化（PSOFactory/BindSlot/渲染项自包含/根签名 JSON 化） | ✅ 大部分完成（StateCache 暂缓，见 §3.4 标注） |
| L2.5 渲染项统一 + JSON 槽位全声明（Step 4.5） | 📋 定案，待实施 |
| L3 横向铺开 / L4 收尾 | ⏳ 待做 |
