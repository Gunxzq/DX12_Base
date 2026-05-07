# L3 调度层 - 消息驱动架构

## 架构概览

```
L4 应用层 (你的代码)
    ↓ 注册System + 发送消息
L3 调度层 (FrameDriver)
    ↓ 动态构建DAG
L2 数据层 (Registry)
    ↓ 数据访问
L1 通信层 (MessageArena + Bucket)
```

## 核心流程

每帧 `Tick()` 执行：

1. **收集消息** - 从所有优先级桶窃取消息
2. **激活System** - 根据消息类型找到感兴趣的System
3. **构建DAG** - 将System转化为Task并建立依赖
4. **执行阶段** - 按阶段执行所有任务
5. **帧同步** - 调用L4层回调（多缓冲交换）

## 使用示例

### 1. 定义事件

```cpp
// Events/PlayerInputEvent.h
#pragma once
#include "System/Event/Event.h"

struct PlayerInputEvent {
    DEFINE_EVENT_TYPE_HASH(0x0001)
    
    int keyCode;
    bool pressed;
};
```

### 2. 注册System

```cpp
// Systems/PlayerMoveSystem.h
#pragma once
#include "System/Scheduler/Scheduler.h"
#include "Events/PlayerInputEvent.h"

using namespace DX12::Scheduler;

// 注册System，声明对PlayerInputEvent感兴趣
static SystemId playerMoveSystemId = REGISTER_SYSTEM(PlayerMoveSystem, Update, Any)
    .WithMessage<PlayerInputEvent>()
    .DependsOn("PhysicsSystem")
    .Func([](ECS::Registry& registry, const MessageContext& ctx) {
        // 处理玩家移动逻辑
        auto view = registry.view<Player, Transform>();
        for (auto [entity, player, transform] : view.each()) {
            // 更新位置...
        }
    });
```

### 3. 发送消息

```cpp
// 从输入系统发送事件
void InputSystem::Update() {
    if (KeyPressed(KeyCode::W)) {
        PostEvent(PlayerInputEvent{ 
            .keyCode = KeyCode::W, 
            .pressed = true 
        }, EventPriority::P2_Normal);
    }
}
```

### 4. 多缓冲交换

```cpp
// 在L4层管理多缓冲
class TransformBuffer {
    std::vector<Matrix> bufferA;
    std::vector<Matrix> bufferB;
    bool frontIsA = true;
    
public:
    void UpdateFromRegistry(ECS::Registry& registry) {
        // 写入Back Buffer
        auto& back = frontIsA ? bufferB : bufferA;
        // ... 更新数据
    }
    
    void Swap() { frontIsA = !frontIsA; }
    
    const Matrix* GetReadPtr() const {
        return frontIsA ? bufferA.data() : bufferB.data();
    }
};

// 注册帧同步回调
TransformBuffer transformBuffer;

void Initialize() {
    // 注册System
    REGISTER_SYSTEM(TransformSync, LateUpdate, Any)
        .Func([](ECS::Registry& r, const MessageContext& ctx) {
            transformBuffer.UpdateFromRegistry(r);
        });
    
    // 注册交换回调（在帧同步点执行）
    OnFrameSync([]() {
        transformBuffer.Swap();
    }, "TransformBufferSwap");
}
```

## 关键设计决策

### 为什么消息驱动？

- **极致节能**：没有消息时，TaskGraph为空，CPU 0负荷
- **热插拔**：L4层可动态注册System，无需重启
- **解耦**：System之间不直接调用，通过消息通信

### 为什么L2层不处理多缓冲？

- **复杂度控制**：Registry保持纯净的EnTT封装
- **灵活性**：L4层自行决定多缓冲策略（单/双/三缓冲）
- **性能可控**：L4层知道何时需要交换，避免盲目交换

### 双缓冲TaskGraph

为避免每帧分配内存，可以使用双缓冲：

```cpp
// FrameDriver内部实现
TaskGraph m_frontGraph;  // 当前执行的图
TaskGraph m_backGraph;   // 后台构建的图

// Tick()中：
std::swap(m_frontGraph, m_backGraph);  // 原子交换指针
m_backGraph.Clear();
// 在m_backGraph上构建下一帧...
```

## 文件清单

| 文件 | 职责 |
|------|------|
| `FrameDriver.h/cpp` | 帧循环控制器，整合所有组件 |
| `TaskGraphBuilder.h/cpp` | 消息到DAG的构建器 |
| `SystemRegistry` | L4层System注册表 |
| `TaskGraph.h/cpp` | DAG管理和拓扑排序 |
| `TaskExecutor.h/cpp` | 线程池任务执行 |
| `Scheduler.h` | L4层便捷API |
