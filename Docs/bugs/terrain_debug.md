# 地形渲染形变问题：根因分析

## 现象

地形渲染在首帧正常，后续帧出现几何形变（顶点置换异常），表现为曲面细分因子错乱导致的网格变形。

## 根本原因

问题由**三个独立因素**叠加导致，按影响程度排序：

---

### 1. 帧时序冲突：RingBuffer 过早回收（主要原因）

**问题链路：**

```
帧 N-1: PreRender → BuildRenderQueue() → Allocate(RingBuffer, fence=N-1) → 上传常量
帧 N:   BeginFrame(nextFence=N) → Reclaim(fence≤N) → 回收帧 N-1 的 RingBuffer 空间
帧 N:   PreRender → BuildRenderQueue() → Allocate(RingBuffer, fence=N) → 覆盖旧数据
帧 N:   Render → 读取帧 N-1 构建的队列 → 但 GPU 地址已被帧 N 的分配覆盖 → 读到垃圾数据
```

**关键错误：**
- `BuildRenderQueue()` 在 **PreRender** 阶段分配 RingBuffer 并上传地形常量
- Render 阶段读取的是 **上一帧** PreRender 构建的队列
- `BeginFrame(nextFence)` 用当前帧 fence 调用 `Reclaim()`，过早回收了上一帧的 RingBuffer 空间
- GPU 读取到被覆盖的曲面细分参数（TessellationFactor, TessellationDistanceMin/Max）→ 形变

**修复：**
- 将 TerrainManager 改为 **LightManager 模式**：在 `Immediate` 回调中完成所有分配和上传
- `UpdateAndUpload(nextFence)` 在 Render 之前执行，分配+上传使用当前帧 fence
- Render 阶段直接使用当帧分配的 GPU 地址，不会被覆盖
- `Reclaim` 移到 `UpdateAndUpload` 内部，使用与 `AllocateUpload` 相同的 fence

---

### 2. 每帧无条件重复上传，RingBuffer 循环覆盖（次要原因）

**问题：**
即使修复了帧时序，`UpdateAndUpload` 每帧都无条件调用 `AllocateUpload` 分配新空间。RingBuffer 容量有限（默认 64KB），多帧后会回绕覆盖旧数据。如果 GPU 仍在消费旧帧，就会读到被覆盖的曲面细分参数。

**修复：**
- 引入 `m_terrainDirty` 标记，匹配 LightManager 模式
- 只在常量真正变化时才重新分配 RingBuffer
- 非 dirty 帧直接复用上一帧的 GPU 地址（地形常量通常是低频更新的）

---

### 3. CBV 256 字节对齐要求

**问题：**
DirectX 12 规范要求 CBV（Constant Buffer View）的 GPU 虚拟地址必须 256 字节对齐（`D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT = 256`）。

批量上传模式中，多个 `TerrainConstants` 打包在一个连续内存块中，每个地形块的 CBV 地址 = `baseAddr + blockIndex * stride`。stride 必须是 256 的倍数，否则第 2、3... 个地形块的 CBV 地址不满足对齐要求。

**修复：**
- `sizeof(TerrainConstants)` = 256 字节（通过 padding 补齐）
- `CONSTANT_ALIGNMENT = 256` 作为 stride
- `GetTerrainBlockAddress(blockIndex)` = `baseAddr + blockIndex * 256`

---

## 修复涉及的文件

| 文件 | 修改内容 |
|------|---------|
| `Engine/Renderer/Scene/TerrainManager/TerrainManager.h` | 添加 `m_terrainDirty`、`MarkDirty()`、`CONSTANT_ALIGNMENT` |
| `Engine/Renderer/Scene/TerrainManager/TerrainManager.cpp` | `UpdateAndUpload` 增加 dirty 检查；`BeginFrame` 移除 Reclaim |
| `Engine/Renderer/Scene/TerrainManager/TerrainResourceType.h` | `TerrainConstants` 填充到 256 字节 |
| `Engine/Renderer/RenderItemBuilder/TerrainRenderItemBuilder.cpp` | 用 `GetTerrainBlockAddress()` 查询已上传地址 |
| `Engine/Scheduler/FrameDriver.cpp` | `BeginFrame` 使用 `completedFence` |
| `Runtime/Game/Scene/GameWorld.cpp` | 注册 Immediate 回调；修正纹理索引 |

## 纹理索引修正

- `HeightMapIndex = 0`：高度图是地形主纹理，用于 DS 阶段的顶点置换
- `AlbedoMapIndex = 0`：暂无独立漫反射纹理，复用高度图
- `NormalMapIndex = 0xFFFFFFFF`：暂无独立法线贴图

## 经验教训

1. **RingBuffer 的 fence 必须与分配的 fence 一致**：Reclaim 和 AllocateUpload 必须使用同一个 fence 值
2. **批量上传模式中 stride 必须满足 CBV 对齐**：不是单个元素大小，而是连续数组中的步长
3. **匹配 LightManager 的 dirty 标记模式**：低频更新的常量不应该每帧重新分配
4. **Immediate 回调是上传的正确时机**：在 Render 之前完成，避免帧时序冲突
