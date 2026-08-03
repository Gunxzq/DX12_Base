# 骨骼动画系统设计

## 概述

骨骼动画在现有 ECS + 事件驱动架构下的接入方案。

## 核心原则

| 层 | 职责 | 行为 |
|:--|:-----|:-----|
| **ECS 组件** | 存数据 | 原子化，无行为，无生命周期 |
| **System（L4）** | 消息驱动状态机节点 | 读写 ECS 组件，发消息，常驻或消息触发 |
| **Builder/Renderer** | 只读 ECS 组件 | 产出 DrawCall，不感知游戏逻辑 |

## 组件定义

```cpp
// File: Engine/ECS/Core/Components.h

struct SkinnedComponent {
    Resource::SkeletonHandle skeletonHandle; // 骨骼资源引用（只读）
    std::string currentClip;                 // 当前动画片段名
    float timePos = 0.0f;                   // 当前时间位置
    uint32_t boneBufferIndex = UINT32_MAX;   // → FrameResource 骨骼缓冲区索引
};
```

## System 定义

### 常驻 System：AnimationAdvancer

每帧推进所有蒙皮实体的动画时间，插值骨骼矩阵，写入 GPU 缓冲区。

```
TaskPhase: LateUpdate
ThreadType: Any
AlwaysRun: true
```

```cpp
REGISTER_SYSTEM(AnimationAdvancer, LateUpdate, Any)
    .AlwaysRun()
    .Func([](Registry &r, const MessageContext &) {
        auto view = r.View<SkinnedComponent>();
        for (auto entity : view) {
            auto &skin = view.Get<SkinnedComponent>(entity);
            skin.timePos += deltaTime;
            // 循环检测
            if (skin.timePos > GetClipEndTime(skin.currentClip))
                skin.timePos = 0.0f;
            // 插值骨骼 → 写入 currFrameResource.SkinnedCB
        }
    });
```

### 消息 System：AnimationStateMachine

处理动画切换、启停等离散事件。

```
TaskPhase: LateUpdate
ThreadType: Any
WithMessage: PlayAnimationEvent, StopAnimationEvent
```

```cpp
REGISTER_SYSTEM(AnimationStateMachine, LateUpdate, Any)
    .WithMessage<PlayAnimationEvent>()
    .WithMessage<StopAnimationEvent>()
    .Func([](Registry &r, const MessageContext &ctx) {
        Entity entity = Entity(ctx.GetSingleValue());
        auto &skin = r.Get<SkinnedComponent>(entity);

        if (ctx.messageType == PlayAnimationEvent::StaticTypeHash) {
            skin.currentClip = ctx.GetClipName(); // 从 payload 解析
            skin.timePos = 0.0f;
            // 可选：发 AnimationStartedEvent
        } else if (ctx.messageType == StopAnimationEvent::StaticTypeHash) {
            skin.timePos = 0.0f;
        }
    });
```

## Builder 变更

```cpp
// OpaqueRenderItemBuilder::BuildTyped() 中

InstanceData instData = {};
// ... 填充世界矩阵、材质索引等

auto *skinned = registry.TryGetComponent<SkinnedComponent>(entity);
if (skinned) {
    instData.Flags |= INSTANCE_FLAG_SKINNED;
    instData.SkinnedCBIndex = skinned->boneBufferIndex;
}

// BatchKey 不变，蒙皮/非蒙皮自动分到不同 batch
```

## Renderer 变更

```
OpaqueRenderer::DrawInstanced()
  → if (Flags & INSTANCE_FLAG_SKINNED)
        cmdList->SetGraphicsRootDescriptorTable(1, boneBufferSRV); // cbSkinned(b1)
  → DrawIndexedInstanced()
```

## 渲染数据流

```
AnimationAdvancer (LateUpdate, CPU)
  │
  ├── 读 SkinnedComponent.timePos
  ├── 插值骨骼矩阵 → SkinnedConstants
  └── CopyData → FrameResource.SkinnedCB
        │
        ▼
OpaqueRenderItemBuilder (PreRender, CPU)
  │
  ├── 读 SkinnedComponent.boneBufferIndex
  └── 写入 InstanceData.Flags + InstanceData.SkinnedCBIndex
        │
        ▼
OpaqueRenderer (Render, GPU)
  │
  ├── 读 InstanceData.Flags
  ├── Flags & SKINNED → 绑 cbSkinned(b1)
  └── DrawIndexedInstanced
```

## 不做的设计

| 做法 | 原因 |
|:-----|:-----|
| ❌ 新增 RenderPhase | 蒙皮仍在 Opaque/Transparent，时机不变 |
| ❌ 包裹层持有动画状态 | 状态在 SkinnedComponent，System 驱动 |
| ❌ 龙书的 Layer 分离 | 分 Phase 不分 Layer，ECS 架构优于龙书 |
| ❌ 每实体独立的 AnimationSystem | 一个常驻 System 遍历所有蒙皮实体 |

## 架构约束

### 高频路径走常驻，离散事件走消息

参考大型引擎的做法（Unreal Tick / Unity Update / Frostbite 分帧调度），高频路径直接调用，不走事件总线：

| 路径 | 适用场景 | 对应机制 | 示例 |
|:-----|:---------|:---------|:-----|
| **高频**（每帧） | 连续过程、必执行 | `AlwaysRun` System | 动画推进、物理步进、Transform 传播 |
| **低频**（离散） | 事件触发、状态变更 | `WithMessage<T>()` System | 动画切换、伤害响应、开门 |

**错误示例（不要这样做）**：
```cpp
// ❌ 高频功能走消息路径 — 每帧发 FrameTick 来驱动动画
PostEvent(FrameTickEvent::StaticTypeHash, ...);
```

**正确示例**：
```cpp
// ✅ 高频用 AlwaysRun，View 按组件筛选
REGISTER_SYSTEM(AnimationAdvancer, LateUpdate, Any)
    .AlwaysRun()
    .Func([](Registry &r, const MessageContext &) {
        auto view = r.View<SkinnedComponent>(); // 只迭代蒙皮实体
        for (auto e : view) { /* 推进动画 */ }
    });

// ✅ 离散事件用消息
REGISTER_SYSTEM(AnimationStateMachine, LateUpdate, Any)
    .WithMessage<PlayAnimationEvent>()
    .Func([](Registry &r, const MessageContext &ctx) {
        // 只响应 PlayAnimationEvent，不会每帧执行
    });
```

### 判据

不确定某功能该走消息还是常驻时，问三个问题：

1. **这个功能每帧都必须执行吗？** → 是 → 常驻 System
2. **这个功能只在某个条件满足时执行吗？** → 是 → 消息 System
3. **这个功能是几帧才发生一次的离散事件吗？** → 是 → 消息 System

## 内存与性能

| 项 | 量级 | 说明 |
|:---|:----|:-----|
| **SkinnedComponent 大小** | ~48 字节 | skeletonHandle(8) + string(24) ≈ 实际指向外部数据 + timePos(4) + boneBufferIndex(4) |
| **骨骼矩阵缓冲区** | 96 × 64 字节 = 6KB 每角色 | SkinnedConstants 的 BoneTransforms[96] |
| **AnimationAdvancer 开销** | O(蒙皮实体数) | 100 个角色 ≈ 600KB 矩阵计算，毫秒级 |
| **合批影响** | 蒙皮/非蒙皮自动分 batch | 蒙皮物体通常很少，不影响主流 |
