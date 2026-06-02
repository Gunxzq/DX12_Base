看完你的代码，数据流动非常清晰。让我用一张图总结你当前的异步地形加载流程：

## 当前数据流动

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  GameWorld::LoadTerrainAsync()  [主线程]                                    │
│  - 生成 requestId                                                           │
│  - 创建 TerrainLoadDataPtr (shared_ptr)                                     │
│  - 存储到 AssetDataManager（key: "TerrainLoadData_X"）                       │
│  - BackgroundExecutor::Submit(TerrainLoadTask)                              │
└─────────────────────────────────────────────────────────────────────────────┘
                                      ↓
┌─────────────────────────────────────────────────────────────────────────────┐
│  BackgroundExecutor::线程池 [Worker 线程]                                   │
│  TerrainLoadTask::execute()                                                 │
│  - AssetLoader::LoadTerrainFromFile()  ← 文件 I/O                           │
│  - 生成顶点/索引数据                                                         │
│  - 填充 shared_ptr<TerrainLoadData>                                         │
│  - PostEvent(TerrainLoaded, requestId)  ← P4_Background                     │
└─────────────────────────────────────────────────────────────────────────────┘
                                      ↓
┌─────────────────────────────────────────────────────────────────────────────┐
│  MessageDispatcher [事件系统]                                               │
│  - TerrainLoaded 进入 P4 桶                                                 │
│  - 下一帧 TaskGraphBuilder::BuildFromBuckets 收集                           │
└─────────────────────────────────────────────────────────────────────────────┘
                                      ↓
┌─────────────────────────────────────────────────────────────────────────────┐
│  TerrainGPUCreateSystem [Render 线程]                                       │
│  - 从 AssetDataManager 取出 TerrainLoadDataPtr                              │
│  - GpuResourceManager::CreateBuffer() 创建 VB/IB                            │
│  - GeometryResourceManager::RegisterTriangleMesh()                          │
│  - 上传纹理（如果尚未加载）                                                   │
│  - PostEvent(TerrainGeometryUploaded, requestId, handle)  ← P4_Background   │
└─────────────────────────────────────────────────────────────────────────────┘
                                      ↓
┌─────────────────────────────────────────────────────────────────────────────┐
│  TerrainUploadCompletionSystem [Main 线程]                                  │
│  - 检查围栏（但这里没有真正等待，只是读取 completedFence 打印）               │
│  - PostEvent(TerrainReady, requestId, handle)  ← P3_Low                     │
└─────────────────────────────────────────────────────────────────────────────┘
                                      ↓
┌─────────────────────────────────────────────────────────────────────────────┐
│  TerrainCombineSystem [Main 线程]                                           │
│  - 注册 LODMesh                                                             │
│  - 创建 ECS 实体，添加 MeshComponent                                        │
│  - 下一帧 RenderItemBuilder 拾取 → 渲染                                     │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 关键观察

| 组件 | 线程 | 事件优先级 | 阶段 |
| :--- | :--- | :--- | :--- |
| `TerrainLoadTask` | Worker | `P4_Background` | 文件 I/O |
| `TerrainGPUCreateSystem` | Render | `P4_Background` | GPU 资源创建 |
| `TerrainUploadCompletionSystem` | Main | `P3_Low` | 围栏检查（无实际等待） |
| `TerrainCombineSystem` | Main | `P3_Low` | ECS 实体创建 |

## 问题定位

你看到的 **"第一帧上传，第四帧才显示"** 的延迟来源：

1. **事件优先级低**：`P4_Background` 和 `P3_Low` 事件在 `TaskGraphBuilder::BuildFromBuckets` 中被处理，但每帧有预算限制（硬限制 1024 条，时间预算 1ms），可能被延迟处理

2. **围栏没有真正等待**：`TerrainUploadCompletionSystem` 只是读取 `GetCompletedFence()` 打印日志，没有实际等待围栏。但由于 `Flush()` 同步上传的存在，围栏实际上在第 1 帧末就已经完成了

3. **事件跨帧传播**：`TerrainGeometryUploaded` 在 `Render` 阶段发送，但 `TerrainUploadCompletionSystem` 在 `Update` 阶段处理，天然延迟 1 帧

4. **`TerrainCombineSystem` 的 `phase = TaskPhase::Render`**：但 `interestedMessages` 是 `TerrainReady`，而 `TerrainReady` 在 `Update` 阶段发出，所以 `Render` 阶段的 System 要等到下一帧的 `Render` 阶段才能处理

## 事件系统的优劣

你的事件系统设计是**正确的**，它提供了：
- 线程安全的跨线程通信
- 优先级驱动的消息调度
- 批量处理预算控制

但在高频、低延迟的场景下，事件驱动会引入**可预测的帧级延迟**（通常 1-2 帧）。这是解耦的代价，对于资源加载来说通常可以接受。

## 优化建议

1. **提高优先级**：将 `TerrainGeometryUploaded` 和 `TerrainReady` 改为 `P2_Normal`
2. **调整 System 阶段**：让所有相关 System 在 `Render` 阶段执行，避免跨阶段延迟
3. **移除 `Flush()`**：初始化中的 `Flush()` 是主要卡顿源，改为异步上传
4. **批量处理**：`TaskGraphBuilder` 的预算限制可以适当提高