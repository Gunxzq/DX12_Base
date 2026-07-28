# BugFix: RTV 描述符槽位跨标签复用导致 rtvSlot 溢出

> 日期：2026-07-26
> 涉及文件：`RenderTargetPool.cpp`、`AmbientOcclusionManager.h/.cpp`、`Editor.cpp`

---

## 现象

编辑器调整视口大小时，若干次 resize 后 `CreateRenderTarget` 返回 `GetRtvHandle(sceneColor) returned null`，视口渲染崩溃，死循环重试。

## 日志

```
[RenderTargetPool::GetRtvHandle] generation mismatch: entry=1, handle=0, poolIndex=0, rtvSlot=1023
```

`rtvSlot=1023` 远超 EditorViewport RTV 分区的大小（64 槽）。

## 根因

### 1. 初始化时序问题

编辑器启动时，`AmbientOcclusionManager` 在 `EditorViewport` 之前初始化：

```
Editor::Initialize()
  → aoMgr.Initialize(..., HeapTag::Default)      // SSAO RT → tag=Default
  → EditorViewport::Initialize()
      → m_appRTs->Initialize(..., EditorViewport) // G-buffer RT → tag=EditorViewport
```

SSAO 用 `HeapTag::Default` 分配 RT。`HeapTag::Default` 没有独立的 RTV 分区，`RenderTargetPool::Allocate(Default, Rtv)` 走 `DescriptorHeapCollection::Allocate` 的 **Fallback 路径**（第 266-270 行），落到全局 CBV_SRV_UAV 堆的分配器上，返回大索引（1023/1022）。

### 2. FindMatchingEntry 跨标签复用

当 EditorViewport 的 `AllocateGBuffer` 分配 G-buffer RT 时，`FindMatchingEntry` 找到了 SSAO 的空闲条目（`!inUse`、尺寸/格式匹配），**不检查 `heapTag`**，直接复用。返回的 handle 带有错误的 `rtvSlot=1023`。后续 `GetRtvHandle` 用该 handle 到 EditorViewport 的 RTV 分区（64 槽）查找，generation 不匹配导致失败。

### 3. 修复链路

| 修复 | 文件 | 改动 |
|:-----|:------|:------|
| SSAO 接收 HeapTag | `AmbientOcclusionManager.h` | `Initialize` 新增 `heapTag` 参数（默认 `Default`） |
| 存储 tag | `AmbientOcclusionManager.cpp` | 新增 `m_heapTag` 成员，`BuildResources` 中 `rtvPool.Allocate(desc, m_heapTag)` |
| Editor 端传正确 tag | `Editor.cpp` | `aoMgr.Initialize(...)` 传入 `HeapTag::EditorViewport` |
| FindMatchingEntry 防复用已清理条目 | `RenderTargetPool.cpp` | 增加 `entry.rtvSlot != UINT32_MAX` 检查 |

### 4. 待处理的其他管理器

以下管理器也使用 `HeapTag::Default` 分配池资源，但在 Editor 多堆模式下应使用 `EditorViewport` tag：

| 管理器 | 分配调用 | 当前 tag |
|:-------|:---------|:---------|
| `LightManager` | `dsPool.Allocate(dsDesc)` ×2, + `Default` ×1 | `Default` |
| `ReflectionProbeManager` | `dsPool.Allocate(..., Default)`, `rtPool.Allocate(..., Default)` | `Default` |

当前它们和 SSAO 同理——如果初始化的时序在 EditorViewport 之前（或之后被 `FindMatchingEntry` 复用），同样会导致跨 tag 的 rtvSlot 错乱。修复方式与 `AmbientOcclusionManager` 一致：加 `HeapTag` 参数，Editor 端传入 `EditorViewport`。

---

## 相关代码位置

- `Engine/Resource/Pool/RenderTargetPool.cpp` — `FindMatchingEntry`、`CreateNewEntry`、`PurgeUnused`
- `Engine/Resource/Core/DescriptorHeapCollection.cpp` — `Allocate` Fallback 路径（第 266-270 行）
- `Engine/Resource/Core/DescriptorSlotAllocator.cpp` — `AllocateLinear`、`Expand`、`maxCapacity` 限制
- `Engine/Renderer/Effects/AO/AmbientOcclusionManager.cpp` — SSAO RT 分配（已修复）
- `Engine/Renderer/Scene/LightManager/LightManager.cpp` — 阴影 RT 分配（待修复）
- `Engine/Renderer/Scene/ReflectionProbeManager/ReflectionProbeManager.cpp` — 探针 RT 分配（待修复）
