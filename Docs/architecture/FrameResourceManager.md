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
