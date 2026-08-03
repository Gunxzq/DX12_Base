# BugFix: RTV 槽位泄漏 + Generation 截断导致 resize 崩溃

> 日期：2026-07-26
> 涉及文件：`RenderTargetPool.cpp`、`ApplicationRenderTargets.cpp`、`EditorViewport.cpp`、`DescriptorHeapCollection.cpp`

---

## 一、问题现象

编辑器调整视口大小时，若干次 resize 后视口渲染崩溃（`ResourceBarrier: NULL pointer`），最终 GPU TDR。

## 二、根因 1：RTV 槽位泄漏

### 2.1 分配器永不重用（已修复）

`DescriptorHeapCollection::AddPartition` 默认分配器已从 `LinearAlloc | EnableExpand` **改为纯 FreeList**（`slotFlags = 0`）：

- FreeList 模式下 `Free` 将索引推回 `m_freeIndices`，`Allocate` 从 `m_freeIndices` 复用
- 不再有槽位泄漏，也不再有 generation 截断风险
- `slotFlags` 参数保留，供未来特殊需求显式覆盖

`DescriptorHeapCollection::AddPartition` 中 RTV 分区使用 `LinearAlloc | EnableExpand` 模式：

```cpp
allocatorConfig.flags = DescriptorSlotFlags::LinearAlloc | DescriptorSlotFlags::EnableExpand;
```

`LinearAlloc` 模式下的 `AllocateLinear` 只递增 `m_nextIndex++`，**从不复用已释放的槽位**。即使 `Free` 将槽位归还给分配器，`Allocate` 也不使用它。配合 `EnableExpand`，分配器在 `m_freeIndices` 为空时会扩容（64→128→256→...），槽位永远不会被真正回收。

### 2.2 Free 不释放描述符槽位

`RenderTargetPool::Free` 仅设置 `inUse=false`，**不释放 RTV/SRV 描述符槽位**。槽位释放由 `PurgeUnused` 负责（每 60 帧触发一次，检查 `frameNumber - lastUsedFrame > 120`）。

### 2.3 修复

| 改动 | 文件 | 说明 |
|:-----|:------|:------|
| `AddPartition` 增加 `slotFlags` 参数 | `DescriptorHeapCollection.h/.cpp` | 允许调用方自定义分配器 flags |
| RTV 分区禁用 `LinearAlloc` | `EditorViewport.cpp` | 改用 FreeList 模式，释放的槽可被分配器重用 |
| RTV 分区禁用 `EnableExpand` | `EditorViewport.cpp` | 固定 256 槽，耗尽时触发 LRU 淘汰 |
| `EvictLRU` 用 fence=0 释放 | `RenderTargetPool.cpp` | 释放的槽立即回到 `m_freeIndices`，无需等 Reclaim |
| `Allocate` 中 `CreateNewEntry` 失败后调用 `EvictLRU` 重试 | `RenderTargetPool.cpp` | 槽位不足时淘汰最久未使用的条目 |
| `oldRtvValid` 在 `DestroyRenderTarget` 前保存 | `EditorViewport.cpp` | 避免 `FreeGBuffer` 清空 handle 后验证永远失败 |

---

## 三、根因 2：Generation 截断

### 3.1 8 位位域截断

`RenderTargetHandle` 中 `generation` 字段只有 **8 位**：

```cpp
struct RenderTargetHandle {
    uint32_t poolIndex : 12;
    uint32_t generation : 8;  // ← 只能存 0-255！
    uint32_t rtvSlot : 12;
};
```

当 `m_nextGeneration` 超过 255 时，赋值 `handle.generation = entry.generation`（如 461）**静默截断**为 `461 & 0xFF = 205`。而 `GetRtvHandle` 以完整 `uint32_t` 比较 `entry.generation（461） != handle.generation（205）` → **永远不匹配**。

### 3.2 触发条件

m_nextGeneration 每创建一个 entry 递增 1。每次 resize 创建 5 个 entry（4 G-buffer + 1 sceneColor）。约 51 次 resize 后（256÷5），m_nextGeneration 超过 255，此后所有 `GetRtvHandle` 调用均返回 null。

### 3.3 修复

| 改动 | 文件 | 说明 |
|:-----|:------|:------|
| `GetRtvHandle` 比较时 mask 低位 | `RenderTargetPool.cpp` | `(entry.generation & 0xFF) != handle.generation` |

---

## 四、相关代码位置

- `Engine/Resource/Struct/DescriptorHandle.h` — `RenderTargetHandle` 结构体（8 位 generation 位域）
- `Engine/Resource/Core/DescriptorSlotAllocator.cpp` — `AllocateLinear`、`AllocateFromFreeList`、`Expand`
- `Engine/Resource/Core/DescriptorHeapCollection.h/.cpp` — `AddPartition`、`Allocate`（Fallback 路径）
- `Engine/Resource/Pool/RenderTargetPool.h/.cpp` — `Allocate`、`Free`、`PurgeUnused`、`EvictLRU`、`FreeSlots`
- `Engine/Renderer/ApplicationRenderTargets.h/.cpp` — `FreeGBuffer`、`AllocateGBuffer`
- `Editor/EditorLib/Scene/EditorViewport.cpp` — `CreateRenderTarget`、RTV 分区配置

---

## 五、修复后的数据流

```
OnResize(newWidth, newHeight)
  ↓
CreateRenderTarget(width, height)
  ↓
DestroyRenderTarget()          → 清空 m_rtvHandle
  ↓
m_appRTs->OnResize()           → FreeGBuffer + AllocateGBuffer
  │     ↓
  │   FreeGBuffer：rtPool.Free(handle, completedFence) → inUse=false
  │   AllocateGBuffer：rtPool.Allocate(desc, m_heapTag)
  │     ├─ FindMatchingEntry → 找到 inUse=false 且尺寸匹配的 → 复用（REUSE）
  │     └─ CreateNewEntry → 分配器有槽 → 成功（CREATE）
  │                         分配器无槽 → EvictLRU → 淘汰最旧空闲条目的槽 → 重试成功
  ↓
m_rtvHandle = GetRtvHandle(sceneColor)
  ↓
  GetRtvHandle: entry.generation(461) & 0xFF = 205 == handle.generation(205) → OK
  ↓
渲染系统使用 m_rtvHandle → 视口正常渲染
```

---

## 六、RTV/DSV 池的正确使用场景

经过此次追踪，明确了 `RenderTargetPool` 和 `DepthStencilPool` 的设计定位：

### 适用场景（走池）

- **尺寸无关的持久资源**：阴影贴图（独立分辨率）、反射探针（固定立方体贴图）
- **高频切换资源**：光源切换时同参数 shadow map 复用（`FindMatchingEntry` 命中）
- **不依赖窗口尺寸**的 RT/DS，尺寸固定，不会随 viewport resize 变化

### 不适用场景（不走池，应直接 `CreateCommittedResource`）

- **依赖窗口尺寸的离屏 RT**：G-buffer、sceneColor、SSAO、深度缓冲
- resize 时尺寸变化 → `FindMatchingEntry` 永不命中 → 每次 `CREATE` → 旧槽泄漏
- 应像主交换链一样阻塞重建：`DestroyRenderTarget` → `CreateCommittedResource` → 分配描述符

### 设计原则

```
窗口尺寸相关 RT (G-buffer, sceneColor, depth, SSAO) → 直接管理，不经过池
    └── resize 时释放旧资源、创建新资源，阻塞等待 GPU 空闲

尺寸无关的资源 (阴影贴图、探针、采样器) → 走池
    └── 利用 FindMatchingEntry 在参数匹配时复用，减少分配次数
```

### 关于 `RenderTargetHandle.generation`

`generation` 只有 **8 位位域**（最大 255），当 `m_nextGeneration` 超过 255 时，
`handle.generation = entry.generation` 静默截断。
`GetRtvHandle` 比较时应以 `c(entry.generation & 0xFF) != handle.generation` 而非全值比较。

---

## 七、G-buffer RTV 槽位 Phantom Free 导致四张 RT 垃圾值

### 现象

RenderDoc 显示 G-buffer 四张 RT 在 OpaqueSystem 写入后数据正确，但 LightingSystem 光照合成后只有漫反射（albedo）正确，normal/material/worldPos 为垃圾值。

### 根因

`EditorViewport` 的 `m_gbufferRtvSlots[4]` 和 `m_gbufferSrvSlots[4]` 初始化为 `{}`（全 0）。

首次 `CreateRenderTarget` 调用时，`DestroyRenderTarget()` 先执行，循环检查 `m_gbufferRtvSlots[i] != UINT32_MAX`：
- 0 ≠ UINT32_MAX → 条件为真 → 调用 `Free(0)` 四次
- 槽位 0 从未被分配，被误加入 `m_freeIndices` 四次（phantom copy）

之后 `CreateRenderTarget` 的 RTV 分配从 `m_freeIndices` pop_back 时：
- G-buffer[0] 分到 phantom 0
- G-buffer[1] 分到 phantom 0  
- G-buffer[2] 分到 phantom 0
- G-buffer[3] 分到 phantom 0

**四张 G-buffer RT 共用同一个 RTV 描述符槽位**，最后一次 `CreateRenderTargetView` 覆盖前一次，最终所有 RTV 均指向最后一张纹理（`m_gbuffer[3]`）。OpaqueSystem 的四路 RT 写入实际只写入了同一张纹理，验证层报 `RTV slot 1 overlaps with slot 2`。

### 修复

| 修复 | 文件 |
|:-----|:------|
| `m_gbufferRtvSlots[4] = {UINT32_MAX,...}` | `EditorViewport.h` |
| `m_gbufferSrvSlots[4] = {UINT32_MAX,...}` | `EditorViewport.h` |

所有槽位数组初始化为 `UINT32_MAX`，首次 `DestroyRenderTarget` 的 `!= UINT32_MAX` 检查正确跳过尚未分配的槽位。

---

## 八、相关代码位置

- `RenderTargetHandle.generation` 位域宽度可考虑扩到 10 位（与 `GeometryHandle` 一致），减少 wrap 频率
- `ApplicationRenderTargets::FreeGBuffer` fence 值改为从外部传入的 `completedFence` 而非硬编码

---

## 九、后续重构方向：WindowFrameResources

### 9.1 背景

`ApplicationRenderTargets` 设计本意是管理窗口尺寸相关的离线 RT，但实现中走 `RenderTargetPool`，
与 §六 中明确的"窗口尺寸相关 RT 不走池"原则相悖。

### 9.2 重构方案

将 `ApplicationRenderTargets` 替换为 **`WindowFrameResources`**（引擎 CORE 层），
采用 §六 确定的直接管理模式（类似 `EditorViewport` 的自管理方式）：

| 新类 | 职责 |
|:-----|:------|
| `WindowFrameResources` | 集中管理所有依赖窗口尺寸的 GPU 资源（G-buffer、SceneColor、DepthStencil 等），统一处理 resize 时安全重建 |

### 9.3 变化

| 旧 (`ApplicationRenderTargets`) | 新 (`WindowFrameResources`) |
|:------|:------|
| 资源经 `RenderTargetPool` 分配 | 直接 `CreateCommittedResource` + `DescriptorHeapCollection::Allocate` |
| resize 通过池的延迟释放 | resize 时**阻塞式**释放旧资源 + 创建新资源 |
| 只管理 G-buffer + SceneColor | 统一管理所有窗口尺寸资源（含 DepthStencil、日后扩展的 SSAO RT 等） |
| 命名偏应用层 | 命名中立，引擎 CORE 层 |

### 9.4 影响模块

| 模块 | 改动 |
|:-----|:------|
| `Engine/Renderer/ApplicationRenderTargets.h/.cpp` | 替换为 `WindowFrameResources.h/.cpp` |
| `Game/Game/RenderPipeline/GameRenderPipeline.h/.cpp` | 改用 `WindowFrameResources` |
| `Editor/EditorLib/Scene/EditorViewport.h/.cpp` | 改用 `WindowFrameResources`，消除自管理 RT 的重复逻辑 |

详见 `Docs/architecture/rendering/WindowFrameResources.md`。
