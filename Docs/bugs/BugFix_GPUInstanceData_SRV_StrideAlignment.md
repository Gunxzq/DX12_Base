# BugFix: GPUInstanceData SRV 元素对齐错位 → 剔除全灭/频闪（SRV stride 与 HLSL 结构体一致性）

## 日期

2026-08-09

## 症状

方案 B（动态 EntityBucketMap）落地后，编辑器小场景/城市场景：

1. **剔除恒全灭**：`[InstanceCulling][Verify] visible=0 / total=N (0.0%)` 恒为 0；
   `readback: total=0 nonZeroBuckets=0 zeroBuckets=1024`（N 从 3 到 1007 不等）
2. **相机静止也频闪**：某些帧有内容（RenderDoc 抓到 `IndirectDrawIndexed(<N, 1>)` InstanceCount=1），
   某些帧空白（InstanceCount=0）——帧间交替
3. **CPU 侧数据自检完全正确**：
   ```
   [InstanceCullingBuffer][Diag] instance0: translate=(-20.429,0.000,-29.322) radius=21.213 bucketOffset=0 bucketCount=1 instances=3
   ```
   实例 0 的 21 米半径包围球在相机 (-15.3, 7.9, -35) 附近（6 平面手算全部通过），**绝不可能被视锥剔除**，
   但 CS 却全灭 → 坐实 CS 读到的 GPU 数据 ≠ CPU 自检数据
4. dispatch 每帧执行：`dispatch: ok=120 skipReady=0 skipCbAddr=0`（无任何拦截）

## 根因（双层）

### 1. RingBuffer 段起始偏移非 96B 对齐 → SRV FirstElement 向下取整错位（本质根因）

`CreateSRV` 的 FirstElement 计算：

```cpp
const D3D12_GPU_VIRTUAL_ADDRESS baseAddr = ringRes->GetGPUVirtualAddress();
const uint64_t byteOffset = segmentAddr >= baseAddr ? (segmentAddr - baseAddr) : 0;
const uint32_t firstElement = static_cast<uint32_t>(byteOffset / sizeof(GPUInstanceData)); // /96
```

**D3D12 规范（MSDN `D3D12_BUFFER_SRV`）**：
- `Buffer.FirstElement` 是**元素索引**，SRV 视图起点 = 缓冲基址 + `FirstElement × StructureByteStride`
- `StructureByteStride` **必须与 HLSL 结构体大小完全一致**（此处 `sizeof(GPUInstanceData)` = 96B）
- 因此**段起始偏移 byteOffset 必须是 96 的整数倍**，否则 firstElement 向下取整 → SRV 元素边界错位

实际发生：

- `frame_resource.json` 中 `InstanceCulling` 条目 `alignment: 16`（2 的幂）
- 方案 B 在 `Upload` 中**同一 RingBuffer 连续分配实例段（N×96B）+ bucketMap 段（M×4B）**
- bucketMap 段大小（4B 倍数）不是 96 的倍数 → 下一帧实例段起始偏移被破坏（如 336、480…）
- `byteOffset=336 → firstElement = 336/96 = 3`（向下取整）→ SRV 视图从 288B 处开始，**实际数据在 336B** → 错位 48B
- CS 读 `gInstances[dtid.x]` 的 world/radius 全是错位垃圾 → 包围球退化 → 全灭
- 某些帧实例段恰好从 0/384/768…（96 倍数）分配 → SRV 正确 → 可见 → **帧间交替 = 频闪**

### 2. RingBuffer::Allocate 位运算对齐对非 2 幂 alignment 无效（放大层）

```cpp
// 原实现（只对 2 的幂有效！）
uint32_t alignedHead = (m_head + alignment - 1) & ~(alignment - 1);
```

96 不是 2 的幂（96 = 2^5 × 3），位运算掩码 `~(96-1)` 无法正确对齐 96 → 即使显式传 alignment=96 也无效。

## 修复（3 文件 4 处）

| 文件 | 改动 |
|:--|:--|
| `FrameResourceManager.h` | `Allocate` 增加 `alignment = 0` 参数（0 = 用条目配置 alignment） |
| `FrameResourceManager.cpp` | `Allocate` 实现透传：`align = alignment ? alignment : entry->alignment` |
| `RingBuffer.cpp` | 对齐算法改**算术对齐**：`((head+align-1)/align)*align`（兼容非 2 幂） |
| `InstanceCullingBuffer.cpp` | Upload 实例段分配显式 `alignment = sizeof(GPUInstanceData)`（96B） |

模拟验证（实例段显式 96 对齐后，bucketMap 段不再破坏）：

```
帧1: 实例段偏移=0   firstElement=0   精确
帧2: 实例段偏移=384 firstElement=4   精确
帧3: 实例段偏移=768 firstElement=8   精确
帧4: 实例段偏移=1152 firstElement=12 精确
```

## 规范要点（MSDN D3D12_BUFFER_SRV，2026-08-09 查证）

- `StructureByteStride`：StructuredBuffer 的**元素大小**，必须与 HLSL 结构体大小一致（跨 C++/HLSL 结构体逐字节对齐）
- `FirstElement`：元素索引，视图起点 = 基址 + FirstElement × Stride——**分配段起始偏移必须按 Stride 对齐**
- HLSL 结构体内部按 16B 对齐（float4/float4x4）与 SRV 元素边界按 Stride 对齐是**两个不同层面**：
  - 结构体**内部字段** 16B 对齐（cbuffer 打包规则/自然对齐）→ 保证 sizeof 正确
  - SRV **元素边界**按 StructureByteStride 对齐 → 保证 FirstElement 精确

## 经验教训

1. **GPU 结构体（C++ ↔ HLSL）必须**：
   - 全显式 float4/float4x4 字段（杜绝标量数组 pad，如 `float pad[5]` 在部分编译路径按 16B 膨胀）
   - `static_assert(sizeof == 预期值)` 锁定 stride（对齐 FrameResourceTypes.h 既有 `% 16 == 0` 模式）
2. **RingBuffer 分配段起始偏移必须按元素 Stride 对齐**（不只按 2 的幂 alignment）——SRV FirstElement 依赖此
3. **对齐算法用算术除乘，不用位运算**（位运算只对 2 的幂有效）
4. 此类问题特征：CPU 自检数据正确 + dispatch 每帧执行 + 但剔除全灭 + 帧间交替（频闪）→ 优先怀疑
   SRV 段偏移/元素对齐
