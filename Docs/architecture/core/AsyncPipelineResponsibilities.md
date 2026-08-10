# 异步加载管线职责边界

## 概述

异步加载管线由四个角色组成，按数据流向排列：

```
           Scene JSON
               ↓
[SceneLoader] 解析
               ↓
     SceneDescription (内存数据)
               ↓
[SceneConstructor] 编排
     ├─ AssetManager::LoadBatch 并行加载 Mesh + Texture
     └─ Mesh + Texture 全部就绪 → 材质注册 + GPU buffer 上传
               ↓
     GeneratorTaskCompleteEvent（payload 高位 = GENERATOR_TYPE_SCENE_CONSTRUCTOR）
               ↓
[SceneConstructSystem] 构造 ECS
               ↓
       场景内的 ECS Entity
```

## 角色职责

### SceneLoader — 场景文件解析器

**职责**：JSON 文件/内存 → `SceneDescription`（纯内存结构）

- 解析场景 JSON 为 `SceneDescription`，包括：
  - `dependencies`: 外部资产依赖（mesh 路径、texture 路径）
  - `materials`: 材质定义（内联，含纹理 key 引用和参数数值）
  - `entities`: 实体列表（组件描述 + 层级结构）
- 材质数据**直接从 JSON 内联定义解析**，不独立读取 `.mat` 文件
- 纹理引用指向 `dependencies.textures` 中的 key（间接引用），而非文件路径
- **纯 CPU 同步操作**，在任意线程可调用
- **不参与**异步加载、GPU 上传、ECS 构造

### AssetManager — 资源加载器

**职责**：文件 IO → GPU 资源句柄，**不关心**数据如何组合

- 接收路径 + 类型 + 回调，输出 `AssetResult`（GPU 句柄）
- 内部使用 `LoadTask` 三段式模型（cpuWork → gpuWork → onComplete），提交到 `BackgroundExecutor`
- 缓存已加载资源（路径 → `AssetResult`），同路径重复请求返回缓存结果
- **Mesh 的 VB/IB 必须走 COPY 队列上传到 DEFAULT 堆**，UPLOAD 堆只做中转

| 资源类型 | cpuWork（后台） | gpuWork（后台） | onComplete（主线程） |
|---------|----------------|----------------|---------------------|
| Mesh     | 读 `.ddsmesh`，解析顶点/索引 | 创建 DEFAULT VB/IB + UPLOAD → Map/memcpy → 录制 COPY | `GeometryResourceManager::RegisterGeometry` |
| Texture  | 读 `.dds`，DDS 解析，创建 GPU 纹理 | （无，纹理在 cpuWork 中已创建） | 分配 SRV → `CreateShaderResourceView` → `TextureManager::RegisterTexture` |
| Material | （无——材质数据来自场景内联，不走 AssetManager） | — | — |

### SceneConstructor — 场景编排器

**职责**：资源句柄 → 场景构造数据（含材质 GPU buffer 上传）

- 接收 `SceneDescription`（已解析的场景定义）
- 收集依赖路径 → 调用 `AssetManager::LoadBatch`（meshes + textures）
- 所有依赖加载完成后，执行编排步骤：

```
Step 1: 遍历 textures → 查 m_cache → 建立 textureKey → SRV 索引映射
Step 2: 遍历 meshes  → 查 m_cache → 建立 meshKey → GeometryHandle 映射
Step 3: 遍历 materials（SceneDescription 中的内联材质定义）
        ├─ 材质中的纹理 key → 查 Step1 的 SRV 映射 → 填入材质数据
        └─ MaterialManager::RegisterMaterial → matKey → MaterialHandle 映射
Step 4: 组装 SceneConstructData（含 geoMap + matMap + entity 列表）
        └─ 存入 SharedDataStore
Step 5: 收集所有材质的 GPU 数据 → 提交材质 buffer COPY 上传 LoadTask
        └─ onComplete（主线程）:
              ├─ 分配 SRV → CreateShaderResourceView → SetMaterialBufferSRV
              └─ PostEvent(GeneratorTaskCompleteEvent, sceneId)
```

- **不直接创建 ECS 实体**
- **不直接用 BackgroundExecutor**——材质 buffer 上传如需辅助接口，通过 AssetManager 预留的通用提交方法，或直接使用 BackgroundExecutor（但此职责需明确归属）

### BackgroundExecutor — GPU 上传/回调调度引擎

**职责**：执行 LoadTask 的 CPU + GPU 工作，**不关心数据的业务含义**

- `cpuWork`：后台线程执行 CPU 密集型工作（文件 IO、解析、几何计算）
- `gpuWork`：后台线程录制 COPY/DIRECT 命令（Close 但不 Submit）
- `Tick` 主线程：
  - Phase 1: 清理已完成的 CPU taskflow
  - Phase 2: 收集就绪的 GpuWorkItem
  - Phase 3: Submit COPY → Signal → Submit DIRECT(Wait COPY) → Signal（不阻塞）
  - Phase 4: 非阻塞检查 fence → 调用 onComplete
  - Phase 5: 执行延后的主线程回调（无 GPU 工作的纯 CPU 任务）
- **唯一任务提交入口**：`SubmitTask` / `SubmitGraph` / `SubmitLoadTask`
- SceneConstructor 和 AssetManager 都通过 `SubmitLoadTask` 提交工作，不绕过此引擎
- **`SubmitGraph`（任务图依赖提交）为预留能力，当前无调用者**：磁盘资源加载（LoadTask）之间无依赖，天然并行（每个 LoadTask 包装为独立单任务 taskflow，并发跑在线程池上）；依赖只存在于加载完成后的句柄层。`TaskGraph` 为 move-only（拷贝已 delete），未来接入时调用方须 `std::move`

### SceneConstructSystem — ECS 构造器

**职责**：场景构造数据 → ECS 实体

- 响应 `GeneratorTaskCompleteEvent`（payload 高位编码生成器类型，低位携带 `sceneId`）
- 从 `SharedDataStore` 取出 `SceneConstructData`
- 遍历实体描述，**递归展平**为扁平 ECS Entity：
  - `TransformDesc` → `TransformComponent`
  - `MeshDesc` → `MeshComponent`（从 geoMap / matMap 取 handle）
  - `LightDesc` → 注册到 LightManager
  - `CameraDesc` → 注册到 CameraManager
  - 其他组件同理
- **不参与**资源加载、GPU 上传
- 构造完成后可触发后续 System（如渲染项构建器）

---

## 设计原则

1. **AssetManager 不组装**：只做文件→GPU 句柄的转换，不关心句柄如何被组合到场景
2. **SceneConstructor 不创建 ECS**：准备构造数据但不直接创建实体，通过事件解耦
3. **材质数据源统一**：运行时默认从场景 JSON 内联获取，`.mat` 仅作为编辑器导出/共享格式，不作为运行时默认管线的必经环节
4. **主线程安全**：所有 Manager 注册操作在 onComplete（主线程）中执行
5. **COPY 队列上传**：Mesh VB/IB 和材质 buffer 都通过 COPY 队列上传到 DEFAULT 堆，不直接在 UPLOAD 堆使用
6. **对称 Barrier**：后台线程录制 gpuWork 命令时，必须完成资源状态的完整转换并恢复到初始状态
7. **加载图并行、依赖建在句柄层**：磁盘资源加载之间无依赖关系，应全并行——并行来自 `tf::Executor` 线程池对多个独立单任务 taskflow 的并发调度，不来自任务图依赖。依赖只存在于加载完成之后的句柄层（Mesh/Material/Texture 三系统、ECS 组件引用 handle）。复合资产的"依赖"用**计数聚合**表达（`AssetBatch::completedCount` / CharacterLoader pending 计数），不用任务图依赖。单任务内部（cpuWork → gpuWork → onComplete）为线性串联，不构建复杂图

## 数据流（完整链路）

```
async_test.scene.json
  ↓
SceneLoader::LoadFromFile
  → SceneDescription (含内联材质定义，不含 .mat 文件路径)
  ↓
SceneConstructor::LoadScene
  ├─ 收集依赖 → AssetManager::LoadBatch(meshes + textures)
  │     ↓
  │   后台: Mesh::cpuWork(读文件+创建UPLOAD) → gpuWork(创建DEFAULT+COPY上传)
  │   后台: Texture::cpuWork(读文件+解析+创建纹理) → (无gpuWork)
  │     ↓
  │   主线程 onComplete: 注册 GeometryHandle / TextureHandle → m_cache
  │     ↓
  └─ OnDependenciesLoaded
       ├─ Step 1: textureKey → SRV 索引映射
       ├─ Step 2: meshKey → GeometryHandle 映射
       ├─ Step 3: 内联材质 → 填 SRV → RegisterMaterial
       ├─ Step 4: SceneConstructData → SharedDataStore
       └─ Step 5: 材质 buffer COPY 上传 LoadTask
             ├─ gpuWork(后台): 创建DEFAULT buffer + UPLOAD Map/memcpy + 录制COPY
             └─ onComplete(主线程):
                  ├─ 分配 SRV → CreateSRV → SetMaterialBufferSRV
                  └─ PostEvent(GeneratorTaskCompleteEvent)
                       ↓
                SceneConstructSystem:
                  ├─ GetData → SceneConstructData
                  └─ 遍历 entities → 创建 ECS Entity + 组件
```

## 与地形态式加载的对比

| 维度 | 地形态（旧模式） | 场景加载（新模式） |
|------|----------------|------------------|
| 材质数据源 | `.mat` 文件 | 场景 JSON 内联 |
| 纹理映射时机 | `TerrainGPUCreateSystem` 响应事件后 | `OnDependenciesLoaded` 编排阶段 |
| 加载完成通知 | `ResourceReadyEvent` → System 消费 | `GeneratorTaskCompleteEvent` → SceneConstructSystem 消费 |
| 注册时机 | System 收到事件后，手动读 SharedDataStore | SceneConstructor 统一注册，事件只传 sceneId |
| GPU 上传 | TerrainLoadTask 内部录制 | 材质 buffer 统一走 COPY 队列 |

## 未来扩展

### 外部材质引用

编辑器导出时，如需跨场景共享材质，可在场景 JSON 中添加引用语法糖：

```json
"materials": {
    "stone": { "$ref": "Materials/SharedStone.mat" }
}
```

加载器在 `SceneLoader::LoadFromFile` 阶段 resolve 为内联数据。运行时默认管线不受影响。

### 流式加载

后续扩展流式加载时，SceneConstructor 可复用同一套编排逻辑，仅入口改为"相机接近→触发局部场景加载"，不阻塞主线程。

**依赖图建设时机**（2026-08-03 定案）：磁盘资源本身无依赖，加载阶段应全并行；依赖建立在加载完成后的句柄层（几何-材质-纹理三系统、ECS 组件）。流式加载若出现**批次内强序**需求（如"高度图 → 地形生成 → 材质上传"必须按序），优先评估两条路：
1. **沿用计数聚合**（现状范式）：N 个并行 LoadTask + 计数回调归零触发下一步——地形等复合资产单 task 内部本就线性（cpuWork → gpuWork → onComplete），无需复杂图
2. **`SubmitGraph`**（任务图依赖，预留）：仅在确实存在跨 task 强序时启用，当前无调用者
