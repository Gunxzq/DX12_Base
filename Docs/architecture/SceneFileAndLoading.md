# 场景文件与资产加载管线

## 概述

将硬编码的场景创建（`GameWorld::CreateTerrain()` 等）替换为统一的场景文件 + 通用加载管线。场景文件定义"需要哪些资产"和"如何构建 ECS"，**场景编辑器**在此基础上得以存在。

---

## 1. 场景文件格式

JSON 格式，编辑时嵌套存储（编辑器友好），加载时展平为扁平 ECS（运行时高效）。

Schema 定义：`Schemas/scene.schema.json`

### 1.1 顶层结构

```json
{
    "version": 1,
    "metadata": {
        "name": "TestLevel",
        "description": "测试场景"
    },
    "dependencies": {
        "meshes": {
            "statue": "Models/Statue.ddsmesh",
            "ground": "Models/Ground.ddsmesh"
        },
        "materials": {
            "stone": "Materials/Stone.mat",
            "grass": "Materials/Grass.mat"
        },
        "textures": {
            "stone_diffuse": "Textures/Stone_D.dds",
            "stone_normal": "Textures/Stone_N.dds"
        }
    },
    "entities": [
        {
            "name": "Statue",
            "components": {
                "transform": { "position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1] },
                "mesh": { "geometry": "statue", "material": "stone" }
            },
            "children": [...]
        }
    ]
}
```

### 1.2 依赖收集

`dependencies` 节声明该场景依赖的所有外部资产。加载器遍历此节，收集所有唯一路径，提交批量加载请求。`entities` 节中的 `"geometry": "statue"` 引用 `dependencies.meshes.statue` 中声明的路径。

这种间接引用的好处：**依赖关系显式声明**，加载器无需遍历 entity 树就能知道需要加载哪些资产。

### 1.3 组件映射

每个 entity 的 `components` 节直接对应 ECS 组件：

| JSON 字段 | ECS 组件 | 处理器 |
|-----------|----------|--------|
| `transform` | `TransformComponent` | 加载时预计算世界矩阵 |
| `mesh` | `MeshComponent` | 将 geometry/material key 解析为 GPU handle |
| `light` | `LightComponent` | 注册到 LightManager |
| `camera` | `CameraComponent` | 注册到 CameraManager |

加载完成后，场景构造系统遍历 entities，对每个 entity 按组件类型分发到对应的填充器。

---

## 2. 加载管线

### 2.1 整体流程

```
场景 JSON → 解析器
    ↓
收集依赖路径 → AssetManager::LoadAssets(所有路径)
    ↓
[后台线程池 - TaskGraph]
  ├─ 并行 Load Mesh A      (IO → 解压 → 解析 → 注册 GPU)
  ├─ 并行 Load Mesh B      (IO → 解压 → 解析 → 注册 GPU)
  ├─ 并行 Load Texture C   (IO → DDS解析 → 注册 GPU)
  └─ 通知: 所有 Task 完成
    ↓
[主线程]
场景构造系统遍历 entity 列表
    ├─ 解析 transform → 计算世界矩阵
    ├─ 查找 mesh 依赖 → 填充 geometryHandle / materialHandle
    ├─ 创建 ECS Entity
    └─ 添加组件
```

### 2.2 AssetManager 职责（见 ResourceManager.md）

```
AssetManager
  ├─ 接收路径列表 → 去重 → 拆分为 TaskGraph
  ├─ 提交到 BackgroundExecutor::SubmitGraph()
  ├─ 跟踪每个 Task 的完成状态
  └─ 全部完成后回调 → 场景构造系统
```

AssetManager **不关心**加载后的数据怎么用。它的产出是 `vector<CpuResourceHandle>`，调用方自行消费。

### 2.3 TaskGraph 的任务拆分

参考 UE5 的加载依赖链，一个资产的加载不是单步 Task，而是 TaskGraph：

```
Load Mesh A:
  IO Task: 读取 .ddsmesh → 内存
    ↓
  Decompress Task: 解压顶点/索引数据
    ↓
  GPURegister Task: 创建 VB/IB → 注册到 GeometryResourceManager → 获取 handle
```

纹理同理：

```
Load Texture C:
  IO Task: 读取 .dds → 内存
    ↓
  Parse Task: 解析 DDS 头 + 像素数据
    ↓
  GPURegister Task: 创建纹理资源 → 注册到 TextureManager → 获取 handle
```

这些 Task 由 `BackgroundExecutor::SubmitGraph` 调度到工作线程池，IO 和 CPU 密集型任务天然并行。

### 2.4 等待语义

场景加载有两种模式：

| 模式 | 触发方式 | 等待策略 |
|------|---------|---------|
| **批量加载**（编辑器打开场景） | 显式调用 LoadScene | TaskGraph 全部完成后回调 |
| **流式加载**（运行时异步流送） | 相机接近 → 触发加载 | LoadScene 同样走 TaskGraph，但不阻塞主线程 |

当前先实现批量加载。流式加载为后续扩展预留接口。

### 2.5 场景构造系统

```
SceneConstructor
  ├─ 接收: 场景 JSON + vector<CpuResourceHandle>
  ├─ 步骤:
  │    1. 建立 key → GPU handle 的映射表
  │       {"statue_geo": geoHandle, "stone_mat": matHandle, ...}
  │    2. 遍历 entities:
  │       - 预计算世界矩阵（递归遍历，展平存储）
  │       - 从映射表中查找 geometry/material handle
  │       - 创建 Entity + 添加 Transform/Mesh 组件
  └─ 产出: 场景中所有 Entity 已就绪，构建筑自动拾取
```

---

## 3. 与现有系统的关系

| 系统 | 角色 | 变更 |
|------|------|------|
| **BackgroundExecutor** | 执行 TaskGraph 中的加载任务 | 已有 `SubmitGraph`，无需变更 |
| **AssetLoader** | 底层文件读取 | 不变 |
| **各 XxxLoader**（TerrainLoader 等） | 解析具体文件格式 | 按资产类型实现，AssetManager 统一调度 |
| **GeometryResourceManager** | GPU 几何体注册 | 不变 |
| **TextureManager** | GPU 纹理注册 | 不变 |
| **MaterialManager** | 材质注册 | 不变 |
| **AssetDataManager** | CPU 端数据中转 | 当前 `TerrainLoadData` 等临时结构迁移到此 |
| **场景文件** | 新增格式 | 新增 `Config/Scenes/*.scene.json` |
| **场景编辑器** | 未来模块 | 依赖场景文件格式定义 |

---

## 4. 与 Snapshot System 的关系

Snapshot System 的 L1 `FileSnapshot` 提供文件级变更检测。当场景文件被外部工具（如 JSON 编辑器）保存时：

```
FileSnapshot 检测到变更 → 通知 AssetManager → 重新解析场景 → 差异更新 ECS
```

当前不做热更新，首次加载完成后忽略文件变更。

---

## 5. 路线图

| 阶段 | 内容 | 前置条件 |
|:----:|------|---------|
| **P0** | 定义 `.scene.json` 格式 + 解析器 | — |
| **P0** | AssetManager 统一加载接口（按路径列表加载） | BackgroundExecutor::SubmitGraph |
| **P1** | 场景构造系统（JSON → ECS） | AssetManager |
| **P2** | 迁移已有 GameWorld 硬编码场景到此管线 | P0 + P1 |
| **P3** | 场景编辑器（依赖场景文件格式） | 场景文件稳定 |
