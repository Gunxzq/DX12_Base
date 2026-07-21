# 引擎核心 API 约束与调用规范

> **定位**：内部开发参考文档，按 Bootstrap 模块维度组织，约束 API 使用行为。
> **与 Blog 分工**：Blog 负责架构设计，本文档负责具体调用约束、互斥规则、错误模式。
> **版本**：对应 Bootstrap 当前持有的模块集合。

---

## 目录

1. [基础设施域](#1-基础设施域)
   - 1.1 ConfigManager
   - 1.2 Logger
   - 1.3 Window
   - 1.4 D3D12DeviceContext
   - 1.5 GameTimer
   - 1.6 DebugUI
2. [调度与数据域](#2-调度与数据域)
   - 2.1 FrameDriver
   - 2.2 BackgroundExecutor
   - 2.3 ECS::Registry
   - 2.4 MessageDispatcher
   - 2.5 SharedDataStore
3. [渲染域](#3-渲染域)
   - 3.1 FrameResourceManager
   - 3.2 CameraManager
   - 3.3 CullingSystem
   - 3.4 LODSystem
   - 3.5 VisibleRaycaster
   - 3.6 ReflectionProbeManager
   - 3.7 AmbientOcclusionManager
4. [资源域](#4-资源域)
   - 4.1 DescriptorHeapCollection
   - 4.2 TextureManager
   - 4.3 GeometryResourceManager
   - 4.4 MaterialManager
   - 4.5 SkeletonManager
   - 4.6 AssetManager
   - 4.7 GpuResourceManager
   - 4.8 RenderTargetPool
   - 4.9 DepthStencilPool
5. [平台域](#5-平台域)
   - 5.1 InputManager
6. [全局约束](#6-全局约束)

---

## 1. 基础设施域

### 1.1 ConfigManager

**获取方式**：`GameContext::Config`（单例 `ConfigManager::GetInstance()`）

**生命周期**：
```
Initialize(configDir) → 使用中 → Shutdown() → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | Bootstrap 初始化阶段，第 1 个初始化的模块 | 必须在 Logger 之前 |
| 读配置 | 随时 | 返回常量引用，线程安全 |
| 写配置 | 运行时 | 写后标记脏，触发节流自动保存 |
| Save | 关闭前 | 强制保存 |
| Reload | 热重载触发 | 先检查脏标记，有未保存修改时警告 |
| Subscribe | 初始化阶段 | 注册配置变更回调 |
| Shutdown | Bootstrap 清理阶段 | 强制保存后释放资源 |

**常见错误**：
- ❌ 在 Logger 初始化之前访问 ConfigManager（此时配置未加载）
- ❌ 直接操作 JSON 文件而非通过 ConfigManager 接口
- ❌ 在析构函数中调用 Save（可能已无文件系统访问能力）

**互斥操作**：
- `Initialize` 与 `Shutdown` 互斥，不可重复调用
- 写操作与 `Reload` 同时触发时，`Reload` 会覆盖未保存的修改

---

### 1.2 Logger

**获取方式**：`GameContext::Logging`（单例 `EngineLogger::GetInstance()`）

**生命周期**：
```
Init(logConfig) → 使用中 → Shutdown() → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Init | Bootstrap 初始化阶段，第 2 个模块 | 依赖 ConfigManager |
| 日志输出 | 随时 | 异步线程处理 I/O，不阻塞主线程 |
| Shutdown | Bootstrap 清理阶段 | 等待异步队列完成 |

**常见错误**：
- ❌ 在 Init 之前调用日志输出（使用 EarlyLog 兜底）
- ❌ 在每帧循环中高频输出 Info/Debug 日志（应使用节流策略）
- ❌ 在 Shutdown 后调用日志输出（未定义行为）

**注意**：
- Release 构建禁用控制台输出，仅文件日志
- 日志队列满时（overflow）Debug 模式阻塞，Release 模式丢弃旧日志

---

### 1.3 Window

**获取方式**：`GameContext::Window`（Bootstrap 通过 `unique_ptr` 拥有）

**生命周期**：
```
Create(config) → Show() → 使用中 → DestroyWindow → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Create | Bootstrap 初始化阶段，第 3 个模块 | 依赖 ConfigManager |
| ProcessMessages | 每帧主循环开头 | 使用 PeekMessage 非阻塞轮询 |
| ShouldClose | 每帧检查 | 收到 WM_CLOSE 后返回 true |
| SetCursorCapture | 运行时 | 切换光标捕获模式 |
| SetFullscreen | 运行时 | 切换全屏模式，触发 ResizeEvent |
| DestroyWindow | 销毁阶段 | 自动在析构函数中调用 |

**常见错误**：
- ❌ 在 Window 创建之前访问 D3D12DeviceContext（需要 HWND）
- ❌ 在非主线程调用 ProcessMessages（Win32 消息只能在主线程处理）
- ❌ 在拖拽调整窗口期间触发 Resize 事件（应在 WM_EXITSIZEMOVE 时统一触发）
- ❌ **在 FrameDriver::Tick() 外部额外调用 ProcessMessages()**（FrameDriver::Tick() 内部已包含 BeginFrame → ProcessMessages → InputMgr->Update 的完整输入处理链路。外部额外调用会导致 BeginFrame 将已处理的消息增量数据——特别是鼠标 delta 和滚轮——清空，表现为右键旋转/滚轮缩放失效。详见 `Docs/bugs/BugFix_Editor_DoubleProcessMessages.md`）

**注意**：
- 窗口尺寸变化时通过 MessageDispatcher 发送 `WindowResizeEvent`，渲染器订阅后重建交换链
- 拖拽窗口边缘时，WM_SIZE 频繁触发，但仅在 WM_EXITSIZEMOVE 时发送一次 ResizeEvent
- 全屏切换使用 `SetWindowLongPtr` 修改窗口样式，不创建新窗口

---

### 1.4 D3D12DeviceContext

**获取方式**：`GameContext::DeviceContext`（Bootstrap 通过 `unique_ptr` 拥有）

**生命周期**：
```
Initialize(params) → 使用中 → OnResize(width, height) |
                                    FlushCommandQueue() | → Shutdown()
                                    BeginFrame/EndFrame   |
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | Bootstrap 初始化阶段，第 4 个模块 | 依赖 Window（HWND） |
| OnResize | 窗口尺寸变化时 | 重建交换链，更新视口 |
| BeginFrame | 每帧渲染开始 | 开始新命令列表 |
| EndFrame | 每帧渲染结束 | 提交命令列表 |
| FlushCommandQueue | 同步点 | 等待 GPU 完成所有命令 |
| FlushCommandQueue(type) | 按类型同步 | 等待指定队列完成 |
| InitDepthSRV | 初始化后 | 创建深度缓冲 SRV（供后处理 Pass） |
| RebuildDepthSRV | Resize 时 | 重建深度 SRV（释放旧槽位 + 创建新 SRV） |
| SetDescriptorHeapCollection | 初始化后 | 关联描述符堆集合 |
| Shutdown | 清理阶段 | 等待 GPU 完成，释放资源 |

**常见错误**：
- ❌ Initialize 前访问 GetDevice() 等任何方法
- ❌ OnResize 中不检查尺寸是否真实变化（窗口初始化过程中重复重建）
- ❌ 在 BeginFrame 之前执行渲染操作
- ❌ 在 Shutdown 后访问任何 D3D12 资源
- ❌ 多线程中同时调用 FlushCommandQueue（CommandManager 内部有同步，但外部调用需谨慎）

**互斥操作**：
- Initialize 与 Shutdown 互斥
- BeginFrame 与 EndFrame 必须成对调用，不可嵌套
- FlushCommandQueue 与 BeginFrame 之间需确保命令列表已提交完成

**重要约束**：
- DepthStencilFormat 由配置文件 `renderer.json` 中的 `depthStencilFormat` 字段控制
- 所有渲染器的 PSO 的 DSVFormat 必须与 GetDepthStencilFormat() 一致，否则报 `ERROR #615: DEPTH_STENCIL_FORMAT_MISMATCH_PIPELINE_STATE`
- 深度格式在 `D3D12_DEPTH_STENCIL_VIEW_DESC::Format` 中也必须使用同一值

---

### 1.5 GameTimer

**获取方式**：`GameContext::MainTimer`（Bootstrap 通过 `unique_ptr` 拥有）

**生命周期**：
```
创建 → Start() → 每帧 Tick() → ... → Stop() → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| 创建 | CreateContext 阶段 | 与窗口创建同时 |
| Start | 主循环开始前 | 启动计时 |
| Tick | 每帧开头 | 更新 DeltaTime |
| GetDeltaTime | 逻辑更新中 | 获取帧间隔时间 |
| GetTotalTime | 随时 | 获取总运行时间 |
| Stop | 销毁前 | 停止计时 |

**常见错误**：
- ❌ Tick 在每帧被多次调用（DeltaTime 会被重置）
- ❌ 暂停后使用 GetDeltaTime 计算物理（应使用独立计时器）

---

### 1.6 DebugUI

**获取方式**：`DebugUI::DebugUIManager::Get()`（单例），通过 `SetGameContext` 注入 Context

**生命周期**：
```
Initialize(hwnd) → InitDX12Backend(device, queue, frameCount, format) → SetGameContext(ctx) → 使用中 → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize(hwnd) | 初始化阶段，D3D12 之后 | 初始化 ImGui Win32 后端 |
| InitDX12Backend | Initialize 之后 | 初始化 ImGui DX12 后端 |
| ApplyDarkTheme | 初始化后 | 可选：应用暗色主题 |
| SetShowMenuBar | 初始化后 | 可选：显示菜单栏 |
| SetGameContext | CreateContext 阶段 | 关联 GameContext |

**常见错误**：
- ❌ 在 InitDX12Backend 之前调用 ImGui 渲染相关 API
- ❌ 在 Release 构建中包含 DebugUI 代码（应通过条件编译排除）

---

## 2. 调度与数据域

### 2.1 FrameDriver

**获取方式**：`GameContext::FrameDriver`（由基础设施层创建，Bootstrap 持有裸指针）

**生命周期**：
```
InitializeSchedulerContext(registry, deviceContext) → Initialize(threadCount) → SetGameContext(ctx) → 每帧 Tick() → Stop() → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| InitializeSchedulerContext | 初始化阶段 | 注册 System、创建任务图 |
| Initialize | InitializeSchedulerContext 之后 | 创建工作线程池 |
| SetGameContext | CreateContext 阶段 | 关联 GameContext |
| Tick | 每帧主循环 | 驱动帧循环各阶段 |
| Stop | 关闭前 | 停止帧循环 |
| RegisterFrameSyncCallback | 初始化阶段 | 注册帧同步回调 |
| RegisterImmediateCallback | 初始化阶段 | 注册即时回调 |
| RegisterSceneDataCallback | 初始化阶段 | 注册场景数据上传回调 |
| SubmitRenderCommand | 渲染阶段 | 提交渲染命令列表 |

**常见错误**：
- ❌ Tick 在 BackgroundExecutor::Tick() 之前调用（异步加载回调不触发）
- ❌ SetGameContext 之前调用 Tick（Context 为空）
- ❌ 在 System 执行期间注册回调（非线程安全）
- ❌ 忘记调用 SetGameContext（FrameDriver 持有 GameContext 裸指针用于后续访问）
- ❌ **在 Tick() 外部额外调用 Window::ProcessMessages()**（Tick() 内部已按正确顺序处理输入：BeginFrame → ProcessMessages → InputMgr->Update。外部额外调用会破坏输入增量数据流，导致鼠标 delta 被清空。详见 `Docs/bugs/BugFix_Editor_DoubleProcessMessages.md`）

**互斥操作**：
- Initialize 与 Stop 互斥
- 回调注册与帧执行互斥（应在帧外注册）

---

### 2.2 BackgroundExecutor

**获取方式**：`GameContext::BackgroundExecutor`（Bootstrap 通过 `unique_ptr` 拥有）

**生命周期**：
```
创建(threadCount) → SetCommandManager(cmdMgr) → 使用中 → 每帧 Tick() → WaitAll() → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| 创建 | Bootstrap 初始化阶段，FrameDriver 之后 | 创建后台线程池 |
| SetCommandManager | 创建后 | 关联命令管理器 |
| Submit(task) | 随时（后台线程安全） | 提交 CPU 任务 |
| SubmitGraph(graph) | 随时（后台线程安全） | 提交有依赖的任务图 |
| SubmitLoadTask(task) | 随时（后台线程安全） | 提交加载任务（CPU→GPU→回调） |
| RegisterGpuWork(item) | 后台线程 | 注册 GPU 工作项 |
| Tick | **每帧主循环，FrameDriver::Tick() 之前** | 提交 GPU 工作、检查完成、触发回调 |
| WaitAll | 关闭前 | 等待所有任务完成 |

**关键约束**：
```
主循环执行顺序：
  1. MainTimer::Tick()
  2. BackgroundExecutor::Tick()  ← 必须先调用
  3. FrameDriver::Tick()          ← 后调用
```

**常见错误**：
- ❌ 主循环中忘记调用 Tick（异步加载完成回调不触发，Skybox 等资源不可用）
- ❌ Tick 在 FrameDriver::Tick() 之后调用（GPU 提交顺序错误）
- ❌ 在主线程调用 Submit（Submit 是线程安全的，但设计上用于后台线程）
- ❌ 在 Shutdown 前不调用 WaitAll（后台线程可能仍在执行）

---

### 2.3 ECS::Registry

**获取方式**：`GameContext::Registry`（Bootstrap 通过 `unique_ptr` 拥有）

**生命周期**：
```
创建 → 注册 System → 使用中 → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| 创建 | Bootstrap 初始化阶段 | 创建空 Registry |
| 注册 System | 初始化阶段 | 注册到指定执行阶段 |
| 创建/销毁实体 | 运行时 | 主线程安全 |
| 添加/移除组件 | 运行时 | 主线程安全 |
| System 执行 | 帧循环中 | 由 FrameDriver 调度 |

**常见错误**：
- ❌ 在 System 执行期间进行内存分配（违反多线程安全约束）
- ❌ 渲染 System 中修改组件数据（渲染 System 只能录制命令）
- ❌ 用消息模拟每帧驱动（高频连续过程走 AlwaysRun，低频事件走 WithMessage）

---

### 2.4 MessageDispatcher

**获取方式**：`GameContext::Dispatcher`（单例 `MessageDispatcher::GetInstance()`）

**生命周期**：
```
Init() → 使用中 → Shutdown() → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Init | Bootstrap 初始化阶段 | 初始化消息队列 |
| PostEvent | 随时 | 发布事件到消息队列 |
| Shutdown | 清理阶段 | 清理消息队列 |

---

### 2.5 SharedDataStore

**获取方式**：`Core::SharedDataStore::GetInstance()`（单例）

**生命周期**：
```
Preallocate(capacity) → Initialize(config) → 使用中 → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Preallocate | 初始化阶段，D3D12 之后 | 预分配数据池 |
| Initialize | Preallocate 之后 | 从配置初始化 |
| 读写 | 运行时 | 通过键值对访问共享数据 |

---

## 3. 渲染域

### 3.1 FrameResourceManager

**获取方式**：`GameContext::FrameResourceManager`（Bootstrap 内联成员）

**生命周期**：
```
Initialize(device, descriptorHeaps, config) → 每帧 BeginFrame(completedFence, nextFence) → Shutdown()
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | Bootstrap 初始化阶段，D3D12 之后 | 创建 RingBuffer |
| BeginFrame | 每帧渲染开始前 | 回收已完成帧的 RingBuffer 空间 |
| Allocate(name, data, size) | 渲染阶段 | 分配 RingBuffer 空间 |
| UpdatePassConstants | 渲染阶段 | 拷贝 PassCB 到 GPU |
| AllocateTemporarySrvSlot | 渲染阶段 | 分配临时 SRV 槽位 |
| FreeTemporarySrvSlot | 渲染阶段 | 释放临时 SRV 槽位（带 fence） |
| Shutdown | 清理阶段 | 释放所有资源 |

**常见错误**：
- ❌ 在 BeginFrame 之前调用 Allocate（RingBuffer 未回收）
- ❌ Allocate 返回 0 地址时未检查（RingBuffer 空间不足）
- ❌ 忘记调用 UpdatePassConstants（PassCB 数据未更新到 GPU）

---

### 3.2 CameraManager

**获取方式**：`GameContext::CameraMgr`（单例 `CameraManager::GetInstance()`）

**生命周期**：
```
Initialize(width, height) → 使用中 → 更新 → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | CreateContext 阶段 | 初始化主摄像机，需要窗口尺寸 |
| 更新摄像机参数 | 每帧 EarlyUpdate 阶段 | 更新位置/朝向/FOV |
| 获取矩阵 | 渲染阶段 | 获取视图/投影矩阵 |

**常见错误**：
- ❌ 在 Initialize 之前访问摄像机矩阵（矩阵未初始化）
- ❌ Initialize 在 Bootstrap 初始化阶段之前调用（窗口尺寸未就绪）

---

### 3.3 CullingSystem

**获取方式**：`GameContext::CullingSystem`（Bootstrap 内联成员）

**执行阶段**：PostCulling

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| 执行 | 每帧 PostCulling 阶段 | 由 FrameDriver 调度 |

**常见错误**：
- ❌ 在 Culling 之前修改实体变换但未更新包围盒
- ❌ 假设剔除结果在下一帧仍然有效

---

### 3.4 LODSystem

**获取方式**：`GameContext::LODSystem`（Bootstrap 内联成员）

**生命周期**：
```
SetLODConfig(config) → SetCameraManager(camMgr) → SetGeometryManager(geoMgr) → 使用中
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| SetLODConfig | 初始化阶段 | 设置 LOD 配置 |
| SetCameraManager | 初始化阶段 | 关联摄像机管理器 |
| SetGeometryManager | 初始化阶段 | 关联几何体资源管理器 |
| 执行 | 每帧 PostCulling 阶段 | 由 FrameDriver 调度 |

**常见错误**：
- ❌ SetCameraManager 之前执行 LOD 计算（缺少视点位置）
- ❌ LOD 级别与网格索引不匹配

---

### 3.5 VisibleRaycaster

**获取方式**：`GameContext::VisibleRaycaster`（Bootstrap 内联成员）

**生命周期**：
```
Initialize(registry) → 使用中 → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | CreateContext 阶段 | 关联 ECS Registry |
| 射线检测 | 运行时 | 结果存入 GameContext::raycastResult |

---

### 3.6 ReflectionProbeManager

**获取方式**：`GameContext::ReflectionProbeMgr`（Bootstrap 内联成员）

**生命周期**：
```
Initialize(device, descriptorHeaps) → 使用中 → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | CreateContext 阶段 | 初始化探针系统 |
| 重新捕获 | 探针位置变化时 | 标记脏，下一帧重新捕获 |

---

### 3.7 AmbientOcclusionManager

**获取方式**：`GameContext::AmbientOcclusionMgr`（Bootstrap 内联成员）

**生命周期**：
```
Initialize(device, descriptorHeaps, width, height) → 每帧执行 → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | CreateContext 阶段 | 初始化 SSAO |
| 执行 | 每帧渲染阶段 | 计算 AO |

**常见错误**：
- ❌ 在深度缓冲 SRV 未初始化时执行 SSAO
- ❌ Resize 时未重新创建 AO 资源

---

## 4. 资源域

### 4.1 DescriptorHeapCollection

**获取方式**：`GameContext::DescriptorHeaps`（Bootstrap 内联成员）

**生命周期**：
```
Initialize(device, configs, mode) → 添加分区 → 使用中 → Shutdown()
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | Bootstrap 初始化阶段，D3D12 之后 | 创建物理堆 |
| AddPartition | Initialize 之后，**所有分配之前** | 注册分区 |
| Allocate(tag, partition) | 运行时 | 分配描述符槽位 |
| Free(tag, partition, index, fence) | 运行时 | 释放描述符槽位 |
| Reclaim(tag, partition, completedFence) | 每帧 | 回收已完成 fence 的槽位 |
| Shutdown | 清理阶段 | 释放所有堆 |

**关键约束**：
```
多堆模式下（HeapMode::Multi）：
  - 所有 GetPartitionCpuHandle / GetPartitionGpuHandle 必须显式传递 HeapTag
  - 禁止依赖 HeapTag::Default 默认参数
  - CPU Handle 和 GPU Handle 必须指向同一个堆

初始化顺序：
  - 非 Default 堆域的分区必须在异步加载和主循环开始前注册完毕
  - 特别是 EditorViewport 标签的 Texture 分区必须在 EditorScene::LoadDefaultScene() 之前
```

**常见错误**：
- ❌ 多堆模式下遗漏 HeapTag 参数（使用默认 Default 堆，导致描述符写入错误堆）
- ❌ CPU Handle 和 GPU Handle 指向不同堆（CreateSRV 写入 Default 堆，绑定 EditorViewport 堆）
- ❌ 在 AddPartition 之前调用 Allocate（分区不存在，返回 UINT32_MAX）
- ❌ 重复添加已存在的分区（OnResize 中重复注册，产生 `reservedStart != baseOffset` 警告）

---

### 4.2 TextureManager

**获取方式**：`GameContext::TextureMgr`（Bootstrap 内联成员）

**生命周期**：
```
Initialize(device, descriptorHeaps) → 使用中 → Shutdown()
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | Bootstrap 初始化阶段 | 初始化纹理管理器 |
| RegisterTexture | 加载纹理时 | 注册纹理，返回句柄 |
| Retain(handle) | ECS 组件拷贝时 | 增加引用计数 |
| Release(handle, fence) | 不再使用时 | 减少引用计数，归零加入待释放 |
| Reclaim(completedFence) | 每帧 | 回收待释放纹理 |
| GetSRV(handle) | 渲染时 | 获取 SRV GPU 句柄 |
| IsValid(handle) | 使用前 | 验证句柄有效性 |
| Shutdown | 清理阶段 | 释放所有纹理 |

**常见错误**：
- ❌ Retain/Release 不对称（引用计数泄漏或提前释放）
- ❌ 使用已释放的纹理句柄（应先调用 IsValid 验证）
- ❌ 直接调用 GpuResourceManager::Release（应由 GpuResourceManager 统一管理）

---

### 4.3 GeometryResourceManager

**获取方式**：`GameContext::GeometryResourceManager`（Bootstrap 内联成员）

**生命周期**：
```
Initialize(capacity) → 使用中 → Shutdown()
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | Bootstrap 初始化阶段 | 初始化几何体管理器 |
| RegisterGeometry(mesh) | 加载网格时 | 注册几何体，返回句柄 |
| GetGeometry<T>(handle) | 使用网格时 | 类型安全查询 |
| GetBounds(handle) | 剔除/碰撞时 | 获取包围盒 |
| Retain(handle) | ECS 组件拷贝时 | 增加引用计数 |
| Release(handle, fence) | 不再使用时 | 减少引用计数 |
| Reclaim(completedFence) | 每帧 | 回收待释放几何体 |
| Shutdown | 清理阶段 | 释放所有几何体 |

**常见错误**：
- ❌ GetGeometry 时模板参数与实际注册类型不匹配（std::get_if 返回 nullptr）
- ❌ Retain/Release 不对称
- ❌ 使用已释放的句柄

---

### 4.4 MaterialManager

**获取方式**：`GameContext::MaterialMgr`（Bootstrap 内联成员）

**生命周期**：
```
Initialize(capacity) → 使用中 → Shutdown()
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | Bootstrap 初始化阶段 | 初始化材质管理器 |
| RegisterMaterial | 加载材质时 | 注册材质，返回句柄 |
| AcquireMaterial(hashId) | 引用材质时 | 按资产 ID 获取句柄，引用 +1 |
| ReleaseMaterial(handle) | 不再使用时 | 释放引用 |
| GetMaterial(handle) | 渲染时 | 获取材质数据 |
| IsDirty / ClearDirty | 每帧渲染前 | 检查脏标记 |
| GetGPUMaterialList | 渲染时 | 获取 GPU 常量列表 |
| Shutdown | 清理阶段 | 释放所有材质 |

**常见错误**：
- ❌ AcquireMaterial 后忘记 ReleaseMaterial（引用泄漏）
- ❌ 渲染前不清除脏标记（重复上传 GPU 数据）
- ❌ 使用无效的 MaterialHandle

---

### 4.5 SkeletonManager

**获取方式**：`GameContext::SkeletonMgr`（Bootstrap 内联成员）

**生命周期**：
```
Initialize(capacity) → 使用中 → Shutdown()
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | Bootstrap 初始化阶段 | 初始化骨骼管理器 |
| 注册骨骼 | 加载骨骼数据时 | 返回骨骼句柄 |
| Shutdown | 清理阶段 | 释放所有骨骼数据 |

---

### 4.6 AssetManager

**获取方式**：`Resource::AssetManager::GetInstance()`（单例），通过 `GameContext` 间接访问

**生命周期**：
```
Initialize(deviceContext, backgroundExecutor, geoMgr, matMgr, texMgr, descriptorHeaps) → 使用中 → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | Bootstrap 初始化阶段，BackgroundExecutor 之后 | 初始化资产管理器 |
| LoadBatch | 运行时 | 提交批量加载任务 |
| 查询进度 | 加载中 | 获取加载进度 |
| 销毁 | 清理阶段 | 释放所有资源 |

**常见错误**：
- ❌ Initialize 之前提交加载任务（AssetManager 内部组件未就绪）
- ❌ BackgroundExecutor 未 Tick 时等待加载完成（加载不会推进）

---

### 4.7 GpuResourceManager

**获取方式**：`Resource::GpuResourceManager::GetInstance()`（单例）

**生命周期**：
```
Initialize() → 使用中 → Update(completedFence) → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | Bootstrap 初始化阶段，DescriptorHeap 之后 | 初始化 GPU 资源管理器 |
| 分配资源 | 运行时 | 创建 GPU 资源 |
| Update | 每帧 | 通过 fence 回调释放已完成的资源 |
| 销毁 | 清理阶段 | 释放所有 GPU 资源 |

**关键约束**：
```
GpuResourceManager 与各资源管理器是协作关系，非包含关系：
  - 资源管理器（TextureManager、GeometryResourceManager 等）只管理自己的槽位
  - 禁止直接调用 GpuResourceManager::Release
  - GPU 资源实际释放由 GpuResourceManager 统一管理
  - 通过 GpuWorkItem::uploadBufferHandles 或 Update 的 fence 回调完成
```

---

### 4.8 RenderTargetPool

**获取方式**：`GameContext::RenderTargetPool`（单例 `RenderTargetPool::GetInstance()`）

**生命周期**：
```
Initialize(device, descriptorHeaps) → 使用中 → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | Bootstrap 初始化阶段 | 初始化 RT 池 |
| 分配 RT | 渲染时 | 按需分配 |
| 归还 RT | 渲染完成后 | 归还到池中复用 |
| 销毁 | 清理阶段 | 释放所有 RT |

**关键约束**：
```
资源池角色：
  - 只负责资源生命周期（分配/释放）
  - 不追踪也不重置 GPU resource state
  - 使用资源的 System 必须自己管理 ResourceBarrier

对称屏障规则：
  - 每个 System 在自己录制的命令列表中完成入口转换
  - 渲染结束后做反向转换恢复到入口前的状态
  - 禁止仅做入口转换而遗漏出口转换
```

---

### 4.9 DepthStencilPool

**获取方式**：`GameContext::DepthStencilPool`（单例 `DepthStencilPool::GetInstance()`）

**生命周期**：
同 RenderTargetPool。

**调用规范**：同 RenderTargetPool 的分配/归还/屏障规则。

---

## 5. 平台域

### 5.1 InputManager

**获取方式**：`GameContext::InputMgr`（单例 `Input::InputManager::Get()`）

**生命周期**：
```
Initialize(configPath, isEditor) → 使用中 → 销毁
```

**调用规范**：
| 操作 | 时机 | 说明 |
|:-----|:-----|:-----|
| Initialize | Bootstrap 初始化阶段，ConfigManager 之后 | 加载输入配置 |
| 处理输入 | 每帧 | 由 Window 提供原始输入 |
| 查询输入状态 | 逻辑更新中 | 获取按键/鼠标状态 |

**常见错误**：
- ❌ Initialize 之前查询输入状态（输入映射未加载）
- ❌ 在窗口消息处理之前处理输入（原始输入未就绪）

---

## 6. 全局约束

### 6.1 初始化顺序

```
严格顺序：
(1) 项目路径 / ShaderUtils
(2) ConfigManager
(3) Logger
(4) InputManager
(5) Window
(6) D3D12DeviceContext
(7) DescriptorHeapCollection + GpuResourceManager + 分区注册
(8) DepthStencilPool + RenderTargetPool
(9) SharedDataStore
(10) MaterialManager + AssetLoader + TextureManager
(11) FrameResourceManager
(12) GeometryResourceManager + SkeletonManager
(13) DebugUI
(14) MessageDispatcher
(15) ECS::Registry
(16) FrameDriver
(17) BackgroundExecutor
(18) AssetManager
(19) GameNetworkingSockets
--- CreateContext ---
(20) CameraManager::Initialize
(21) ReflectionProbeManager::Initialize
(22) AmbientOcclusionManager::Initialize
(23) VisibleRaycaster::Initialize
(24) FrameDriver::SetGameContext
(25) DebugUI::SetGameContext + AutoRegisterToFrameDriver
```

### 6.2 主循环顺序

```
每帧严格顺序：
(1) MainTimer::Tick()
(2) BackgroundExecutor::Tick()  ← 异步加载推进
(3) FrameDriver::Tick()         ← 帧循环调度（内部处理窗口消息 + 输入更新）
```

**关键约束**：
- `FrameDriver::Tick()` 内部已包含完整的消息处理链路：`InputMgr->BeginFrame()` → `Window->ProcessMessages()` → `InputMgr->Update()`
- **禁止在外部额外调用 `Window::ProcessMessages()`**——否则 `BeginFrame()` 会清空已处理的消息增量数据（鼠标 delta / 滚轮），导致右键旋转、滚轮缩放等依赖增量数据的输入失效
- 所有主循环（Game 端 / Editor 端）必须遵循此顺序，保持一致

### 6.3 对称屏障规则

```
所有 GPU ResourceBarrier 操作必须遵循：

入口屏障：资源 → 所需状态（System 开始）
渲染操作
出口屏障：所需状态 → 原始状态（System 结束）

此规则适用于所有资源（共享 RT、私有 RT、深度缓冲等），
不因资源是否被复用而豁免。
```

### 6.4 资源管理器协作规则

```
各资源管理器（TextureManager、GeometryResourceManager、MaterialManager 等）
与 GpuResourceManager 是协作模式，而非包含/依赖关系：

- 资源管理器：只管理自己的槽位/索引/引用计数
- 禁止直接调用 GpuResourceManager::Release
- GPU 资源释放：通过 GpuWorkItem::uploadBufferHandles 或 GpuResourceManager::Update 的 fence 回调
- 资源管理器在 Release/Reclaim 中释放自己的槽位即可
```

### 6.5 描述符堆 Tag 规则

```
多堆模式（HeapMode::Multi）下：

- 所有描述符操作必须显式传递 HeapTag
- 禁止依赖 HeapTag::Default 默认参数
- CPU Handle 和 GPU Handle 必须指向同一个堆
- 单堆模式（HeapMode::Single）下，所有 HeapTag 映射到同一堆，不暴露此问题
```

### 6.6 全屏 Quad 渲染规则

```
全屏 Quad / 后处理 Pass 渲染时：

- 必须在 Draw 前显式设置视口（RSSetViewports）和裁剪矩形（RSSetScissorRects）
- 新分配的命令列表无默认视口
- PSO 须设置 CullMode = NONE（SV_VertexID 生成的三角形绕序为逆时针）
```

### 6.7 根签名规则

```
所有渲染器的根签名须完整声明着色器使用的静态采样器
否则 PSO 创建失败且无编译错误提示

CD3DX12_ROOT_SIGNATURE_DESC 必须使用 Init() 方法初始化
禁止手动赋值 NumParameters/pParameters/Flags 等字段
{} 零初始化后手动赋值会导致 D3D12SerializeRootSignature 内部触发 std::bad_alloc

CD3DX12_ROOT_PARAMETER 必须显式设置 ShaderVisibility 字段
```

### 6.8 矩阵常量规则

```
所有从 C++ 端传入的 XMMATRIX 矩阵常量，在 HLSL 中必须声明为 row_major float4x4

float4x4（默认 column_major）与 C++ 端 XMMATRIX 的行主序存储不匹配
导致 mul 变换结果错误

非矩阵类型（float4 等）不能加 row_major/column_major 修饰
否则编译错误 X3077
```

### 6.9 System 调度规则

```
- 高频连续过程：走 AlwaysRun 常驻 System
- 低频离散事件：走 WithMessage 消息触发
- 禁止用消息模拟每帧驱动
- System 中不能进行内存分配
- 渲染 System 只能录制命令，不能修改组件数据
```

### 6.10 编辑器初始化顺序约束

```
Editor::Initialize() 中严格顺序：

(1) SkyboxManager::Initialize(heapTag)
(2) AddPartition(CBV_SRV_UAV, Texture, ..., EditorViewport)
(3) EditorScene::LoadDefaultScene()
(4) EditorViewport::Initialize()

EditorViewport::CreateRenderTarget 中只添加 RTV/DSV 分区
不允许重复添加 CBV_SRV_UAV / Texture 分区

Editor::Run() 主循环中：
(1) MainTimer::Tick()
(2) BackgroundExecutor::Tick()  ← 必须
(3) FrameDriver::Tick()
```

---

> **文档维护**：本文件应与 Bootstrap 持有的模块集合同步更新。
> 新增模块时，在此文件中添加对应的 API 约束章节。
> 模板：模块名 → 获取方式 → 生命周期 → 调用规范 → 常见错误 → 互斥操作 → 关键约束。


# 附录：资源申请与释放对等检查

> **定位**：补充约束，覆盖所有 GPU 资源生命周期的 Acquire↔Release 对等性检查。

---

## 1. 命令分配器（CommandAllocatorPool）

### 生命周期
```
GetAllocatorHandle<Type>(completedFence)   // 申请分配器
  → GetAllocator<Type>(handle)             // 获取 ID3D12CommandAllocator*
  → AcquireCommandListHandle<Type>(alloc)  // 申请命令列表
  → GetCommandList<Type>(handle)           // 获取 CommandList
  → cmd.Close()
  → SubmitRenderCommand(phase, cmdH)       // 提交
  → ReleaseAllocator<Type>(allocH, seq)    // 释放分配器（延迟到 fence seq 完成）
```

### 约束
- **每帧每个渲染 System 必须恰好完成一次完整的 Acquire→Release 周期**
- `ReleaseAllocator` 的 `fenceValue` 参数必须使用 `GetNextSequence()` 的返回值（不是 `GetCompletedFence`）
- 如果 System 中途 early-return（如资源未就绪），**不能已经获取了分配器**——必须在分配器获取之前 early-return
- 典型错误：先 `GetAllocatorHandle` 再条件判断 early-return，导致分配器泄漏

### 对等检查
```
System 入口：
  if (资源未就绪) return;          // ✅ 正确：分配器未获取
  auto allocH = GetAllocatorHandle(...);
  auto cmdH = AcquireCommandListHandle(...);
  ...
  cmd.Close();
  SubmitRenderCommand(phase, cmdH);
  ReleaseAllocator(allocH, seq);   // ✅ 必须执行
```

---

## 2. GPU 资源回收（GpuResourceManager / Pool）

### 生命周期
```
GpuResourceManager::CreateTexture2D / CreateBuffer
  → GpuResourceManager::Release(handle, fenceValue)  // 标记延迟释放
  → GpuResourceManager::Update(completedFence)        // 实际回收（每帧必须调用）

RenderTargetPool::Allocate(desc)
  → RenderTargetPool::Release(handle, fenceValue)     // 标记延迟释放
  → RenderTargetPool::Reclaim(completedFence)          // 实际回收（每帧必须调用）

DepthStencilPool::Allocate(desc)
  → DepthStencilPool::Release(handle, fenceValue)     // 标记延迟释放
  → DepthStencilPool::Reclaim(completedFence)          // 实际回收（每帧必须调用）

TextureManager::RegisterTexture
  → TextureManager::Reclaim(completedFence)            // 回收（每帧必须调用）

GeometryResourceManager::RegisterGeometry
  → GeometryResourceManager::Reclaim(completedFence)   // 回收（每帧必须调用）
```

### 约束
- **`Release` 只是标记"未来的 fence 值到达后释放"，不立即释放**
- **`Update`/`Reclaim` 必须每帧在主线程调用**，否则被标记释放的资源永远不会实际回收
- 所有 `Reclaim` 使用相同的 `completedFence`（来自 `FenceManager::GetFence(Type)->GetCompletedValue()`）
- 以下三个回收必须同时调用且顺序无关：
  ```cpp
  GpuResourceManager::GetInstance().Update(completedFence);
  DepthStencilPool::GetInstance().Reclaim(completedFence);
  RenderTargetPool::GetInstance().Reclaim(completedFence);
  ```

### 对等检查
```
// 分配
auto handle = rtPool.Allocate(desc);
// 使用
...
// 释放
rtPool.Release(handle, nextSeq);
// 回收（每帧，在 FrameDriver::Tick 或 GameWorld::Update 中）
rtPool.Reclaim(completedFence);
```

---

## 3. 描述符堆（DescriptorHeapCollection）

### 生命周期
```
Allocate(PartitionType::Texture, index)       // 申请槽位
  → GetPartitionCpuHandle(...)                // 获取 CPU handle
  → device->CreateShaderResourceView(...)     // 写入描述符
  → Free(PartitionType::Texture, slot, seq)   // 释放槽位（延迟到 fence seq 完成）
```

### 约束
- `Allocate` 和 `Free` 的 `PartitionType` 必须一致
- `Free` 的 `fenceValue` 使用 `GetNextSequence()`，确保 GPU 完成使用后才回收
- 描述符堆的 `Free` 是延迟释放，需要在 `DescriptorHeapCollection` 内部由回收机制触发（或依赖 `Reclaim` 模式）
- 编辑器多堆模式下，`Allocate`/`Free` 必须显式传入 `HeapTag`

### 对等检查
```
uint32_t slot = heaps->Allocate(PartitionType::Texture);
if (slot != UINT32_MAX) {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heaps->GetPartitionCpuHandle(PartitionType::Texture, slot);
    device->CreateShaderResourceView(resource, &srvDesc, cpu);
    // ... 使用 ...
    heaps->Free(PartitionType::Texture, slot, nextSeq);
}
```

---

## 4. GPU Resource Barrier 对称性

### 约束
每个 System 在自己录制的命令列表中，必须遵循 **初始状态→所需状态→渲染→初始状态** 的对称屏障规则：

```
入口屏障：COMMON → RENDER_TARGET（或 PIXEL_SHADER_RESOURCE 等）
  → 渲染操作
出口屏障：RENDER_TARGET → COMMON（或对应回退状态）
```

### 对等检查
```
// 入口
auto barrierIn = CD3DX12_RESOURCE_BARRIER::Transition(res, COMMON, RENDER_TARGET);
cmd->ResourceBarrier(1, &barrierIn);

// 渲染
...

// 出口（必须与入口完全对称）
auto barrierOut = CD3DX12_RESOURCE_BARRIER::Transition(res, RENDER_TARGET, COMMON);
cmd->ResourceBarrier(1, &barrierOut);
```

---

## 5. 典型泄露模式

| 模式 | 症状 | 检查方法 |
|------|------|---------|
| 分配器未 Release | 运行数秒后 `AcquireAllocator` 返回无效 handle | 在 `AcquireAllocator` 中加计数断言 |
| 缺少 `Reclaim`/`Update` | 资源池描述符/内存持续增长，最终崩溃 | 在 `Reclaim` 中加 `pool.size()` 断言 |
| early-return 在分配器获取之后 | 分配器泄漏 | 所有系统统一模式：先检查条件，再获取分配器 |
| `OnResize` 未检查尺寸变化 | 反复创建/销毁 RT，池化资源残留 | 在 `OnResize` 入口加 `if (width == m_width && height == m_height) return;` |
| 屏障不对称 | GPU 验证层报错或渲染异常 | 检查每个 ResourceBarrier 的成对出现 |

---

## 6. FrameDriver 中必须包含的回收代码

```cpp
uint64_t completedFence = m_gameContext->GetCompletedFence();
if (completedFence > 0) {
    Resource::GpuResourceManager::GetInstance().Update(completedFence);
    Resource::DepthStencilPool::GetInstance().Reclaim(completedFence);
    Resource::RenderTargetPool::GetInstance().Reclaim(completedFence);

    if (m_gameContext->TextureMgr)
        m_gameContext->TextureMgr->Reclaim(completedFence);
    if (m_gameContext->GeometryResourceManager)
        m_gameContext->GeometryResourceManager->Reclaim(completedFence);
    if (m_gameContext->SkeletonMgr)
        m_gameContext->SkeletonMgr->Reclaim(completedFence);
}
```

此代码必须在所有渲染 System 执行之后、每帧至少执行一次。Game 端在 `GameWorld::Update()` 中调用，Editor 端依赖 `FrameDriver::Tick()` 中的统一回收路径。