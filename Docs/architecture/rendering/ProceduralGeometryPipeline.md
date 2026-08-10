# 程序化几何体异步管线

> 日期：2026-08-05（更新）
> 状态：📋 **当前方案（GeometryProceduralTask 直接调用）已放弃，转向 §6 AssetManager 虚拟资产（引用模式）**
> 关联：`Docs/architecture/rendering/RenderPipelineSpecification.md`（渲染管线规范）、
> `Docs/architecture/core/AsyncPipelineResponsibilities.md`（异步管线职责）、
> `.atomcode.md` 第 25 条

---

## 1. 管线总览

程序化几何体（程序化水面、天空盒、程序化装饰等）走标准异步加载管线，与网格文件加载一致：

```
SceneConstructor 触发
  │
  ▼
BackgroundExecutor::SubmitLoadTask(task)
  │
  ├─ cpuWork（后台线程）: GeometryGenerator 生成 CPU MeshData
  ├─ gpuWork（后台线程）: 创建 DEFAULT VB/IB + UPLOAD 中转 → COPY → DIRECT barrier
  └─ onComplete（主线程）: 注册到 GeometryResourceManager → 继续组件组装
  │
  ▼
ECS 实体（MeshComponent + RenderSlotComponent + ...）
```

---

## 2. 参考实现：天空盒（SceneConstructor.cpp:259-300）

```cpp
// 1. 创建任务（三段式：cpuWork → gpuWork → onComplete）
auto procResult = std::make_shared<Async::GeometryProceduralOutput>();
auto task = Async::GeometryProceduralTask::Create(
    type, params, device, cmdMgr, geoMgr, procResult);

// 2. 链式扩展 onComplete（拿到 GeometryHandle 后继续）
auto prevOnComplete = task.onComplete;
task.onComplete = [this, prevOnComplete, procResult](bool success) {
    if (prevOnComplete) prevOnComplete(success);
    if (success && procResult->success) {
        GeometryHandle geoHandle = procResult->geometryHandle;
        // → 继续组装：SkyboxManager::SetSkybox(...)
    }
};

// 3. 提交到后台执行器
m_context->BackgroundExecutor->SubmitLoadTask(std::move(task));
```

---

## 3. 水实体异步构建流程

水实体（`waterBlocks[]` → 标准 ECS 实体）的异步构建应完全复用天空盒模式：

```
EditorSceneManager::OnSceneConstructReady
  │
  ├─ 遍历 waterBlocks[]
  │
  ├─ 创建空实体 + TransformComponent（同步）
  │
  ├─ 提交 GeometryProceduralTask（异步）
  │     └─ onComplete:
  │           ├─ GeometryHandle → LODSystem::RegisterLODMesh
  │           ├─ MeshComponent{lodMeshHandle, localBounds}
  │           ├─ GeoMgr->GetSubMeshInfo(geoHandle) → SubMeshInfo
  │           ├─ RenderSlotComponent{
  │           │     slots[0] = {material="Water", subMeshRanges, shaderType=Water}
  │           │   }
  │           ├─ WaterComponent（波浪参数）
  │           └─ RenderSlotCache::MarkDirty()
  │
  └─ 所有 task 提交后 → 下帧 Rebuild 桶 → "water" 桶有数据
```

### 3.1 关键约束

- **组件组装必须在 onComplete（主线程）中完成**，不可在 cpuWork/gpuWork 中访问 ECS Registry
- **GeometryHandle 可用后才能调用 GetSubMeshInfo**（onComplete 中 procResult->success 为 true 时）
- **RenderSlotCache::MarkDirty()** 必须在组件添加后调用，确保下帧 Rebuild 将水实体纳入桶
- **WaterManager::CollectFromECS** 不依赖 MeshComponent，只 view WaterComponent，所以 WaterComponent 可以先于 MeshComponent 添加

---

## 4. GeometryProceduralTask 接口

```cpp
// 文件：Engine/Background/GeometryProceduralTask.h
// 命名空间：DX12Engine::Async

struct GeometryProceduralOutput {
    Resource::GeometryHandle geometryHandle;
    bool success = false;
};

class GeometryProceduralTask {
    static LoadTask Create(
        const std::string &type,                    // "sphere" | "grid" | "cube"
        const Resource::ProceduralGeometryDesc &params, // 几何参数
        ID3D12Device *device,
        Renderer::CommandManager *cmdMgr,
        Resource::GeometryResourceManager *geoMgr,
        std::shared_ptr<GeometryProceduralOutput> outResult);
};
```

### 4.1 输出内容

| 输出 | 说明 |
|:--|:--|
| `GeometryHandle` | 注册到 GeometryResourceManager 后的句柄 |
| `SubMeshInfo` | 1 条：`{startIndex=0, indexCount, startVertex=0}`（程序化网格仅 1 个子网格） |
| `localBounds` | `BoundingAABB`，grid 为 XZ 平面 `(-w/2, 0, -d/2) ~ (w/2, 0, d/2)` |

---

## 5. 数据流图

```
SceneConstructor / EditorSceneManager
  │  SubmitLoadTask(task)
  ▼
BackgroundExecutor
  │  cpuWork → gpuWork → onComplete
  ▼
GeometryProceduralTask::onComplete
  ├─ geoMgr->RegisterGeometry(*geometryOut) → GeometryHandle
  ├─ geoMgr->GetSubMeshInfo(handle) → SubMeshInfo[]
  ├─ geoMgr->GetBounds(handle) → localBounds
  │
  ├─ 天空盒：SkyboxManager::SetSkybox(tex, geo)
  ├─ 水实体：LODSystem::RegisterLODMesh → MeshComponent + RenderSlotComponent + WaterComponent
  │
  └─ RenderSlotCache::MarkDirty()
        │
        ▼
  下帧 BuilderUpload::Rebuild → Dispatch
        │
        ▼
  "water" 桶有数据 → WaterRenderItemBuilder::ForEachBucket("water")

---

## 6. 未来方向：AssetManager 虚拟资产（引用模式）

### 6.1 当前局限

`GeometryProceduralTask` 不经过 `AssetManager`，程序化网格与磁盘资产（.dxmesh）走两套路径：

| 维度 | 磁盘资产（.dxmesh） | 程序化网格（当前） |
|:--|:--|:--|
| 加载入口 | `AssetManager::LoadBatch` | `GeometryProceduralTask::Create` 直接调用 |
| 引用方式 | `mesh.geometry = "mesh_key"` | `mesh.procedural = { type: "grid", ... }` |
| 缓存/去重 | `AssetManager` 缓存 | 无去重（每水块重复生成） |
| 生命周期 | `AssetManager` 统一管理 | 手动管理 |

### 6.2 大型引擎参考

| 引擎 | 做法 |
|:--|:--|
| **Unreal** | `UStaticMesh` 可从磁盘加载，也可通过 `UStaticMesh::BuildFromString` 等工厂方法从程序化数据构建。`UProceduralMeshComponent` 运行时生成，材质槽/碰撞一体 |
| **Unity** | `new Mesh()` 运行时创建，`AssetDatabase.CreateAsset()` 保存为 `.asset`。`Resources.Load` 和程序化生成统一为 `Mesh` 对象 |
| **Godot** | `ArrayMesh` 程序化生成后通过 `resource_local_to_scene` 或保存为 `.tres`/`.res`。`ResourceLoader.load("uid://...")` 和 `load("res://...")` 统一 |

**核心规律：** 程序化几何体是**虚拟资产**，应与磁盘资产走同一套加载管线，只是数据源从"文件 IO"变为"程序生成"。

### 6.3 改造方案：URI Scheme 引用

```
AssetManager::Load("procedural://grid/840x780/32x32")
  → 识别 URI scheme "procedural://"
  → 分发到 ProceduralGeometryLoader（与 MeshLoadTask 并列）
  → 创建 GeometryProceduralTask（cpuWork + gpuWork + onComplete）
  → 注册到 GeometryResourceManager
  → 返回 GeometryHandle（与 .dxmesh 返回的 GeometryHandle 类型一致）
```

场景 JSON 中统一引用：

```json
{
  "mesh": {
    "geometry": "procedural://grid/840x780/32x32",
    "materials": ["Water"]
  }
}
```

`MeshDesc.geometry` 兼容两种形式：

| 形式 | 示例 | 说明 |
|:--|:--|:--|
| 文件路径 | `"Models/water_plane.dxmesh"` | 磁盘资产，走 `MeshLoadTask` |
| 程序化 URI | `"procedural://grid/840x780/32x32"` | 虚拟资产，走 `ProceduralGeometryLoader` |
| 内联描述 | `{ "type": "grid", "width": 840, ... }` | 当前方案，过渡期保留 |

### 6.4 改造步骤

| 步骤 | 内容 | 优先级 |
|:--:|:--|:--:|
| 1 | `AssetManager` 新增 URI scheme 识别（`"procedural://"` 前缀分发） | P2 |
| 2 | 创建 `ProceduralGeometryLoader`（与 `MeshLoadTask` 并列，注册到 `AssetManager` 注册表） | P2 |
| 3 | `MeshDesc.geometry` 支持 `"procedural://"` URI 字符串（`from_json` 中不做特殊处理——`AssetManager` 内部识别 scheme） | P2 |
| 4 | 缓存去重：相同 URI 的网格只生成一次，`AssetManager` 引用计数管理 | P2 |
| 5 | 内联 `mesh.procedural` 过渡方案保留，逐步迁移到 URI 引用 | P3 |