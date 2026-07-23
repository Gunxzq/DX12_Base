# Bugfix 记录

> 记录本轮重构中修复的关键 Bug，供后续排查参考。

---

## 1. CommandAllocatorPool 扩容崩溃

### 症状
随机崩溃在 `CommandAllocator::Reset()` → `m_allocator->Reset()`，`m_allocator` 为空。位置随机（SSAO / ImGui / 纹理上传）。

### 根因
`std::deque<Entry>` 存储值类型 Entry（含 `unique_ptr` + `atomic` + `alignas(64)`）。扩容时 MSVC deque 的块指针表重分配导致已有 Entry 被搬迁（move），`unique_ptr<CommandAllocator>` 内的 `ComPtr<ID3D12CommandAllocator>` 被移走，旧 Entry 残留 `m_allocator = nullptr`。后续 `Acquire` 命中该残留条目时崩溃。

### 修复
`std::deque<std::unique_ptr<Entry>>` — Entry 通过 `make_unique` 在堆上独立分配，deque 只存 8 字节指针。`push_back` 永不失效已有引用。

### 涉及文件
`Engine/Renderer/RHI/Command/Allocator/CommandAllocatorPool.h/.cpp`

---

## 2. 后台任务 AcquireAllocator 传错 Fence 值

### 症状
材质 buffer 上传时 `COMMAND_ALLOCATOR_RESET` → 设备移除。`HR: 0x887A0005`。

### 根因
`MeshLoadTask` / `TextureLoadTask` / `SceneConstructor` 的 `gpuWork` 中 `AcquireAllocator` 传了 `GetNextSequence()`（CPU 单调计数器），但池子期望的是 **GPU 实际已完成 fence 值**。CPU 计数器永远超前，导致池子误判 allocator 可回收，返回了 GPU 仍在执行中的 allocator。提交命令后 GPU 访问已回收内存。

另外 COPY 队列的完成值被错误地用于 DIRECT 队列的 allocator 获取——两个队列的 GPU fence 进度独立，互不通用。

### 修复
- `AcquireAllocator` 使用 `GetCompletedFenceValue(type)`（GPU 实际已完成值）
- `Release` / `gpuMgr.Release` 使用 `GetNextSequence()`（CPU 未来值，保护资源不被提前释放）
- COPY 和 DIRECT 各自使用自己队列的 `GetCompletedFenceValue`

### 涉及文件
`Engine/Background/MeshLoadTask.h`
`Engine/Background/TextureLoadTask.h`
`Engine/Scene/SceneConstructor.cpp`

---

## 3. entt 组件存储池 Worker 线程竞态

### 症状
`entt::dense_map::emplace` → `vector::_Emplace_reallocate` → `free` 堆损坏崩溃。

### 根因
`OpaqueTag` / `TransparentTag` 等组件的 entt 存储池首次通过 `assure()` 创建时，`dense_map` 内部 vector 重分配不是线程安全的。之前旧函数（`CreateGroundPlane` / `CreateWater`）在主线程添加组件时顺带触发了 `assure`。删除后首次触发放到了 Worker 线程的 Builder 系统中。多 Worker 线程并发 `emplace` 导致堆损坏。

### 修复
在 `GameWorld::Initialize()` 中预触所有 Builder 可能访问的组件类型：
```cpp
m_registry->view<MeshComponent>();
m_registry->view<TransformComponent>();
m_registry->view<OpaqueTag>();
m_registry->view<TransparentTag>();
m_registry->view<SkinnedTag>();
m_registry->view<SkinnedComponent>();
m_registry->view<TerrainComponent>();
m_registry->view<BillboardComponent>();
m_registry->view<WaterComponent>();
```

### 涉及文件
`Game/Game/Scene/GameWorld.cpp`

---

## 4. SceneConstructor 材质 buffer 重复分配（已修复，非 crash）

### 症状
`async_test.json` 场景加载后材质 buffer 被创建两次（一次来自 `CreateMaterials`，一次来自 `SceneConstructor`），后一次通过 `SetMaterialBufferSRV` 覆盖前一次。

### 修复
移除 `CreateMaterials()`，材质注册 + GPU buffer 上传全部由 SceneConstructor 异步完成。

### 涉及文件
`Game/Game/Scene/GameWorld_Assets.cpp`
`Engine/Scene/SceneConstructor.cpp`

---

## 5. 帧资源配置残留（已清理，非 crash）

### 症状
`Config/game/frame_resource.json` 和 `Game/Config/frame_resource.json` 各有一份，前者残留 `"WaterCB"` 条目。

### 修复
移除 `Config/game/frame_resource.json`，`Game/Config/frame_resource.json` 中删除 `"WaterCB"` 条目（WaterManager 自管 RingBuffer）。

### 涉及文件
`Game/Config/frame_resource.json`

---

## 总结

| # | Bug | 根因类别 | 严重程度 |
|:-:|:----|:---------|:---------|
| 1 | AllocatorPool 扩容崩溃 | 容器选型 + 并发 | 致命 |
| 2 | 后台任务 fence 值传递错误 | 语义混淆 | 致命 |
| 3 | entt 组件存储池 Worker 竞态 | 线程安全 | 致命 |
| 4 | 材质 buffer 重复分配 | 设计残留 | 低 |
| 5 | 配置文件残留 | 清理遗漏 | 低 |
