# 帧资源分配器（FrameResourceManager）

## 职责边界

FrameResourceManager 只管理**每帧动态分配 + 回收的临时 GPU 缓冲区**（RingBuffer），不管理：

- **PassConstants**（`m_passCB`）— 地址固定的持久 CBV，不属于帧分配器，由独立的 PassCB 管理
- **静态实体持久缓冲区**（已移除）— 静态组件不应通过帧分配器管理

## 生命周期策略

RingBuffer 统一走"每帧分配 + 完成后 Reclaim"模式：

```
帧 N:  Allocate(data, size) → GPU 消费 → 帧 N+3: Reclaim
```

扩容自动进行（当前大小翻倍），调用方无需关心容量。

## 配置驱动

分配器不硬编码 RingBuffer 名称和数量，由外部配置初始化：

```json
{
  "ringBuffers": [
    { "name": "ObjectCB",   "initialSize": "16MB", "alignment": 256, "usage": "CBV"   },
    { "name": "Skinning",   "initialSize": "16MB", "alignment": 16,  "usage": "SRV"   },
    { "name": "Instance",   "initialSize": "16MB", "alignment": 16,  "usage": "SRV"   },
    { "name": "WaterCB",    "initialSize": "16MB", "alignment": 256, "usage": "CBV"   }
  ]
}
```

新增渲染功能（粒子、贴花、程序化植被）只需要在配置中加条目，**不修改引擎 core**。

## GPU 地址对齐

| 绑定方式 | 对齐要求 | 说明 |
|:---------|:--------:|------|
| CBV | 256 字节 | D3D12 硬件要求 |
| StructuredBuffer SRV | 16 字节 | 无硬件约束 |
| Raw/Typed Buffer SRV | 16 字节 | 同上 |

## 历史遗留清理

- **持久化缓冲区**（`AllocatePersistentObjectCB` / `AllocatePersistentInstanceBuffer` 等）已移除
  — 这是早期静态组件的遗留设计，静态实体应通过独立的 committed resource 管理

## 未来演进：命名与持久化能力

### 生命周期规则

帧驱动器不提供单帧生命周期的资源，所有 RingBuffer 至少 3 帧缓冲：

```
FrameResourceManager: 帧 N Allocate → 帧 N+3 Reclaim   (3 帧)
FrameScratchAllocator: 帧 N Allocate → 帧 N+1 Reset     (例外，仅用于临时上传)
```

### 静态 ECS 组件的持久化方向

后续静态 ECS 组件将省略每帧的矩阵计算，对应的 D3D12 资源是持久化的（上传一次，存活到组件销毁）。这意味着：

- 持久化资源**不是**通过 `FrameResourceManager` 的 RingBuffer 管理的
- 它们走独立的 committed resource 或独立的 persistent upload heap

### 命名考虑

如果未来 `FrameResourceManager` 需要同时管理临时 RingBuffer 和持久化缓冲区，当前名字可能不够准确：

| 未来可能的命名 | 说明 |
|---------------|------|
| `TransientRingBuffer` | 当前 RingBuffer 的职责，3 帧 reclaim |
| `PersistentBufferManager` | 静态 ECS 组件的持久化 GPU 资源 |
| `BufferAllocator` | 如果两者合并为一个统一分配器（暂不采用） |

当前阶段无需改动，记录备查。

## 预计算管线的边界（CPU 提前 N 帧计算）

### 动机

在场景实体数极大（>10000）或单帧 CPU 逻辑（AI/物理/剔除）成为瓶颈时，让 CPU 提前计算 N 帧的结果、缓存到 RingBuffer 中，使 GPU 可以直接消费预计算好的数据，是一种有效的性能手段。

### 关键约束

```
预计算管线 ≠ 事件缓冲
  ├─ 事件系统（MessageDispatcher）是同一帧内生产→消费，不跨帧
  └─ 预计算管线是跨帧的：CPU 帧 N 算出的结果，GPU 帧 N+K 才消费
```

### 与 FrameResourceManager 的关系

预计算结果需要写入 RingBuffer，且不能覆盖 GPU 尚未消费的槽位：

```
CPU 帧 N: 预计算下一帧的剔除/变换结果 → Allocate(ringBuf, size) → 写入
CPU 帧 N+1: GPU 帧 N 尚未完成 → RingBuffer N+1 的槽位不能回收
CPU 帧 N+2: GPU 帧 N 完成 → 槽位可回收复用
```

这完全复用了现有的 fence 保护机制——`BeginFrame(completedFence, nextFence)` 已经做了这件事：

| 帧 | CPU 写入 | GPU 读取 | fence 值 | 回收 |
|:---|:---------|:---------|:---------|:-----|
| N | Alloc → 写入 | — | submit=5 | — |
| N+1 | Alloc → 写入 | — | submit=6 | — |
| N+2 | Alloc → 写入 | 读取帧 N | completed=5 | 帧 N 槽位可回收 |
| N+3 | Alloc → 写入 | 读取帧 N+1 | completed=6 | 帧 N+1 槽位可回收 |

### 与动静分批/八叉树的区别

| | 动静分批 + 八叉树 | 预计算管线 |
|:--|:------------------|:-----------|
| **目标** | 减少**每帧**的 CPU 剔除量 | 让 CPU 跑在 GPU 前面 N 帧 |
| **瓶颈** | CPU 视锥剔除耗时 | CPU→GPU 数据依赖（上传、fence） |
| **前提** | 静态物体缓存 BVH，增量更新 | ECS 快照、确定性逻辑 |
| **与帧资源的关系** | 无关 | 强相关——预计算结果需写入 RingBuffer，fence 保护 |
| **可动态开关** | 可开关 | **必须可开关**——不能假定 CPU 比 GPU 快 |

### 当前阶段结论

**不实现预计算管线。** 当前场景实体数极少，FrameResource 的 3 帧 RingBuffer + Immediate 回调已足够保证 CPU/GPU 流水线并行。当实体数 > 10000 或单帧 CPU 时间超过 GPU 时间时再考虑。

详见 `Docs/todos/remaining_issues.md` #33（动静分批/八叉树）。
