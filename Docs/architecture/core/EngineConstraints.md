# 引擎开发约束

## 架构分层

| 层 | 职责 | 行为边界 |
|:--|:-----|:---------|
| **ECS 组件** | 原子化数据 | 无行为，无生命周期，纯数据结构体 |
| **System（L4）** | 状态机节点 | 可读写 ECS 组件，可发消息，常驻或消息触发 |
| **Builder** | 构建渲染项 | 只读 ECS 组件，产出 RenderItem 队列 |
| **Renderer** | 提交 DrawCall | 只读 RenderItem，绑定 PSO/缓冲区 |
| **事件系统（L1）** | 跨线程通知 | SoA 环形缓冲区，优先级桶，无锁写入 |

## System 调度约束

| 路径 | 适用场景 | 注册方式 | 示例 |
|:-----|:---------|:---------|:-----|
| **高频**（每帧必执行） | 连续过程 | `AlwaysRun()` | 动画推进、物理步进、Transform 传播 |
| **低频**（离散事件） | 状态变更 | `WithMessage<T>()` | 动画切换、伤害响应、开门 |

**禁止**：用消息系统模拟每帧驱动（如每帧发 FrameTick 来触发 System）。

## 多线程安全

- System 中不能进行内存分配
- 渲染阶段的 System 只能录制命令列表
- 跨线程数据传递通过事件系统的 Arena 完成，不直接共享指针

## 渲染管线数据访问

参阅 `Docs/architecture/rendering/RenderDataAccess.md`

| 资源 | 可读 | 可写 | 写入者 |
|:----:|:----:|:----:|--------|
| 主深度缓冲（Main DSV） | 所有 Pass | ❌ 仅 Opaque 主渲染阶段 | 场景主渲染 |
| 主颜色缓冲（Main RTV） | 所有 Pass | ❌ 仅 Opaque 主渲染阶段 | 场景主渲染 |
| 私有深度/颜色缓冲 | 创建者 + 下游 | ✅ 创建者 Pass | 各 Pass 内部 |

## GPU 地址对齐要求

`RingBuffer` 分配时通过 alignment 参数控制 GPU 地址对齐粒度，不同类型 GPU 资源的对齐要求不同：

| GPU 绑定方式 | 对齐要求 | 说明 |
|:------------|:--------:|------|
| **CBV**（Constant Buffer View） | **256 字节** | D3D12 硬件要求 `D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT` |
| **StructuredBuffer SRV**（t#） | **16 字节** | 无硬件对齐约束，16 字节兼容所有结构体 stride |
| **Raw / Typed Buffer SRV**（t#） | **16 字节** | 同上 |

**原则**：`RingBuffer` 默认 alignment=256（保守适配 CBV），非 CBV 用途应在 `AllocateUpload` 调用处显式传入 `16` 避免空间浪费。

## 蒙皮骨骼动画

参阅 `Docs/architecture/animation/SkinnedAnimation.md`

- `SkinnedComponent` 持有动画状态（timePos、currentClip、boneBufferIndex）
- `AnimationAdvancer`（常驻 System）每帧推进时间 → 插值矩阵 → 写入 GPU
- `AnimationStateMachine`（消息 System）响应 `PlayAnimationEvent` / `StopAnimationEvent`
- Builder 只读 SkinnedComponent，根据 `boneBufferIndex` 标记 InstanceData

## GPU 资源状态管理

- 资源管理器/池（`RenderTargetPool`、`GpuResourceManager`）只负责资源生命周期
- **管理器不追踪也不重置 GPU resource state**
- 使用资源的 system 必须在自己管理的命令列表中通过 `ResourceBarrier` 将资源转到所需状态
- **不能假设资源初始状态为 `COMMON`**：池化资源被释放后可能在任意 GPU 状态，复用时状态不会重置
- `OnResize`/重建资源的方法必须检查尺寸是否真实变化，避免窗口初始化过程中重复重建

### ResourceBarrier 规则

1. 每个 system 独立管理其使用的所有资源的屏障
2. system 不能假定上一个 system 留下了什么状态——它必须自己处理
3. 推荐模式：`COMMON → 目标状态 → 工作 → COMMON`，保证帧间状态一致
4. 如果有多条 code path（如 blur PSO 未就绪时提前返回），**所有路径都必须做屏障回退**

## 组件与渲染项的关系

- **组件**：持久化存储，ECS Registry 管理生命周期
- **渲染项**：每帧从组件临时生成，渲染完成后丢弃
- 渲染项不直接持有 GPU 数据，通过索引引用帧资源的常量缓冲区
