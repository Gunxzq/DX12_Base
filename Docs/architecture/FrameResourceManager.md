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
