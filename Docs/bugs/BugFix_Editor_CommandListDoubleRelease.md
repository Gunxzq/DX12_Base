# BugFix: Editor 端 CommandList 双重释放导致 COMMAND_LIST_SYNC

## 日期

2026-07-12

## 症状

编辑器启动后立即崩溃，D3D12 依次输出：

```
D3D12 ERROR: ID3D12CommandQueue::ExecuteCommandLists: Failed to execute a command list
  ... because the command queue fence has not advanced past previous executions of the command list.
  [ EXECUTION ERROR #553: COMMAND_LIST_SYNC]

Assertion failed!
File: ...\Engine\Renderer\RHI\Command\CommandList\CommandListPool.cpp
Line: 144
Expression: entry.inUse.load() == true
```

## 根因

`EditorMainClearSystem`（`Editor/EditorLib/Editor.cpp`）在调用 `SubmitRenderCommand` 后**又手动调用了** `cmdMgr.ReleaseCommandList`：

```cpp
cmdList.Close();
m_context->FrameDriver->SubmitRenderCommand(RenderPhase::PrePass, cmdHandle);
// 正确：FrameDriver 内部会释放 cmdList
uint64_t seq = m_context->GetNextSequence();
cmdMgr.ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdHandle);  // ← 错误：双重释放
cmdMgr.ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle, seq);
```

### 调用链

```
EditorMainClearSystem (alwaysRun, RenderPhase::PrePass)
  ├─ cmdMgr.AcquireAllocator / AcquireCommandListHandle
  ├─ 录制命令
  ├─ cmdList.Close()
  ├─ SubmitRenderCommand(PrePass, cmdHandle)   ← FrameDriver 存储 handle
  ├─ ReleaseCommandList(cmdHandle)              ← 第一次释放 (手动)
  └─ ReleaseAllocator(allocHandle, seq)

FrameDriver::ExecuteRenderPhase (稍后)
  └─ ReleaseCommandList(handle)                 ← 第二次释放 (自动)
```

### 级联崩溃链

1. `CommandListPool::Release` 断言 `entry.inUse == true` 失败 → 命令列表被提前释放
2. 分配器对应的 `lastFenceValue` 未正确更新 → 下一帧 `AcquireAllocator` 认为该分配器可用
3. 实际 GPU 仍在处理该分配器的旧命令列表 → `ExecuteCommandLists` 报 `COMMAND_LIST_SYNC`
4. 分配器被标记为 `inUse` 但 fence 不推进 → 后续帧全部崩溃

## 约束规则

**`SubmitRenderCommand` 与 `ReleaseCommandList` 是互斥方法。**

- `SubmitRenderCommand` 将命令列表句柄交给 `FrameDriver`，由 `FrameDriver::ExecuteRenderPhase` 在渲染阶段执行完毕后自动调用 `ReleaseCommandList` 释放
- 调用方**禁止**在 `SubmitRenderCommand` 之后再次调用 `ReleaseCommandList`
- `ReleaseCommandList` 仅在独立使用命令列表（不经过 `FrameDriver`，如后台上传任务）时手动调用

## 正确模式

```cpp
// 所有经过 FrameDriver 的渲染 System 必须遵循此模式：
cmdList.Close();
m_context->FrameDriver->SubmitRenderCommand(phase, cmdHandle);

uint64_t seq = m_context->GetNextSequence();
m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle, seq);
// 没有 ReleaseCommandList！FrameDriver 会处理
```

## 参考

- Game 端所有 System（`GameWorld_RenderSystems.cpp`）均遵循此模式，从不手动调用 `ReleaseCommandList`
- `FrameDriver::SubmitRenderCommand` 实现见 `Engine/Scheduler/FrameDriver.cpp` L83-86
- `FrameDriver::ExecuteRenderPhase` 释放逻辑见 `Engine/Scheduler/FrameDriver.cpp` L95-99