# todo-10 — PreRender 并行化与多缓冲分析

日期：2026-07-02（合并自原 todo10.md + todo11.md）
关联：todo.md#14、todo.md#21

---

## 一、问题背景

### 当前：三阶段三次遍历

```
PreCulling:
  CullingSystem → for each entity → cull test → CullingResult(visible)
                           ↓ 遍历一次

PostCulling:
  LODSystem     → for visible in CullingResult → LODResult
                           ↓ 遍历二次

PreRender:
  Builder       → for visible in CullingResult → read LODResult → RenderItem
                           ↓ 遍历三次
```

问题：
1. 一个实体被遍历三次，每次只做一件小事
2. `CullingResult` 和 `LODResult` 是纯传递数据的临时结构，每帧分配
3. 三个 System 之间的数据耦合使并发复杂化（需阶段间同步点）

### FrameDriver::Tick() 中的位置

```
Frame N:
  ① Render Phase          ← 读 Slot N-1（上帧 FrameSync 冻结的）
  ② Update Phase          ← 写 Slot N（实时 ECS）
     ├── EarlyUpdate / Update / LateUpdate
     ├── PreCulling       → CullingResult(临时)
     ├── PostCulling      → LODResult(临时)
     ├── SceneDataUpload
     └── PreRender        → RenderQueue(临时)
  ③ FrameSync            ← 冻结 Slot N，下帧 Render 可读
```

---

## 二、方案：构建器内联剔除 + LOD

### 目标

CullingSystem 和 LODSystem **不再作为 ECS System 存在**，降级为纯函数工具库。

Builder 在单次遍历中完成所有工作：

```
PreRender:
  Builder → for each entity {
                if (!FrustumCull(transform, bounds, frustum)) continue;
                geo = PickLOD(transform, lodMesh, cameraPos);
                BuildRenderItem(entity, geo, transform, ...);
             }
```

工具函数签名：
- `FrustumCull(TransformComponent, BoundingVolume, Frustum) → bool`
- `PickLOD(TransformComponent, LODMeshHandle, CameraPos) → GeometryHandle`

### 收益

| 对比 | 三阶段 | Builder 内联 |
|:-----|:-------|:-------------|
| 遍历次数 | O(3N) | O(N) |
| 临时结构 | CullingResult + LODResult | 无 |
| 每帧分配 | visible 列表 + LOD 列表 | 无（栈上局部变量） |
| 数据局部性 | 差（写→读→再写） | 好（循环内一次完成） |
| 并发友好度 | 中（需阶段间同步点） | 高（每个 entity 独立） |

### 并发模式

内联后每个 entity 处理完全独立，天然适合 `parallel_for`：

```cpp
parallel_for(0, entities.size(), [&](size_t begin, size_t end) {
    LocalQueue localQueue; // 线程局部队列
    for (size_t i = begin; i < end; ++i) {
        auto entity = entities[i];
        if (!FrustumCull(transform, bounds, cameraFrustum))
            continue;
        auto geo = PickLOD(transform, lodMesh, cameraPos);
        localQueue.Add(BuildItem(entity, geo, ...));
    }
    merge(localQueue); // 合并到全局队列
});
```

### 前提条件

- [ ] **CullingSystem 移除**：剔除逻辑下沉到 Builder
- [ ] **LODSystem 移除**：LOD 计算下沉到 Builder
- [ ] **MeshComponent 已有 localBounds**：已存在，可以用于剔除
- [ ] **多线程安全的内存分配**：`Allocate("Instance", ...)` 等需要在并发环境下安全，或改为统一上传阶段
- [ ] **ThreadLocal RenderQueue**：Builder 产出需先写线程局部队列，最后合并

---

## 三、多缓冲场景分析

### 3.1 当前多缓冲场景

#### 3.1.1 SwapChain 双缓冲/三缓冲（GPU 级）

| 位置 | 类型 | 说明 |
|:-----|:-----|:------|
| `SwapChainManager` | DXGI SwapChain | 硬件层面，2-3 个 back buffer |
| 生产者 | GPU（渲染管线） | Present() 时翻转 |
| 消费者 | 显示器 | 扫描输出 |

**不需要额外处理**——DXGI 管理。

#### 3.1.2 FrameResource RingBuffer（CPU→GPU 级）

| 位置 | 数量 | 说明 |
|:-----|:-----|:------|
| `FrameResourceManager` | `FRAME_COUNT = 3` | ObjectCB、InstanceData、Skinning 等每帧数据的 RingBuffer |
| `BeginFrame(completedFence, nextFence)` | — | 通过围栏回收已消费的槽位 |
| 生产者 | 各 Builder（PreRender） | `Allocate("ObjectCB", ...)`, `Allocate("Instance", ...)` |
| 消费者 | GPU（Render 阶段） | 提交后异步执行 |

**这是最核心的多缓冲场景。** 每个 RingBuffer 内部通过 fence 值来追踪 GPU 消费进度。

#### 3.1.3 FrameDriver::FrameSync（逻辑→渲染级）

| 位置 | 数量 | 说明 |
|:-----|:-----|:------|
| `FrameDriver::Tick()` | 2 帧（逻辑 vs 渲染） | Render 读**上一帧**冻结数据，Update 写**当前帧** |
| `FrameSync()` | 回调钩子 | L4 层注册的回调在此执行多缓冲交换 |

```cpp
// Game 层注册的回调示例
m_context->FrameDriver->RegisterFrameSyncCallback([this]() {
    std::swap(m_frontBuffer, m_backBuffer);
});
```

**FrameSync 是逻辑帧与渲染帧之间的天然同步屏障。** 它保证了 Render 读取的数据在整个 Render 阶段内稳定不变。

#### 3.1.4 CullingResult / LODResult（临时结构）

| 位置 | 数量 | 说明 |
|:-----|:-----|:------|
| `CullingResult` | 1 帧 | PreCulling 写入，PreRender 读取，然后丢弃 |
| `LODResult` | 1 帧 | 同上 |

**不需要多缓冲。** 它们在同一个 PreRender 阶段内生产并消费，不存在跨帧竞争。采用内联方案后这两个结构直接被移除。

#### 当前多缓冲总览

| 场景 | 缓冲区数量 | 同步机制 | 是否需要手动管理 |
|:-----|:-----------|:---------|:----------------|
| SwapChain | 2-3 | DXGI Present() | ❌ 自动 |
| FrameResource RingBuffer | 3 | GPU Fence | ✅ FrameResourceManager |
| FrameSync（逻辑/渲染分离） | 2 | FrameSync 回调 | ✅ L4 层回调 |
| CullingResult / LODResult | 0（单帧） | 无 | ❌ 建议移除 |

### 3.2 未来多缓冲场景分析

#### 3.2.1 异步资产流式加载 — ✅ 已被事件系统覆盖

当前 `TerrainLoadTask` 的模式（后台线程 → `PostEvent` → 主线程响应）会泛化为通用 `AsyncAssetTask<T>`。

**不需要额外实现 SPSC Queue。** `MessageDispatcher` 本身就是线程安全的 MPMC 队列：

```
后台线程池（N个worker）                         主线程
  LoadMesh("soldier.m3d")                         每帧 Tick():
    ↓                                                ↓
  PostEvent(ASSET_LOADED) ──────→ MessageDispatcher → BuildFromBuckets() → System 响应
  LoadTexture("stone.dds")           (线程安全队列)  （DAG 构建时消费）
    ↓
  PostEvent(ASSET_LOADED) ──────→  消息被缓冲，         ↓
                                  不会丢失          注册 Handle → ECS
```

**事件系统本身已经是异步加载的多缓冲机制。** 不需要额外的 SPSC Queue，`MessageDispatcher` 已经提供了线程安全的生产者-消费者通道。

#### 3.2.2 网络状态同步 — ✅ 已被事件系统覆盖

P2P 模式下，网络线程收到远端状态包后通过事件系统安全传递到主线程：

```
网络线程                          主线程
  recv player_state_packet           每帧 Tick():
       ↓                                ↓
  PostEvent(NET_PLAYER_STATE) ──→ MessageDispatcher
                                    → BuildFromBuckets()
                                    → System 响应：更新 ECS
```

**不需要手动双缓冲。** 事件系统天然提供了「网络线程写入 ↔ 主线程消费」的隔离。当前 `NetworkTopologyP2P` 还没有走事件系统写入 ECS，这是一个可改进的点。

#### 3.2.3 输入预测与回滚 — 可能性中

如果未来需要帧同步网络对战：

```
角色输入 → InputBuffer[N]（环形缓冲，保存最近 N 帧输入）
              ↓
每帧：读取当前帧输入 → 模拟 → 渲染
              ↓
收到确认帧 → 如果预测错误 → 回滚到确认帧 → 重新模拟 → 跳到当前帧
```

`InputBuffer[N]` 本身就是多缓冲——为回滚保留历史状态。当前 `NetworkTopologyP2P` 已有 `m_playerInputs` 和 `m_pendingInputs` 存储，但尚未形成完整的回滚机制。

#### 3.2.4 GPU Driven Culling — 可能性中

如果剔除移到 GPU：

```
CPU 端（每帧上传）                    GPU 端（逐帧执行）
  Upload InstanceData[N] ──────→    DispatchCullingCS
  Upload TransformData[N]            ↓
                                    VisibleList (GPU Buffer)
                                    ↓
                                  IndirectDraw
```

`VisibleList` 在 GPU 内存中，CPU 不读——**不需要 CPU 级的多缓冲**。GPU 端通过 fence + RingBuffer 管理内存复用，这已经是 `FrameResourceManager` 的能力范畴。

#### 3.2.5 多线程物理 — 可能性低

如果 Physics 在 worker 线程上并行写入 TransformComponent：

```
Physics Worker 0: Entity 0-999    Physics Worker 1: Entity 1000-1999
       ↓                                   ↓
  写 Transform.position              写 Transform.position
  (线程私有区块)                      (线程私有区块)
       ↓                                   ↓
  ────────── 同步屏障（TaskExecutor 内 barrier）──────────
       ↓
  PreRender 读 Transform（已稳定）
```

**不需要 FrameDriver 级别的多缓冲。** Physics 的线程同步在 `Update` 阶段内部通过 `TaskExecutor` 的 barrier 完成，不跨 TaskPhase。

#### 3.2.6 材质参数动态更新 — 可能性低

如果材质参数需要频繁更新：直接写 upload buffer 即可，只有更新量达到每帧上千个材质时才需要双缓冲。**当前阶段不需要。**

### 3.3 多缓冲场景决策矩阵

| 场景 | 需要多缓冲？ | 机制 | 优先级 |
|:-----|:-----------|:-----|:-------|
| **SwapChain** | ✅ 已存在 | DXGI 自动 | — |
| **FrameResource RingBuffer** | ✅ 已存在 | GPU Fence | — |
| **FrameSync（逻辑/渲染）** | ✅ 已存在 | L4 回调 | — |
| **CullingResult / LODResult** | ❌ 建议移除 | 不需要 | **当前** |
| **异步资产流式加载** | ✅ 已被事件系统覆盖 | MessageDispatcher | **无需额外实现** |
| **网络状态同步** | ✅ 已被事件系统覆盖 | MessageDispatcher | **无需额外实现** |
| **输入预测与回滚** | ✅ 需要 N 帧环形缓冲 | InputBuffer[N] + 快照 | **中** |
| **GPU Driven Culling** | ❌ 不需要 | GPU 端 fence 管理 | **低** |
| **多线程物理** | ❌ 不需要 | TaskPhase 内 barrier | **低** |
| **材质参数更新** | ❌ 不需要 | 直接写 upload buffer | **低** |

### 3.4 核心判断

- **CullingResult/LODResult 是「假多缓冲」**：它们只是阶段间数据传递的临时分配，不是真正的多缓冲（没有并发生产者和消费者）。内联方案直接移除。
- **真正的多缓冲只在「跨线程交互」处出现**：网络↔主线程、后台加载↔主线程。事件系统 `MessageDispatcher` 已经统一覆盖了这些场景。
- **渲染管线内部的「多缓冲」已由 FrameResourceManager 的 RingBuffer 统一覆盖**：Builder、Renderer 等消费 GPU 内存的地方统一走 `Allocate("ObjectCB", ...)` / `Allocate("Instance", ...)`，不需要各自独立实现。
- **FrameSync 不是多缓冲场景的终点，而是多缓冲的协调点**：它提供了安全的回调时机让各模块做自己的缓冲交换，但不负责每个模块的内部逻辑。

---

## 四、MeshComponent 的 localBounds

### 必要性

`MeshComponent` 中需要 `localBounds` 字段，用于剔除。当前已经存在：

```cpp
struct MeshComponent {
    Resource::LODMeshHandle lodMeshHandle;
    Resource::MaterialHandle materialHandle;
    uint32_t indexCount, startIndex;
    int32_t startVertex;
    Math::BoundingVolumeVariant localBounds; // ✅ 已存在
    bool IsValid() const;
};
```

### 剔除时的世界空间变换

```cpp
// CullingSystem / Builder 中
BoundingAABB worldBounds;
worldBounds.min = transform.position + localBounds.min;
worldBounds.max = transform.position + localBounds.max;
```

### 大型引擎的分层包围盒

| 层级 | 粒度 | 更新频率 |
|:-----|:------|:---------|
| **粗粒度** — 八叉树/BVH | Chunk | 静态/很少 |
| **中粒度** — Entity 组件 | 单个物体 | 每帧（随变换更新） |
| **细粒度** — GPU Compute | 实例 | 每帧 |

当前阶段：物体级别的 AABB 视锥剔除即可。八叉树是后续优化。

---

## 五、当前阶段建议

### 优先：构建器内联剔除 + LOD

改动范围：
1. 删除 `CullingSystem` 的 ECS System 注册
2. 删除 `LODSystem` 的 ECS System 注册
3. 将 `FrustumCull()` 和 `PickLOD()` 实现为独立工具函数（放在 `Renderer/Core/` 下）
4. 各 Builder 的 `BuildTyped()` 循环内直接调用这两个函数
5. 移除 `PreCulling` / `PostCulling` TaskPhase（如果其他 System 不再依赖它们）
6. 验证 `VisibleRaycaster` 是否独立工作（不从 CullingResult 读数据）

### 后续：统一上传阶段

Builder 内联后，`Allocate("ObjectCB", ...)` / `Allocate("Instance", ...)` 仍在 Builder 循环内被调用。如果要实现 Builder 并发，需要：
- 每个线程有独立的临时缓冲区
- 或在 PreRender 之前统一计算每帧需要的总内存量，分配一次，Builder 只从预分配区域取用

这对应 todo.md#14「构建器并发」+ 统一资源上传阶段。
