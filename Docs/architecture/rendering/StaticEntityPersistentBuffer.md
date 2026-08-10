# 静态实体持久化方案 v2（StaticComponent + 烘焙数据 + 长期 GPU 缓冲）

> 日期：2026-08-03
> 状态：方案定稿（待实施）
> 关联：`Docs/architecture/rendering/FrameResourceManager.md`（帧/临时分配器约束）、`RendererDataDriven.md` §4.1（缓存分桶）、`Engine/ECS/Core/Components/Misc.h`（StaticComponent 已定义未启用）

---

## 一、背景与历史教训

### 1.1 早期静态组件失败原因（资源丢失）

`StaticComponent` 在 `Misc.h` 中已定义但**未启用**（含 `persistentCBAddress`/`persistentInstanceAddress`/`worldDirty`/`cachedWorld` 等字段）。早期设计通过 `FrameResourceManager` 的 `AllocatePersistentObjectCB` / `AllocatePersistentInstanceBuffer` 分配持久资源，**该路径已移除**（见 `FrameResourceManager.md` §未来演进）。

**失败根因**：持久化资源（存活到组件销毁）被交给了**帧资源分配器 / 临时分配器**管理——RingBuffer 按"每帧分配 + N 帧后 Reclaim"回收，持久缓冲一旦被 Reclaim 即地址失效（资源丢失），静态组件缓存的世界矩阵/实例数据指向已释放内存 → 渲染异常/崩溃。

**结论（v2 铁律）**：
- ❌ **禁止**使用帧资源分配器（`FrameResourceManager` RingBuffer）管理静态实体持久资源
- ❌ **禁止**使用临时分配器（`FrameScratchAllocator`）管理静态实体持久资源
- ✅ **必须**使用长期存活的资源：独立 committed resource，存活到实体销毁

### 1.2 资源管理归属：GpuResourceManager 包装

`GpuResourceManager`（`Engine/Resource/GpuResourceManager.h`）满足"长期资源"要求：

```cpp
GpuResourceHandle CreateBuffer(ID3D12Device *device, size_t size, const std::wstring &name, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState, uint64_t initialFence);
ID3D12Resource *GetResource(GpuResourceHandle handle) const;
void Release(GpuResourceHandle handle, uint64_t completedFenceValue); // fence 延迟释放
```

- `CreateBuffer` 创建**独立 committed resource**（长期存活，非 RingBuffer）
- `GetResource` 按句柄取 `ID3D12Resource*`（上传/绑定用）
- `Release(handle, fence)` 由 fence 回调延迟释放（GPU 完成后回收），**不涉及句柄复用**
- 资源生命周期与实体生命周期绑定：实体销毁（`StaticComponent` 销毁）时 `Release`

**句柄系统考量**：`GpuResourceHandle` 是"创建→长期持有→fence 释放"的句柄，**不是复用池**。静态实体持久缓冲不需要句柄复用（实体数量稳定，创建一次持有到销毁），因此直接使用 `GpuResourceManager` 的长期句柄语义即可，无需引入新的句柄复用系统。

---

## 二、方案 v2 架构（三数据源）

```
┌─ ECS 组件层：StaticComponent（运行时状态）
│   ├─ persistentInstanceAddress（GpuResourceManager 长期缓冲 GPU 地址）
│   ├─ worldDirty / cachedWorld / cachedWorldInvTranspose（已有字段）
│   └─ persistentId（NameComponent，与烘焙数据 key 关联）
│
├─ 缓存表层：RenderSlotCache（已有，§4.1c）
│   └─ 实体 → 渲染槽位（材质 + 子网格区间），CRUD 驱动驻留
│
└─ 烘焙数据层：sceneEnvironment.precomputed（save 时生成）
    ├─ nonUniformScale: bool（save 时给定字段，标记是否非均匀缩放）
    ├─ instanceData[]: 按 persistentId 索引的 { world, worldInvTranspose }
    │     （worldInvTranspose 仅 nonUniformScale=true 时存储）
    └─ staticWorldBounds[]: 按 persistentId 索引的世界 AABB（剔除/八叉树直接消费）
```

### 2.1 persistentId 关联（烘焙数据 key）

- `EntityDesc.persistentId`：JSON 层实体标识（fnv1a 64-bit hex 字符串）
- `NameComponent.persistentId`：运行时标识（uint64_t，`SceneConstructor` 从 JSON 恢复，无 JSON ID 时 `NextPersistentId()` 分配）
- **烘焙数据必须按 persistentId 索引**，加载时通过 `persistentId → precomputed` 查表取回矩阵/AABB
- 若 `StaticComponent` 不存 persistentId，将无法在烘焙 key 表中定位实体数据——**persistentId 复用 NameComponent 字段即可，无需在 StaticComponent 重复存储**

### 2.2 非均匀缩放标记

- `precomputed.nonUniformScale`：save 时遍历静态实体，按 scale 三分量判定（|sx−sy|/max < ε 且 |sy−sz|/max < ε 为均匀），全场景统一标记
- `nonUniformScale=true`：烘焙 `worldInvTranspose`（16 float/实体）
- `nonUniformScale=false`：仅烘焙 `world`，加载时 `worldInvTranspose = world`（正交矩阵性质，省存储与计算）
- 参考大型引擎（UE GPUScene / Unity SRP Batcher）：**不逐实体跳过求逆**（判断成本与风险不成比例），以缓存 + 烘焙覆盖收益

### 2.3 场景动静比例参数

```
sceneEnvironment.entityMotionPolicy: "static" | "dynamic"
  → 未设置 StaticComponent 的实体按此默认分配（多数场景默认 static）
  → 显式组件覆盖场景默认
```

---

## 三、资源生命周期约束（v2 铁律）

| 资源 | 分配器 | 存活期 | 释放 |
|:--|:--|:--|:--|
| 静态实例持久缓冲 | **GpuResourceManager::CreateBuffer**（committed） | 实体存活期 | 实体销毁时 `Release(handle, fence)` |
| 动态实例数据 | FrameResourceManager RingBuffer | 每帧 | 3 帧 Reclaim（现有链路，不改） |
| 烘焙 JSON | 无 GPU 资源 | 文件 | 文件删除 |

**关键点**：
- 静态/动态实例数据**不共享缓冲**（生命周期不同，混用会导致 Reclaim 误释放）
- 静态缓冲**一次上传**（加载时由 precomputed memcpy），此后每帧零上传；`worldDirty` 时低频刷新单实例
- `GpuResourceManager::Release` 走 fence 回调（`GpuWorkItem::uploadBufferHandles` 机制或 `Update` 的 fence 回调），**不允许管理器直接调用 `GpuResourceManager::Release`**（见 .atomcode.md 规则 #11：资源管理器只释放自己的槽位）

---

## 四、实施步骤

```
Step 1：StaticComponent 启用
  - SceneLoader/SceneConstructor 解析 precomputed + nonUniformScale + entityMotionPolicy
  - 加载时按 persistentId 从 precomputed 取回矩阵/AABB → StaticComponent
  - 未标记实体按 entityMotionPolicy 补 StaticComponent

Step 2：precomputed 生成（save 时全量重算，无性能约束）
  - EditorSceneManager::SaveToFile 遍历静态实体：
      world / worldInvTranspose（按 nonUniformScale 决定是否存）
      世界 AABB → staticWorldBounds
  - 写入 sceneEnvironment.precomputed（按 persistentId 索引）

Step 3：持久化缓冲 + 每帧零重传
  - GpuResourceManager::CreateBuffer 分配静态实例缓冲（长期）
  - Builder/FrameSync：静态 batch 上传一次 + 每帧跳过（worldDirty 驱动刷新）
  - 八叉树/剔除消费 staticWorldBounds（加载即就绪）
```

---

## 五、运行时数据源职责（三来源确认）

| 数据源 | 内容 | 生命周期 |
|:--|:--|:--|
| ECS 组件（StaticComponent） | 运行时状态：持久缓冲地址、worldDirty、缓存矩阵 | 加载时写入，实体生命周期驻留 |
| 缓存表（RenderSlotCache） | 实体→渲染槽位（材质+子网格区间） | CRUD 驱动驻留（已有） |
| 烘焙数据（precomputed） | 静态实体 World/WorldInvTranspose/AABB（persistentId 索引） | 加载时一次消费 → 上传持久缓冲 |

**关系**：烘焙数据是静态实体的**基准值**（save 时固化）；ECS 组件是**运行时快照**（烘焙值 memcpy + worldDirty 追踪变化）；缓存表是**渲染分桶**（与变换数据正交）。

---

## 相关代码位置

- `Engine/ECS/Core/Components/Misc.h` — StaticComponent（L17-18，已定义未启用）
- `Engine/Resource/GpuResourceManager.h` — CreateBuffer/GetResource/Release（长期 committed + fence 释放）
- `Engine/Asset/IO/Loader/SceneDescription.h` — SceneEnvironment（L526）、SceneMetadata（L19）、EntityDesc.persistentId（L445）
- `Engine/Asset/IO/Loader/SceneLoader.cpp` — metadata/precomputed 解析点
- `Engine/Scene/SceneConstructor.cpp` — persistentId 恢复（L540-542）
- `Engine/Renderer/RenderItemBuilder/OpaqueRenderItemBuilder.cpp` — 每帧矩阵求逆（L119）
- `Editor/EditorLib/Core/Editor.cpp` — FrameSync_EditorUploadInstanceData（每帧全量上传）
- `Editor/EditorLib/Scene/EditorSceneManager.cpp` — SaveToFile / InitCameraConfig
