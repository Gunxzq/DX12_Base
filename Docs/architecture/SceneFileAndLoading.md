# 场景文件与资产加载管线

## 概述

将硬编码的场景创建（`GameWorld::CreateTerrain()` 等）替换为统一的场景文件 + 通用加载管线。场景文件定义"需要哪些资产"和"如何构建 ECS"，**场景编辑器**在此基础上得以存在。

材质数据**内联于场景 JSON**，不依赖独立的 `.mat` 文件。`.mat` 作为编辑器导出/共享的工具格式保留，但不参与运行时默认加载路径。

---

## 1. 场景文件格式

JSON 格式，扁平化存储，实体间的关系通过 ID 引用表达（`relationships` 数组），不嵌套、不递归。

> **设计决策**：扁平 JSON + ID 引用 vs 嵌套树。
> 选择扁平的理由：（1）解析线性 O(n)，可并行；（2）增量加载只需 append；（3）ECS 天然扁平，`RelationshipComponent` 直接映射 JSON 字段；
> （4）关系只是数据，可以随意切断，不破坏文件结构。
> 详见 `Docs/architecture/RelationshipModel.md`。

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
        "textures": {
            "stone_diffuse": "Textures/Stone_D.dds",
            "stone_normal": "Textures/Stone_N.dds",
            "stone_mr": "Textures/Stone_MR.dds",
            "stone_ao": "Textures/Stone_AO.dds"
        }
    },
    "materials": {
        "stone": {
            "shader": "OpaquePBR",
            "params": {
                "baseColor": [0.8, 0.6, 0.4, 1.0],
                "metallic": 0.2,
                "roughness": 0.8,
                "ao": 1.0,
                "alphaCutoff": 0.5
            },
            "textures": {
                "baseColor": "stone_diffuse",
                "normal": "stone_normal",
                "metallicRoughness": "stone_mr",
                "ao": "stone_ao",
                "emissive": ""
            }
        },
        "grass": {
            "shader": "OpaquePBR",
            "params": {
                "baseColor": [0.2, 0.7, 0.1, 1.0],
                "metallic": 0.0,
                "roughness": 0.9,
                "ao": 1.0
            },
            "textures": {
                "baseColor": "grass_diffuse",
                "normal": "grass_normal"
            }
        }
    },
    "entities": [
        {
            "name": "Statue",
            "persistentId": 1001,
            "components": {
                "transform": { "position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1] },
                "mesh": { "geometry": "statue", "material": "stone" }
            }
        },
        {
            "name": "MainCamera",
            "persistentId": 1002,
            "components": {
                "transform": { "position": [0, 5, -10] },
                "camera": { "fov": 60, "near": 0.1, "far": 1000, "projection": "perspective", "isMain": true }
            },
            "relationships": [
                { "kind": "parent", "targetId": 1001 }
            ]
        }
    ],
    "hash": "a1b2c3d4e5f67890"
}
```

顶层可选字段说明：
| 字段 | 类型 | 说明 |
|:-----|:------|:------|
| `version` | uint32 | 场景格式版本 |
| `metadata` | object | 场景元数据（名称、描述） |
| `baseURL` | string | 依赖路径前缀 |
| `sceneEnvironment` | object | **管理器全局数据**（环境光、天空盒等，不进入 ECS Registry）。区别于 `entities`（ECS 实体数据），见下方 §1.1.1 |
| `dependencies` | object | 外部资产依赖 |
| `materials` | object | 内联材质定义 |
| `entities` | array | **ECS 实体列表**（有 TransformComponent 的场景物体） |
| `hash` | string | 场景内容 FNV-1a 64-bit 十六进制 hash（可选，由工具链填充） |

#### 1.1.1 sceneEnvironment 字段结构

`sceneEnvironment` 统一存放管理器专有、无实体身份的全局场景参数，**不进入 ECS Registry**。

| 子字段 | 类型 | 对应 Manager | 说明 |
|:-------|:------|:-------------|:------|
| `ambient` | object | `LightManager` → `SetAmbientLight` | 环境光全局参数（当前仅 `ambientLight` 数组） |
| `skybox` | object | `SkyboxManager` → `SetSkybox` | 天空盒配置（纹理、几何体、兜底颜色） |

`skybox.geometry` 支持两种形式：

**程序化生成（推荐）**——`geometry` 为 object，指定形状类型和参数：
```json
"skybox": {
    "texture": "sky_cubemap",
    "geometry": {
        "type": "cube"
    },
    "color": [0.5, 0.6, 1.0, 1.0]
}
```

球体带细分参数：
```json
"geometry": {
    "type": "sphere",
    "radius": 1.0,
    "rings": 16,
    "segments": 16
}
```

**外部文件引用**——`geometry` 为 string，引用 `dependencies.meshes` 中的 key（向后兼容）：
```json
"geometry": "skybox_mesh",
"dependencies": {
    "meshes": {
        "skybox_mesh": "Models/cube.dxmesh"
    }
}
```

程序化生成时由 `SceneConstructor` 在 `OnDependenciesLoaded` 阶段调用 `GeometryGenerator` 生成立方体或球体的 VB/IB，创建 DEFAULT 堆 GPU 缓冲并注册到 `GeometryResourceManager`，不依赖外部 mesh 文件。详见 `GeometryGenerator` 接口文档。

> **设计原则**：`sceneEnvironment` 中的数据无 TransformComponent、不被剔除、不被拾取、不被属性卡编辑。它们由 Manager 直接在初始化阶段设置。`entities` 中的 ECS 实体数据则参与完整的剔除/拾取/编辑/序列化流程。详见 `Docs/architecture/EngineOverview.md §9.6`。

### 1.2 依赖收集

`dependencies` 节声明该场景依赖的所有外部资产。加载器遍历此节，收集所有唯一路径，提交批量加载请求。

与旧版的关键区别：

| 旧版 | 新版 |
|------|------|
| `dependencies.materials` 引用 `.mat` 文件路径 | ❌ 不再存在 |
| `materials` 节不存在 | ✅ `materials` 节内联定义材质数据 |
| 材质中的纹理路径是文件路径 | ✅ 纹理路径引用 `dependencies.textures` 中的 key |
| 需单独读取 `.mat` 文件 | 一次 JSON 解析即可获取全部材质数据 |

依赖引用的好处：**依赖关系显式声明**，加载器无需遍历 entity 树就能知道需要加载哪些资产。材质中引用的是 `dependencies.textures` 的 key，而非文件路径，避免了"纹理→路径→再解析"的步骤。

### 1.3 材质定义规范

每个材质项包含：

| 字段 | 类型 | 说明 |
|------|------|------|
| `shader` | string | 渲染器类型哈希，如 `"OpaquePBR"` |
| `params.baseColor` | float[4] | 基础色 RGBA |
| `params.metallic` | float | 金属度 |
| `params.roughness` | float | 粗糙度 |
| `params.ao` | float | 环境光遮蔽强度 |
| `params.alphaCutoff` | float | 透明度裁切阈值（可选） |
| `textures.baseColor` | string | 引用 `dependencies.textures` 中的 key |
| `textures.normal` | string | 同上 |
| `textures.metallicRoughness` | string | 同上 |
| `textures.ao` | string | 同上 |
| `textures.emissive` | string | 同上（可为空） |

材质中的纹理引用是 **key**（非文件路径），解析步骤在 `OnDependenciesLoaded` 中通过 `textureKey → SRV 索引` 映射完成。

### 1.4 外部材质引用（编辑器扩展）

编辑器如需复用跨场景的材质，可使用 `$ref` 语法糖：

```json
"materials": {
    "stone": { "$ref": "Materials/SharedStone.mat" }
}
```

`SceneLoader` 在解析阶段 resolve 为内联数据。运行时默认管线不依赖此路径。

### 1.5 实体字段

每个实体包含以下字段：

| 字段 | 类型 | 必需 | 说明 |
|:-----|:------|:------|:------|
| `name` | string | ✅ | 实体名称（运行时同步到 `NameComponent`） |
| `persistentId` | string | ✅ | 持久化 ID（fnv1a 64-bit 十六进制 hash 字符串），永不重复。作为场景 JSON 中跨实体引用的唯一标识 |
| `components` | object | ✅ | ECS 组件数据（见 §1.6 组件映射） |
| `relationships` | array | ❌ | 关系列表，每个元素 `{ kind, targetId, socketName? }`，`targetId` 为目标实体的 persistentId |
| `children` | array | ❌ | **已废弃** — 扁平 JSON 不支持嵌套 children。旧文件中的 children 会在加载时展平并转换为 `relationships`（kind=parent） |

### 1.6 组件映射

每个 entity 的 `components` 节直接对应 ECS 组件：

> **层级说明**：以下 ECS 组件均为运行时场景实体数据（Scene/Entity Layer），不属于引擎 CORE 基础设施。引擎 CORE 只提供 `World`（ECS 源头）、`SceneManager`（序列化器）、`WindowFrameResources`（窗口尺寸 GPU 资源）等基础设施抽象。组件本身是纯数据结构，无行为、无生命周期。

| JSON 字段 | ECS 组件 | 层 | 处理器 |
|-----------|----------|:---|--------|
| `transform` | `TransformComponent` | Scene Entity Data | SceneConstructSystem 构造时预计算世界矩阵 |
| `mesh` | `MeshComponent` | Scene Entity Data | 从 geoMap / matMap 查找 GPU handle |
| `light` | `LightComponent` | Scene Entity Data | 注册到 LightManager |
| `camera` | `CameraComponent` | Scene Entity Data | 注册到 CameraManager |

### 1.7 实体关系（Relationships）

实体间的关系在 `relationships` 数组中表达，使用目标实体的 `persistentId`（fnv1a 64-bit hash 字符串）引用：

```json
"relationships": [
    { "kind": "parent", "targetId": "a1b2c3d4e5f67890" },
    { "kind": "socket", "targetId": "a1b2c3d4e5f67890", "socketName": "hand_r" }
]
```

**导入流程**（SceneConstructor）：

```
第 1 遍：遍历所有实体 → registry->create() → 建 hash→entt::entity 映射
第 2 遍：遍历 relationships → 用映射表将 targetId 解析为 entt::entity handle
运行时：RelationshipComponent.targetEntity 直接存储 entt::entity（O(1) 访问）
```

**导出流程**（Editor 端 ExportToDescription）：

```
从 SceneSnapshot::entityDescs 缓存读取 persistentId
→ 直接写入 JSON 的 targetId 字段
→ 不需要从 ECS 反查
```

**设计要点**：

- **关系是数据，不是树** — 引擎 CORE 只负责存储 `RelationshipComponent { targetId, kind }`，不解释、不维护树结构、不做级联删除
- **关系可切断** — Gameplay 脚本随时可以删除 `RelationshipComponent`，切断后实体自然独立
- **骨架层级不是场景关系** — 基于骨骼的角色是单一 ECS 实体，骨骼树属于动画系统，不在关系模型中
- **引擎 CORE 不自动传播 Transform** — 相机跟随、武器挂点同步等行为由 Gameplay 层脚本实现
- **Editor 端的联动**（拖动父带动子）由 Editor 端自行实现，不在 CORE 中

详见 `Docs/architecture/RelationshipModel.md`。

---

## 2. 加载管线

### 2.1 整体流程

```
场景 JSON → SceneLoader::LoadFromFile
    ↓
SceneDescription（内存：依赖 + 内联材质 + 实体列表）
    ↓
SceneConstructor::LoadScene
    ├─ 收集 deps → AssetManager::LoadBatch(meshes + textures)
    │     ↓
    │   [BackgroundExecutor 后台线程池]
    │     ├─ Mesh A: cpuWork(IO+解析) → gpuWork(创建DEFAULT+录制COPY)
    │     ├─ Mesh B: cpuWork(IO+解析) → gpuWork(创建DEFAULT+录制COPY)
    │     ├─ Texture C: cpuWork(IO+解析+创建纹理) → (无gpuWork)
    │     └─ Texture D: cpuWork(IO+解析+创建纹理) → (无gpuWork)
    │     ↓
    │   [主线程 Tick] ProcessGpuWork → Submit COPY → Signal → ...
    │     ↓
    │   onComplete: 注册句柄 → m_cache
    │     ↓
    └─ OnDependenciesLoaded
          ├─ Step 1: textureKey → SRV 索引映射
          ├─ Step 2: meshKey → GeometryHandle 映射
          ├─ Step 3: 遍历内联材质 → 填 SRV → RegisterMaterial
          ├─ Step 4: SceneConstructData + 材质 buffer COPY 上传
          └─ PostEvent(GeneratorTaskCompleteEvent, payload=genType<<32|sceneId)
                ↓
         SceneConstructSystem
           └─ 遍历 entities → ECS Entity + 组件
```

### 2.2 各角色职责

参见 `AsyncPipelineResponsibilities.md`，简要概括如下：

| 角色 | 职责 |
|------|------|
| **SceneLoader** | JSON → SceneDescription（一次解析，无二次 IO） |
| **AssetManager** | 文件→GPU 句柄（Mesh/Texture），不关心组合 |
| **SceneConstructor** | 编排依赖、SRV 映射、材质注册、buffer 上传、发事件 |
| **BackgroundExecutor** | 后台执行 LoadTask（cpuWork + gpuWork），Tick 驱动提交 |
| **SceneConstructSystem** | 响应事件 → SharedDataStore → 创建 ECS Entity |

### 2.3 材质在管线中的位置

```
[Scene JSON 解析阶段]                            SceneLoader
  ├─ materials 内联 → 解析为 MaterialData 列表
  └─ 纹理引用为 key → 留待后续 resolve

[依赖加载阶段]                                    AssetManager
  ├─ Mesh 并行加载（文件→GPU DEFAULT 堆）
  └─ Texture 并行加载（文件→GPU 纹理→SRV）

[编排阶段]                                       SceneConstructor
  ├─ 纹理 key → 实际 SRV 索引（查 textureManager）
  ├─ 材质数据 ← 填入 SRV 索引
  ├─ MaterialManager::RegisterMaterial（此时纹理已就绪）
  └─ 材质 GPU buffer COPY 上传
```

### 2.4 纹理→材质依赖

由于 shader 使用无界描述符数组（bindless），材质存储的是纹理的 SRV heap 索引，而非 GPU 资源句柄。因此：

1. 纹理必须先加载并完成 SRV 分配
2. 材质注册时才能拿到正确的 SRV 索引
3. 纹理的加载（后台 IO + GPU 创建）和材质注册（主线程）天然串行

这与大型引擎的做法一致：
- **glTF**：材质引用 `textures[]` 数组索引，加载器按序加载
- **Unreal**：纹理在 Streaming Manager 中挂载后，材质参数才有效
- **Unity**：`Material.SetTexture` 在纹理加载完成后调用

### 2.5 等待语义

场景加载有两种模式：

| 模式 | 触发方式 | 等待策略 |
|------|---------|---------|
| **批量加载**（编辑器打开场景） | 显式调用 LoadScene | 所有 LoadTask 完成后回调 OnDependenciesLoaded，再等待材质 buffer 上传完成后发事件 |
| **流式加载**（运行时异步流送） | 相机接近 → 触发加载 | 走同一套流程，但不阻塞主线程 |

当前先实现批量加载。流式加载为后续扩展预留接口。

### 2.6 SceneConstructSystem

```
SceneConstructSystem (响应 GeneratorTaskCompleteEvent, 检查 payload 高位生成器类型)
  ├─ 从 event payload 读取 sceneId
  ├─ SharedDataStore::GetTypedData("scene_construct_N")
  ├─ 遍历 entities:
  │     ├─ TransformDesc → 预计算世界矩阵
  │     ├─ MeshDesc:
  │     │     ├─ geoMap[geometry_key] → GeometryHandle
  │     │     └─ matMap[material_key] → MaterialHandle
  │     │     └─ ECS::Entity.AddComponent<MeshComponent>
  │     ├─ LightDesc → LightManager::Register
  │     └─ (其他组件同理)
  └─ 构造完成后释放 SharedDataStore 中的场景数据
```

---

## 3. 与现有系统的关系

| 系统 | 角色 | 变更 |
|------|------|------|
| **BackgroundExecutor** | 执行 LoadTask 的 cpuWork/gpuWork | 已有 `SubmitLoadTask`，无需变更 |
| **AssetManager** | 文件 IO → GPU 句柄，缓存管理 | ✅ Mesh 需修复 UPLOAD→DEFAULT |
| **SceneLoader** | 场景 JSON → SceneDescription | ✅ `materials` 节改为内联解析 |
| **SceneConstructor** | 编排 + 材质注册 + buffer 上传 | ✅ 移除 `.mat` 文件读取路径 |
| **SceneConstructSystem** | 构造数据 → ECS 实体 | **待实现** |
| **GeometryResourceManager** | GPU 几何体注册 | 不变 |
| **TextureManager** | GPU 纹理注册 + SRV 管理 | 不变 |
| **MaterialManager** | 材质注册 + GPU buffer 管理 | 不变 |
| **DescriptorHeapCollection** | 描述符分配 | 不变 |
| **SharedDataStore** | 场景构造数据中转 | 不变 |
| **`.mat` 文件** | 编辑器导出/共享格式 | ❌ 不再参与运行时默认加载路径 |

---

## 4. 与 Snapshot System 的关系

Snapshot System 的 L1 `FileSnapshot` 提供文件级变更检测。当场景文件被外部工具（如 JSON 编辑器）保存时：

```
FileSnapshot 检测到变更 → 通知 AssetManager → 重新解析场景 → 差异更新 ECS
```

当前阶段不做热更新，首次加载完成后忽略文件变更。

---

## 5. 路线图

| 阶段 | 内容 | 前置条件 |
|:----:|------|---------|
| **P0** | 场景 JSON 格式定稿（含内联材质） + SceneLoader 更新 | — |
| **P0** | SceneConstructor 移除 `.mat` 读取路径，改用内联材质 | P0 格式定稿 |
| **P1** | SceneConstructSystem 实现（事件驱动 ECS 构造） | SceneConstructor 事件发出 |
| **P1** | Mesh 修复：AssetManager::Load 走 COPY 队列上传 DEFAULT 堆 | — |
| **P2** | 迁移已有 GameWorld 硬编码场景到此管线 | P0 + P1 |
| **P3** | 场景编辑器 + 外部 `.mat` 引用语法糖 | 场景文件稳定 |

---

## 6. 已知设计空缺：场景资源生命周期

### 6.1 问题

`MeshLoadTask` 创建的 DEFAULT VB/IB（`Mesh_VB_Default`、`Mesh_IB_Default`）通过 `TriangleMesh` 注册到 `GeometryResourceManager`。当场景卸载或几何体被释放时，`GeometryResourceManager::FreeEntry` 仅清空槽位，**不释放 GPU 资源**。

其原因是资源管理器与 `GpuResourceManager` 的协作模式约束（见 `.atomcode.md` 规则11）：资源管理器禁止直接调用 `GpuResourceManager::Release`。

### 6.2 当前状态

运行时不出现问题的原因：

| 因素 | 说明 |
|:-----|:------|
| 场景仅加载一次 | 当前编辑器/Game 启动后场景只加载一次，不涉及重载释放 |
| 常量缓冲 vs 几何体 | DEFAULT VB/IB 作为永久资源长期驻留，不属于"泄漏"——它们就是场景本身 |
| RenderDoc 看到的是"使用中" | 这些资源在 RenderDoc 中可见是正常的，因为它们正在被 GPU 使用 |

### 6.3 归属

DEFAULT VB/IB 的 GPU 资源释放应归**场景管理器（SceneManager）** 负责，而非资源管理器自身。场景切换时，场景管理器遍历当前场景的所有几何体句柄，通过 `GpuResourceManager::Release` 释放底层 GPU 资源，再通过 `GeometryResourceManager::Release` 释放槽位。

当前没有场景管理器，场景卸载/重载的逻辑尚未实现。此问题在未来实现场景切换时统一处理。

---

## 7. 未来方向：场景大堆（Scene Heap）

### 7.1 动机

当前每个 mesh 的 VB/IB 是独立 `GpuResourceHandle`，精细但释放零散。大型引擎在发布模式下通常使用**场景大堆**：

```
场景 JSON 解析完成
  ↓
计算所有 mesh 的 VB/IB 总大小
  ↓
CreateHeap + CreatePlacedResource（单一大堆，零碎片）
  ↓
场景使用期间全部从此堆分配
  ↓
场景卸载 → 整堆释放（O(1)，无需逐 mesh 回收）
```

### 7.2 优势

| 维度 | 当前精细化模式 | 场景大堆模式 |
|:-----|:--------------|:-------------|
| 分配 | 逐 mesh 独立 `CreateCommittedResource` | 预分配 `CreateHeap` + `CreatePlacedResource` |
| 碎片 | 可能产生外部碎片 | 零外部碎片（线性分配） |
| 释放 | 逐 mesh 引用计数 + fence 等待 | 整堆 `Release`，GPU 空闲后一次性回收 |
| 差异更新 | ✅ 支持单 mesh 热替换 | ❌ 需整体重建或预留空洞 |
| 实现复杂度 | 低（当前模式） | 中（需场景管理器编排） |

### 7.3 混合方案

编辑器模式保留精细化（差异更新），发布模式使用场景大堆（性能）。堆大小可记录在场景 JSON 中：

```json
{
  "version": 1,
  "heap": {
    "vbSize": 4194304,
    "ibSize": 2097152
  },
  "dependencies": { ... },
  "entities": [ ... ]
}
```

场景加载时据此预分配堆，所有 mesh 的 VB/IB 通过 `CreatePlacedResource` 放置到该堆上。卸载时整堆释放。

### 7.4 当前阶段

当前仍处于编辑器/调试阶段，精细化模式是合理的。场景大堆作为发布版优化，在场景管理器实现后再考虑。

---

## 8. 编辑器序列化（ExportToDescription）已知缺口

编辑器的 `ExportToDescription()` 从 ECS 运行时状态重建 `SceneDescription`，用于 JSON 文件保存。以下字段在当前实现中**尚未导出**或**依赖缓存**：

| 组件/字段 | 状态 | 原因 | 后续支持 |
|:----------|:-----|:------|:---------|
| **SkinnedComponent** | ❌ 未导出 | ECS 中无反向映射（skeleton/animationClip key 未缓存） | 在 `m_entityDescs` 缓存中增加 `skinned` 字段 |
| **CameraComponent** | ✅ 已修复 | ECS `CameraComponent` 已创建，`SceneConstructor::ConstructEntity` 已实现，`ExportToDescription` 已支持直接读取 ECS 组件 | — |
| **BillboardComponent** | ⚠️ 依赖缓存 | 从 `m_entityDescs` 中读取缓存的 key；新创建（未缓存）的 billboard 实体丢失 | 无缓存的实体 fallback 读取 ECS 组件原始参数 |
| **TerrainComponent** | ⚠️ 依赖缓存 | 同 billboard，key 从缓存读取 | 同上 |
| **Material extraParams** | ⚠️ 数据丢失 | `MaterialDesc::to_json` 不写入 `"extra"` 字段，但 `from_json` 可以读取 | `to_json` 补充 `extra` 序列化 |
| **Material hash** | ✅ 已修复 | `SaveToFile` 自动从 JSON 内容计算 FNV-1a 64-bit hash 并写入 `"hash"` 字段 | — |
| **Children 层级** | ✅ 已修复 | `ExportToDescription` 通过 `collectChildren` 递归遍历并匹配子实体名称重建层级 | — |
| **Light color alpha** | ✅ 已修复 | LightDesc::color 改为 4 分量 `{x, y, z, 1.0f}` | — |
| **$schema 字段** | ✅ 已修复 | `SaveToJSON` 写入 `"$schema": "https://dx12engine.dev/schemas/scene.schema.json"` | — |
| **JSON 键顺序** | ✅ 已修复 | `SaveToJSON` 改用 `nlohmann::ordered_json`，按 schema 字段顺序排列 | — |
| **依赖路径** | ✅ 已修复 | 从 `m_originalDependencies` 读取原始相对路径，不再写入空字符串 | — |
| **内联材质** | ✅ 已修复 | 从 `m_originalMaterials` 还原材质定义 | — |
| **baseURL** | ✅ 已修复 | 从 `m_originalBaseURL` 还原 | — |

### 8.1 序列化数据流（当前状态）

```
ECS 运行时实体
  ↓ ExportToDescription()
SceneDescription（含 m_originalDependencies/materials/baseURL）
  ↓ SaveToJSON() → ordered_json
JSON 文件（$schema + 键顺序 + hash）
```

### 8.2 写入字段覆盖（按 schema 定义顺序）

| 字段 | to_json 写入 | 说明 |
|:-----|:-----------|:-----|
| `$schema` | ✅ 写入 | `"https://dx12engine.dev/schemas/scene.schema.json"` |
| `version` | ✅ 始终写入 | |
| `baseURL` | ✅ 非空时写入 | |
| `metadata` | ✅ 非空时写入 | |
| `environment` | ✅ 始终写入 | |
| `skybox` | ✅ 存在时写入 | C++ 字段，schema 可选，当前编辑器支持 |
| `dependencies` | ✅ 非空时写入 | |
| `materials` | ✅ 非空时写入 | |
| `entities` | ✅ 始终写入 | |
| `hash` | ✅ 自动计算写入 | SaveToFile 中计算 FNV-1a 64-bit |
