# 待办清单

> 根目录工作跟踪，详细文档见 `Docs/todos/`

---

## 编辑器多堆策略（进行中）

### 设计决策

**隐式多堆 + 运行时模式标志**：堆策略由 `HeapMode` 在初始化时决定，而非编译时。

```
                    Game 模式           Editor 模式
    Debug           多堆 ✅             多堆 ✅
    Release         单堆 ✅             多堆 ✅ (必须!)
```

- 调用方通过 `HeapTag` 枚举标识自己所属的堆域，不关心底层策略
- `HeapMode::Single`：所有 `HeapTag` 映射到同一个全局物理堆（Release Game）
- `HeapMode::Multi`：每个 `HeapTag` 对应独立物理堆（Editor / Debug Game）

### 实现步骤

| # | 步骤 | 说明 | 涉及文件 |
|:-:|:-----|:------|:---------|
| 1 | 定义 `HeapTag` 枚举 + `HeapMode` 枚举 | 标识各个堆域和运行模式 | `DescriptorHeapCollection.h` |
| 2 | 改造 `DescriptorHeapCollection` | 内部支持 tag→多堆映射 + Mode 开关 | `DescriptorHeapCollection.h/.cpp` |
| 3 | 迁移调用方 | 所有 `Allocate(PartitionType)` → `Allocate(HeapTag, PartitionType)` | 全局搜索 `Allocate` 调用点 |
| 4 | 编辑器 Viewport Pass | `HeapTag::EditorViewport` 独立堆，离屏 RT + 渲染 | 新建 |
| 5 | 验证 | 编辑器模式多堆分配 + 渲染正确性 | — |

### 相关文档

- `Docs/architecture/Editor.md` 第2节 — 多堆策略完整方案
- `Engine/Resource/Core/DescriptorHeapCollection.h` — 当前实现

---

## 其他待办（摘要）

详见 `Docs/todos/remaining_issues.md`。

| 优先级 | 任务 |
|:------:|:-----|
| P1 | DxMeshLoader 完整实现（骨骼数据路径） |
| P1 | 去掉 Shaders POST_BUILD 复制 |
| P2 | 公告牌手动放置（JSON 驱动） |
| P2 | AssetManager 注册表模式 |
| P3 | 蒙皮角色 JSON 加载 |

---

## 事件系统文档差异（待确认）

Blog 事件系统文档（`blog/src/posts/DX12Engine/EventSystem/`）中描述的若干功能与实际代码存在差异，需在架构确定后统一修正：

| 功能 | Blog 描述 | 实际状态 | 决策 |
|:-----|:----------|:---------|:-----|
| 协程挂起-唤醒 | `Schedule layer.md` 中描述 `ctx.suspend()` | 不存在，FrameDriver 无此机制 | ❓ 未来是否实现？ |
| 双缓冲 Transform | `EventSystem.md` 中 `RenderTransform` + `std::swap(front, back)` | `TransformComponent` 为简单结构体，无双缓冲 | ❓ 未来是否引入？ |
| 静态分片 | 按 Entity ID 哈希分片，零锁竞争 | 不存在 | ❓ 未来是否引入？ |
| 渲染数据分级 | 🔴一级/🟡二级/🟢三级 分级策略 | 不存在 | ❓ 未来是否引入？ |
| ECS 组件名称 | `Position`, `Velocity`, `RenderMesh`, `Health` | `TransformComponent`, `MeshComponent` 等 | ❓ 是否改名？ |
| MessageDispatcher | 5 篇文章均未提及 | 事件系统对外唯一接口 | ✅ 需补充文档 |
| API 名称 | `MessageBus.Post()` | `MessageDispatcher::PostEvent()` | ❓ 是否保留 Blog 的抽象名称？ |

> **建议**：等功能架构确定后再统一修正，当前 Blog 文章可视为"设计蓝图"。

---

## 资源系统文档差异（待确认）

Blog 资源系统文档（`blog/src/posts/DX12Engine/ResourceSystem/ResourceManager.md`）描述的统一资源管理器与实际代码不符：

| 功能 | Blog 描述 | 实际状态 | 决策 |
|:-----|:----------|:---------|:-----|
| 资源管理器架构 | 单一 `ResourceManager` + `HandlePool` + `DataPool` | 多个独立管理器：`TextureManager`、`GeometryResourceManager`、`MaterialManager`、`SkeletonManager` | ❓ 未来是否统一？ |
| 句柄格式 | 18-bit Index + 10-bit Generation + 4-bit AllocatorID | 各管理器使用独立句柄类型（`TextureHandle`、`GeometryHandle`、`MaterialHandle`） | ❓ 是否统一句柄格式？ |
| 数据池 | `DataPool` 大块内存页 + 碎片整理 | 无统一 DataPool，各管理器管理自己的数据 | ❓ 未来是否引入？ |
| 与 GpuResourceManager 关系 | 未提及 | 实际是协作模式，资源管理器释放槽位，GpuResourceManager 统一释放 GPU 资源 | ❓ 是否补充？ |

> **建议**：当前各独立管理器工作正常，统一方案需等架构决策。

---

## 渲染器文档差异（待确认）

Blog 渲染器文档（`blog/src/posts/DX12Engine/DX12Core/Renderer/Renderer.md`）列举的渲染器与实际代码不符：

| 渲染器 | Blog 列出 | 实际存在 | 说明 |
|:-------|:---------:|:--------:|:-----|
| OpaqueRenderer | ✅ | ✅ | 匹配 |
| ShadowRenderer | ✅ | ✅ | 匹配 |
| SkyboxRenderer | ✅ | ✅ `SkyRenderer` | 名称不同 |
| TransparentRenderer | ✅ | ❌ | 不存在 |
| UIRenderer | ✅ | ❌ | 不存在 |
| ParticleRenderer | ✅ | ❌ | 不存在 |
| PostProcessRenderer | ✅ | ❌ | 不存在 |
| SkinnedRenderer | ❌ | ✅ | Blog 未列出 |
| LightingRenderer | ❌ | ✅ | Blog 未列出 |
| SsaoRenderer | ❌ | ✅ | Blog 未列出 |
| TerrainRenderer | ❌ | ✅ | Blog 未列出 |
| WaterRenderer | ❌ | ✅ | Blog 未列出 |
| BillboardRenderer | ❌ | ✅ | Blog 未列出 |
| ReflectionProbeRenderer | ❌ | ✅ | Blog 未列出 |
| GridRenderer | ❌ | ✅ | Blog 未列出 |

> **建议**：Blog 的渲染器列表可能是早期设计，实际渲染器已大幅扩展。

---

## 插值系统文档差异（待确认）

Blog 插值系统文档（`blog/src/posts/DX12Engine/Interpolation.md`）描述的 `SynchronizationLayer` + 插值系统在当前代码中不存在：

| 功能 | Blog 描述 | 实际状态 | 决策 |
|:-----|:----------|:---------|:-----|
| SynchronizationLayer | 独立同步层，管理状态快照和 Fence 检查 | 不存在 | ❓ 未来是否实现？ |
| InterpolationSystem | 双缓冲插值，TimeLerp 计算 | 不存在 | ❓ 未来是否实现？ |
| 状态快照 | `GetStateSnapshot(sequenceId)` 接口 | 不存在 | ❓ 未来是否引入？ |

> **建议**：当前渲染使用 ECS 组件的直接数据，插值系统需等渲染管线稳定后再设计。


