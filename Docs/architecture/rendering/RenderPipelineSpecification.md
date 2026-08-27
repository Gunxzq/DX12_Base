# 渲染管线规范

> 日期：2026-08-04
> 状态：📋 规范文档（2026-08-27 更新）
> 范围：渲染管线全流程、材质槽机制、水实体规范、扩展步骤、约束铁律
>
> **状态注记（2026-08-27）**：
> - 5 层模型主体（材质槽 → shaderType 桶 → Builder → Renderer → FrameDriver）为现行活跃规范；标准构建器/管线仍属早期模式（缺乏改造和利用）。
> - §10.6 桶编号/偏移铁律：**整节失效**——属 L2c 桶体系，被 **2026-08-18 无桶流程定案**取代（无桶/无槽、两张段表运行时生成、ExecuteIndirect 一次调用），保留为历史。
> - §6.1/§6.2 RenderPhase 为 2026-08-12 快照；权威枚举以 **`Engine/Scheduler/RenderPhase.h`** 为准（后续新增 `Dispatch` GPU 剔除 + `DerivedCollect` 派生渲染项阶段，阴影等挂 DerivedCollect）。
> - 公告牌：**环境公告牌管线**（EnvironmentBillboardRenderer）已废弃（2026-08-26）；`RenderPhase::Billboard` 阶段保留，标准公告牌构建器/管线属早期模式。
> - §11 已知缺陷：#2（Game 透明队列无消费）/#5（Game/Editor 阶段不一致）**已修复**；#1/#3/#4/#6 仍未修复。
> - §4.2 合批键：实际为 **5 字段 BatchKey**（含 startVertex）——CullData 不合批（1:1 实例），间接绘制命令按 key 合批（2026-08-27 用户确认）。

---

## 目录

1. [渲染管线总览（5 层模型）](#1-渲染管线总览5-层模型)
2. [ECS 组件层](#2-ecs-组件层)
3. [材质分桶层（RenderSlotCache）](#3-材质分桶层renderslotcache)
4. [构建器层（Builder）](#4-构建器层builder)
5. [渲染器层（Renderer）](#5-渲染器层renderer)
6. [提交阶段层（FrameDriver）](#6-提交阶段层framedriver)
7. [材质槽机制](#7-材质槽机制)
8. [水实体规范](#8-水实体规范)
9. [扩展渲染器/效果的标准步骤](#9-扩展渲染器效果的标准步骤)
10. [铁律与约束](#10-铁律与约束)
11. [已知缺陷登记](#11-已知缺陷登记)

---

## 1. 渲染管线总览（5 层模型）

整个渲染管线由 5 层组成，数据单向流动，每层职责清晰：

```
第 1 层：ECS 组件（数据源）
  │  RenderSlotCache::Rebuild（变更驱动，非每帧）
  ▼
第 2 层：缓存表 + 桶（分拣层）
  │  RenderSlotCache::DispatchAll / Dispatch（每帧，按 shaderType 分桶）
  ▼
第 3 层：构建器按桶处理（Builder 层）
  │  ForEachBucket(renderer_name) → 精确筛选 → 合批 → 自包含 RenderItem
  ▼
第 4 层：渲染器消费（Renderer 层）
  │  RenderSystem 录制命令列表 → SubmitRenderCommand(phase)
  ▼
第 5 层：FrameDriver 阶段执行
  │  ExecuteRenderPhase(phase) → GPU 执行
  ▼
最终帧
```

**核心原则**：
- 所有渲染器走同一流程，**包括水、透明物体、公告牌等**，一律走材质槽 → 桶 → Builder 按桶处理
- 禁止使用 `TransparentTag`/`OpaqueTag` 等标记组件替代材质槽分拣（已废弃，见 §10）
- 禁止 Builder 直接 `view` 遍历 ECS 替代桶消费（已废弃，见 §10）

---

## 2. ECS 组件层

### 2.1 核心组件

| 组件 | 字段 | 用途 |
|:--|:--|:--|
| `MeshComponent` | `lodMeshHandle`（LODMeshHandle） | 网格引用（通过 LODSystem 解析为 GeometryHandle） |
| | `localBounds`（BoundingVolumeVariant） | 本地包围盒（剔除用） |
| | `receivesShadow`（bool） | 阴影接收标记 |
| `RenderSlotComponent` | `slots[]`（vector<RenderSlot>） | 材质槽数组（见 §7） |
| `TransformComponent` | `position/rotation/scale` | 世界矩阵 |
| | `cullDistance`（float） | 剔除距离（0=无穷远） |
| `StaticComponent`（可选） | `cachedWorld` / `worldDirty` | 静态实体烘焙矩阵（编辑器 Gizmo 修改后置脏） |

### 2.2 组件序列要求

**所有可渲染实体**必须同时拥有以下组件：
- `MeshComponent`（几何引用）
- `RenderSlotComponent`（材质槽，至少一个有效槽位）
- `TransformComponent`（世界矩阵 + 剔除范围）

**不得使用**以下淘汰组件/标记：
- ❌ `TransparentTag`（改用材质槽 `shaderType == Transparent`/`Water`）
- ❌ `OpaqueTag`（改用材质槽 `shaderType == PBR`）
- ❌ `SkinnedTag`（改用材质槽 `shaderType == Skinned` + 几何条件 `IsSkinned()`）

---

## 3. 材质分桶层（RenderSlotCache）

### 3.1 数据结构

```
RenderSlotCache
├── m_slotTable: unordered_map<Entity, vector<RenderSlot>>  // 缓存表（驻留，变更驱动）
└── m_buckets: array<Bucket, ShaderType::Count>              // 桶（每帧分发产物）
    各 Bucket = vector<Entry>
    各 Entry 含：
      entity, slot, meshComp*, transformComp*, staticComp*
```

### 3.2 流程

**Rebuild**（变更驱动，非每帧）：

```
void RenderSlotCache::Rebuild(Registry &registry) {
    view<MeshComponent, RenderSlotComponent> 遍历
    → 每个实体，取 slotComp.slots（过滤无效槽位）
    → m_slotTable[entity] = slots[]
}
```

调用时机：实体增删改后（由变更方显式调用 `MarkDirty()`，`BuilderUpload` 每帧检查脏后重建）。

**DispatchAll**（Game 端，每帧 BuilderUpload 中调用）：

```
void RenderSlotCache::DispatchAll() {
    for (m_slotTable 中每实体) {
        for (slot : slots) {
            bucketIdx = static_cast<size_t>(slot.shaderType)
            m_buckets[bucketIdx].push_back({entity, slot, ...})
        }
    }
}
```

**ForEachBucket**（Builder 查询）：

```
void ForEachBucket(rendererName, fn) {
    for 每个 shaderType 的桶:
        route = FindMaterialRoute(shaderType)
        if route.renderer == rendererName → fn shaderType, 桶
        Unknown → fallback "opaque"（兼容历史资产）
}
```

### 3.3 桶 → 渲染器映射

由 `ShaderRoute.h` 的 `MaterialRoute` 表定义：

| ShaderType | renderer 名 | requireSkinned | 说明 |
|:--|:--|:--:|:--|
| `PBR` | `"opaque"` | false | 标准 PBR |
| `PBR_ClearCoat` | `"opaque"` | false | 清漆 PBR |
| `NPR` | `"opaque"` | false | NPR（预留） |
| `Skinned` | `"skinned"` | true | 蒙皮 |
| `Transparent` | `"transparent"` | false | 透明（预留，需实现 TransparentRenderer） |
| `Water` | `"water"` | false | 水（预留，需实现 WaterRenderItemBuilder 桶消费） |
| `Unknown` | fallback `"opaque"` | false | 兼容历史资产 |

---

## 4. 构建器层（Builder）

### 4.1 构建器规范

所有 Builder 必须遵循以下模式：

```
// 1. 通过桶获取数据，而非 ECS view
m_cache->ForEachBucket(renderer_name, [&](ShaderType, const Bucket &bucket) {
    for (const auto &entry : bucket) {
        // 2. 精确视锥筛选（粗筛基础上进一步精确）
        if (!FrustumCull(meshComp->localBounds, transform->GetMatrix(), frustum))
            continue;
        // 3. LOD 选择 → GeometryHandle
        geoHandle = PickLOD(lodMesh, cameraPos, LODConfig);
        // 4. 遍历材质槽的 subMeshRanges，按 BatchKey 分组合批
        for (range : slot.subMeshRanges) {
            BatchKey{ geoHandle, materialIdx, range.startIndex, range.indexCount }
            → 同 key 实体合并 instance 数组
        }
    }
});
// 5. VB/IB 地址解析 → 自包含 RenderItem（Draw 阶段零查询）
```

### 4.2 BatchKey（合批键）

```cpp
struct BatchKey {
    GeometryHandle geometry;       // 几何句柄
    uint32_t materialIdx;          // 材质 GPU 索引
    uint32_t startIndex;           // 索引区间起点
    uint32_t indexCount;           // 索引数
};
```

> **2026-08-27 用户确认代码形态**：实际 `BatchKey` 为 **5 字段**——`geometry / materialIdx / startIndex / startVertex (int32_t) / indexCount`
> （+ `operator==` 全字段比较），上方列表缺 `startVertex`。合批条件 = 完整 5 字段 key：
> - **CullData 不合批**（剔除票与实例 1:1）；
> - **间接绘制命令存在合批**（同 5 字段 key 的实体合并 instance 数组）——主要依据就是 key。

- 相同 `BatchKey` 的实体跨实体合批（实例化 `DrawIndexedInstanced`）
- `startVertex` 恒为 0（索引已绝对化，`BaseVertexLocation` 必须传 0）
- 每个 `subMeshRange` 生成一个 BatchKey，不同区间不能合并

### 4.3 渲染项自包含

RenderItem 必须固化以下字段，绘制阶段零查询：

```
OpaqueRenderItem (自包含):
  ├── geometryHandle       ← 几何句柄（用于 ResourceBarrier 等）
  ├── instanceBuffer       ← 实例数据 GPU 地址（FrameSync 回填）
  ├── instanceCount        ← 实例数
  ├── startIndex/indexCount ← 索引区间（来自 subMeshRange）
  ├── vbAddr/vbStride/vbSizeBytes ← 顶点缓冲 GPU 地址
  ├── ibAddr/ibSizeBytes   ← 索引缓冲 GPU 地址
  ├── indexFormat/topology ← 索引格式/拓扑
  └── probeIndex           ← 反射探针索引（可选）
```

### 4.4 当前 Builder 清单

| Builder | renderer_name | 桶消费？ | 阶段 | 备注 |
|:--|:--|:--:|:--|:--|
| `OpaqueRenderItemBuilder` | `"opaque"` | ✅ | PreRender/Worker | 标准实现 |
| `SkinnedRenderItemBuilder` | `"skinned"` | ✅ | PreRender/Worker | 标准实现 |
| `TerrainRenderItemBuilder` | `"terrain"` | ✅ | PreRender/Worker | 独立网格 |
| `TransparentRenderItemBuilder` | `"transparent"` | ❌ 用 ECS view | PreRender/Worker | **需迁移** |
| `WaterRenderItemBuilder` | `"water"` | ❌ 用 ECS view | PreRender/Worker | **需迁移** |

---

## 5. 渲染器层（Renderer）

### 5.1 渲染器规范

```
RenderSystem（alwaysRun，阶段 = Render，renderPhase = 对应阶段）：
  1. 分配命令列表
  2. 屏障：资源转到所需状态
  3. 设置视口/裁剪矩形
  4. OMSetRenderTargets（RT + DSV）
  5. 设置描述符堆
  6. Renderer::BeginFrame（设置 PSO/根签名/全局 CB）
  7. for each RenderItem:
       Renderer::DrawXxx(cmd, item.geometryHandle, ...)
  8. Renderer::EndFrame
  9. 屏障：资源恢复初始状态
  10. 命令列表 Close + SubmitRenderCommand(phase)
```

### 5.2 渲染器注册

```cpp
SystemRegistry::Register({
    .name = "OpaqueRenderSystem",
    .func = [this](const MessageContext &) { /* 录制 */ },
    .phase = TaskPhase::Render,
    .threadType = ThreadType::Render,
    .priority = TaskPriority::Normal,
    .renderPhase = RenderPhase::Opaque,  // ← 必须与 Submit 一致！
    .alwaysRun = true
});
```

**铁律**：`renderPhase` 字段必须与 `SubmitRenderCommand(phase, ...)` 的实际阶段一致。

---

## 6. 提交阶段层（FrameDriver）

### 6.1 RenderPhase 枚举

> ⚠️ **状态（2026-08-27）**：下列为 2026-08-12 快照。权威枚举以 `Engine/Scheduler/RenderPhase.h` 为准——
> 现行版本含 **`Dispatch`**（GPU 剔除 dispatch，首阶段）与 **`DerivedCollect`**（派生渲染项收集+渲染显式阶段，
> Dispatch 后、PrePass 前，阴影为首例）；PrePass 注释亦收敛为"清屏"。

```cpp
// 顺序 = FrameDriver::Tick 提交顺序（2026-08-12 重排，对齐 Tick 执行序列；HZB_Build 单开保证串行）
enum class RenderPhase : uint8_t {
    PrePass,           // 清屏、阴影、遮挡剔除（消费【上一帧】HZB）+ G-buffer
    Opaque,            // G-buffer 写入（不透明物体；生成当前帧深度图）
    HZB_Build,         // 构建 HZB（消费本帧深度图 → mip 链；供本帧 SSR/接触阴影 + 下一帧遮挡剔除）
    DynamicAOcclusion, // SSAO（G-buffer 就绪后；直接采样深度，不依赖 HZB）
    Lighting,          // 延迟光照（G-buffer + SSAO + 本帧 HZB → 交换链）
    SSR,               // 屏幕空间反射（Lighting 后、Transparent 前：G-buffer 法线/深度/粗糙度 + HZB 层级步进 → 半分辨率反射图 + 全场景菲涅尔合成，2026-08-12）
    Billboard,         // 公告牌（复用 Opaque 深度）
    Transparent,       // 透明物体（含水；水采样 SSR 反射图）
    PostProcess,       // 后处理、天空盒
    FSR3_Upscale,      // 超采样
    UI,                // 界面
    Count
};
```

### 6.2 执行顺序（FrameDriver::Tick）

> ⚠️ **状态（2026-08-27）**：下列为 2026-08-12 快照；实际执行顺序以 FrameDriver::Tick 为准——
> 现行版本在开头含 `ExecuteRenderPhase(Dispatch, 0)`（GPU 剔除）与 `ExecuteRenderPhase(DerivedCollect, 0)`
> （阴影等派生渲染项，见 §10.7）。

```
ExecuteRenderPhase(PrePass, 0)
ExecuteRenderPhase(Opaque, 0)           ← G-buffer 写入（生成当前帧深度图）
ExecuteRenderPhase(HZB_Build, 0)        ← 构建 HZB（消费本帧深度图 → mip 链）
ExecuteRenderPhase(DynamicAOcclusion, 0) ← SSAO（依赖 G-buffer 法线+深度）
ExecuteRenderPhase(Lighting, 0)         ← 延迟光照到交换链（SSR/接触阴影消费本帧 HZB）
ExecuteRenderPhase(SSR, 0)              ← 屏幕空间反射（2026-08-12）：HZB 层级步进 → 半分辨率反射图
                                          + 全场景合成（CompositePS：基底 sceneColor 拷贝 + 反射图 +
                                          G-buffer 粗糙度 → 菲涅尔叠加写回 sceneColor；水在 Transparent 采样反射图）
ExecuteRenderPhase(Billboard, 0)        ← 公告牌
ExecuteRenderPhase(Transparent, 0)      ← 透明物体 / 水（Lighting 之后）
ExecuteRenderPhase(PostProcess, 0)      ← 天空盒 / 后处理
ExecuteRenderPhase(FSR3_Upscale, 0)
ExecuteRenderPhase(UI, 0)
```

### 6.3 阶段分配规则

| 渲染器 | 阶段 | 备注 |
|:--|:--|:--|
| 不透明物体（PBR/NPR） | `Opaque` | 写 G-buffer（生成当前帧深度图） |
| 蒙皮不透明物体 | `Opaque` | 写 G-buffer |
| 地形 | `Opaque` | 写 G-buffer |
| HZB 构建 | `HZB_Build` | Opaque 之后：深度图 → mip 链（CS downsample；供本帧 SSR/接触阴影 + 下一帧遮挡剔除） |
| 公告牌 | `Billboard` | Lighting 之后，不写深度（2026-08-27：环境公告牌管线已废弃、阶段保留；标准公告牌构建器/管线属早期模式） |
| 透明物体（玻璃等） | `Transparent` | Lighting 之后，透明混合 |
| 水 | `Transparent` | Lighting 之后，透明混合 |
| 天空盒 | `PostProcess` | 最后覆盖 |
| 调试线框 | `PostProcess` | 天空盒之后（最上层） |
| SSAO | `DynamicAOcclusion` | Opaque 之后、Lighting 之前（直接采样深度，不依赖 HZB） |
| SSR（屏幕空间反射） | `SSR` | Lighting 之后、Transparent 前（2026-08-12）：SsrManager/SsrRenderer 全屏 quad——HZB 层级步进 → 半分辨率反射图 + CompositePS 菲涅尔合成写回 sceneColor（全场景光滑表面） |
| UI | `UI` | 最后 |

---

## 7. 材质槽机制

### 7.1 数据结构

```cpp
// RenderSlot（单个渲染槽位）
struct RenderSlot {
    Resource::MaterialHandle material;            // 材质句柄
    std::vector<SubMeshRange> subMeshRanges;      // 该材质覆盖的子网格区间
    Renderer::ShaderType shaderType;              // 渲染器标记（路由键）
};

// SubMeshRange（子网格区间）
struct SubMeshRange {
    uint32_t startIndex = 0;  // 索引区间起点（绝对索引）
    uint32_t indexCount = 0;  // 索引数
};

// RenderSlotComponent（一个实体挂载一个）
struct RenderSlotComponent {
    std::vector<RenderSlot> slots;  // 本实体全部渲染槽位
};
```

### 7.2 材质槽 → 网格信息定位链路

材质槽不直接持有网格句柄，通过以下链路定位：

```
JSON 场景:
  mesh.materials[] = ["mat_skin", "mat_armor", ...]    ← 材质 key 数组
  mesh.geometry = "character"                           ← 几何 key

SceneConstructor::ConstructEntity:
  1. geoMap["character"] → GeometryHandle              ← 依赖加载
  2. geoMgr->GetSubMeshInfo(geoHandle) → subMeshes[]   ← SubMesh 表
     ↓
  3. 槽位 i ↔ 子网格 i:
     slot.subMeshRanges = { subMeshes[i].startIndex, subMeshes[i].indexCount }
     slot.material = matMap["mat_xxx"]                  ← 材质句柄
     slot.shaderType = ParseShaderType(materialData.name)  ← 路由键
     ↓
  4. MeshComponent.lodMeshHandle = LODSystem::RegisterLODMesh({geoHandle})
     ↓

运行时 Builder:
  entry.meshComp->lodMeshHandle → LODSystem → PickLOD → GeometryHandle
  entry.slot.subMeshRanges      → BatchKey{ geometry, materialIdx, startIndex, indexCount }
  GeometryResourceManager::GetGeometry<TriangleMesh>(geoHandle) → VB/IB GPU 地址
```

**关键语义**：
- `MeshComponent` 持有 `LODMeshHandle`（LOD 级句柄），**不是** `GeometryHandle`（由 LODSystem 在构建时解析）
- 材质槽不持有任何句柄，只携带 `MaterialHandle` + 索引区间 + 渲染器标记
- 合批时，`geometry` 来自构建时解析的 `GeometryHandle`，`startIndex/indexCount` 来自材质槽
- `BaseVertexLocation` 恒为 0（索引已绝对化，禁止双重偏移）

### 7.2a 定案修正（2026-08-08）：槽位 = 材质（渲染器），子网格区间聚合

**问题（已修正）**：此前实现"槽位 i ↔ 子网格 i 一对一"（每个子网格一个材质槽），导致：
- 同一实体的每个材质段独立成桶，world 矩阵复制 N 份、CS 剔除同一包围球 N 次
- ExecuteIndirect 数 = 子网格数级（实测 1800 条，每桶 InstanceCount 仅 1~8）

**定案（对照大型引擎：UE GPUScene 实例级剔除 + Unity BRG materialID 批次）**：

| 层 | 语义 | 粒度 |
|:--|:--|:--|
| **实例（剔除票）** | 1 实体 = 1 包围球 = 1 次剔除，**不按子网格拆分** | 实体级 |
| **材质槽** | 按材质 key 聚合：同材质的多子网格区间并入同一槽（`subMeshRanges` 多段），材质本身携带渲染器标记（`MaterialData.rendererTypeHash` + shaderType） | 材质段级 |
| **桶（绘制命令）** | `{geometry, materialIdx}`（不含 startIndex/indexCount）；同材质多段并入一桶，`ExecuteIndirect MaxCommandCount=段数` 一次提交 | 材质段级 |

**关键认知**：材质切换必须分桶（无法一个 Indirect 画两种材质）——目标不是"1 条命令"，而是 **命令数 = 材质段数、剔除票 = 实体数**。

> ⚠️ **2026-08-27 修正**：上表"桶（绘制命令）= {geometry, materialIdx}（不含 startIndex/indexCount）、同材质多段并入一桶"
> 为 2026-08-08 历史定案，**不适用于现状**——实际合批条件为 **5 字段 BatchKey**（含 startIndex/startVertex/indexCount，见 §4.2），
> **不同子网格区间不合批**；CullData 不合批（1:1 实例），间接绘制命令按 key 合批。

### 7.3 材质路由

材质 `.mat` 文件的 `shader` 字段 → 渲染器路由：

```
.mat 文件:
  { "shader": "PBR/Standard", "params": { ... } }
    → ParseShaderType("PBR/Standard")           ← 大小写不敏感前缀匹配
    → ShaderType::PBR
    → FindMaterialRoute(ShaderType::PBR)
    → MaterialRoute{ renderer="opaque", requireSkinned=false, fallbackVariant="gbuffer" }
    → Builder 消费 "opaque" 桶
```

---

## 8. 水实体规范

### 8.1 JSON 表达（推荐方案）

```json
{
  "name": "Sea_WaterBody",
  "components": {
    "transform": {
      "position": [1500.0, 0.0, -1530.0],
      "rotation": [0.0, 0.0, 0.0, 1.0],
      "scale": [1.0, 1.0, 1.0],
      "cullDistance": 5000.0
    },
    "mesh": {
      "procedural": {
        "type": "grid",
        "width": 840.0,
        "depth": 780.0,
        "widthSegments": 32,
        "depthSegments": 32
      },
      "materials": ["Water"]
    },
    "water": {
      "amplitude": 0.5,
      "frequency": 1.0,
      "speed": 0.5,
      "direction": 0.0
    },
    "transparent": null
  }
}
```

### 8.2 JSON → ECS 组件映射

| JSON 字段 | ECS 组件 | 说明 |
|:--|:--|:--|
| `transform` | `TransformComponent` | 世界矩阵 + `cullDistance` 剔除范围 |
| `mesh.procedural` | `MeshComponent.lodMeshHandle` | 程序化 grid 网格（`GeometryProceduralTask` 生成后注册） |
| `mesh.materials[]` | `RenderSlotComponent.slots[]` | 材质槽：`{material=Water, subMeshRanges=[{0, indexCount}], shaderType=Water}` |
| `water` | `WaterComponent` | 波浪参数（amplitude/frequency/speed/direction） |
| `transparent: null` | ~~TransparentTag~~ | **已废弃**，未来由 `shaderType=Water/Transparent` 的材质槽替代 |

### 8.3 程序化网格（GeometryProceduralTask）

- `GeometryProceduralTask` 负责生成程序化网格（grid/sphere/cube）并注册到 `GeometryResourceManager`
- 注册后返回 `GeometryHandle`，同时写入 `SubMeshInfo` 表（1 条，`{0, indexCount, 0}`）和 `localBounds`
- `SceneConstructor` 从 `mesh.procedural` 字段（`ProceduralGeometryDesc` 类型）触发任务，生成 → 注册 → 挂 `MeshComponent.lodMeshHandle`
- 程序化网格的材质槽 `subMeshRanges` 从 `GetSubMeshInfo(geoHandle)` 填充，同标准实体

### 8.4 WaterManager 参数模式

`WaterManager` 的职责等同于 `LightManager`：

| 管理器 | 管理内容 | 数据源 | 不参与 |
|:--|:--|:--|:--|
| `LightManager` | 光源参数 → `LightConstants` CB | ECS `LightComponent` | 渲染（由 LightingPass 消费 CB） |
| `WaterManager` | 波浪参数 → `WaterConstants` CB | ECS `WaterComponent` | 渲染（由 WaterRenderer 消费 CB） |

- `WaterManager::CollectFromECS` 从 ECS 收集波浪参数（与 `LightManager` 一致）
- `WaterManager::UpdateAndUpload` 上传 `WaterConstants` CB，与 `LightManager::UpdateAndUpload` 一致
- 材质和几何由标准的 `MeshComponent` + `RenderSlotComponent` 承载，`WaterManager` 不参与

### 8.5 水块旧格式废弃

场景级 `waterBlocks[]` 数组（`WaterBlockDesc`：`min/max/world/tiling`，来自邻接 Sea 合并）为旧格式（.scene 二进制同构）。实体化改造后，每个 `waterBlock` 转换为一个水实体（程序化 grid + world 的 pos/scale 填 transform），`waterBlocks[]` 数组废弃。

---

## 9. 扩展渲染器/效果的标准步骤

新增一个渲染器/效果需按以下 5 步，缺一不可：

### Step 1：ShaderRoute 声明

`Engine/Renderer/Core/ShaderRoute.h`：

```cpp
// 1a. ShaderType 枚举新增
enum class ShaderType : uint8_t {
    Unknown = 0, PBR, PBR_ClearCoat, NPR, Skinned,
    Transparent, Water,
    // 新增: MyEffect,  // ← 新增
    Count
};

// 1b. ParseShaderType 新增前缀匹配
if (matchesHead("myeffect"))
    return ShaderType::MyEffect;

// 1c. FindMaterialRoute 新增路由
{ShaderType::MyEffect, "myeffect", false, "default"},
```

### Step 2：材质 `.mat` 文件

```json
{
  "shader": "MyEffect/Standard",
  "params": { ... }
}
```

### Step 3：Builder 注册（消费桶）

```cpp
// 3a. 创建 MyEffectRenderItemBuilder
// 3b. 注册 BuildMyEffect 系统（PreRender, Worker），消费 "myeffect" 桶
REGISTER_SYSTEM(BuildMyEffect, PreRender, Worker)
    .Func([this](const MessageContext &) {
        m_cache->ForEachBucket("myeffect", [&](ShaderType, const Bucket &bucket) {
            // 精确筛选 + 构建 MyEffectRenderItem
        });
    })
    .AlwaysRun()
    .DependsOn("BuilderUpload")
    .Build();
```

### Step 4：Renderer 实现 + RenderSystem 注册

```cpp
// 4a. 创建 MyEffectRenderer（继承 IRenderer）
// 4b. 注册 RenderSystem（alwaysRun, RenderPhase 与 Submit 一致）
SystemRegistry::Register({
    .name = "MyEffectRenderSystem",
    .func = [this](const MessageContext &) {
        // 录制命令列表
        // SubmitRenderCommand(RenderPhase::MyPhase, cmd)
    },
    .phase = TaskPhase::Render,
    .threadType = ThreadType::Render,
    .renderPhase = RenderPhase::MyPhase,  // ← 必须一致
    .alwaysRun = true
});
```

### Step 5：材质槽 JSON 使用

```json
{
  "mesh": {
    "geometry": "my_mesh",
    "materials": ["MyEffectMat"]
  }
}
```

---

## 10. 铁律与约束

### 10.1 桶模式铁律

- ❌ **禁止 Builder 使用 ECS `view` 直接遍历替代桶消费**（所有 Builder 必须走 `ForEachBucket`）
- ❌ **禁止使用 `TransparentTag`/`OpaqueTag`/`SkinnedTag` 标记组件**（全部由材质槽 `shaderType` 路由替代）
- ❌ **禁止新渲染器不声明 ShaderRoute**（每一步都必须补全 `ParseShaderType` + `FindMaterialRoute`）

### 10.2 渲染阶段铁律

- ❌ **禁止透明物体/水在 `Opaque` 或 `PrePass` 阶段录制**（必须走 `Transparent` 阶段，Lighting 之后、PostProcess 之前）
- ✅ **`renderPhase` 字段必须与 `SubmitRenderCommand(phase)` 一致**
- ✅ **不透明物体（PBR/Skinned/Terrain）必须在 `Opaque` 阶段写 G-buffer**

### 10.3 渲染项自包含铁律

- ✅ **Builder 构建时必须解析 VB/IB GPU 地址固化为 RenderItem，Draw 阶段零查询**
- ✅ **`BaseVertexLocation` 恒为 0**（索引已绝对化，禁止双重偏移）
- ✅ **`startVertex` 仅作记录，不参与绘制**

### 10.4 资源屏障铁律

- ✅ **每个 System 必须对称：入口转换 → 渲染 → 出口转换**（恢复资源到入口前状态）
- ✅ **禁止假设资源初始状态为 `COMMON`**（由 System 自己转）

### 10.5 数据上传铁律（Builder 不分配 CB / FrameSync 统一上传）

- ✅ **Builder 只填 CPU 侧数据**（矩阵/材质索引等），**禁止分配 GPU 资源**（CB/VB/IB 一律不归 Builder）
- ✅ **GPU 数据上传统一走 FrameSync 回调**：`FrameResourceManager::Allocate(name, data, size)` 从 RingBuffer 分配 + 上传，地址写回 RenderItem（对齐 `OpaqueRenderItem.instanceBuffer ← FrameSync_EditorUploadInstanceData`）
- ✅ **`ObjectConstants` 等每帧可变数据必须每帧经 RingBuffer 上传**，禁止常驻 UPLOAD CB 承载（矩阵变化、可见集变动时地址不稳，持久化为伪命题）
- ❌ **禁止在 `ConstructEntity` / `SceneConstructor` 中创建持久 CB 承载 World 矩阵**——水渲染早期旁路（`WaterObjCB_Persistent`）即因此只填 `MaterialIndex`、`World` 恒 0，导致 VS 输出全 0/NaN（RenderDoc 实测）。正确路径：Builder 填 `worldMatrix`/`materialIndex` → FrameSync `Allocate` 上传 → `DrawWater` 绑定地址
- ❌ **禁止 Renderer 拿 `worldMatrix` 参数但不写入上传数据**（数据必须落地上传，不能丢弃）

### 10.6 桶编号 / 偏移设计铁律（2026-08-15 新增，防偏移设计问题复发）

> ⚠️ **状态（2026-08-27）：整节失效**。本节（桶编号 FrameSync 单点分配/桶偏移表前缀和/bucketMap/每桶 args）
> 属 L2c 桶体系，已被 **2026-08-18 无桶流程定案**取代——无桶/无槽（旧桶段区/bucketIndex/槽位数组/桶偏移表全部废弃）、
> 回调同步只需拼接、两张段表（culldata 段表 + 批次段表前缀和）运行时生成、ExecuteIndirect 一次调用
> （MaxCommandCount=总命令数）。正文铁律保留为历史，不再适用。
> （这也解决了 `BlockBaking.md` 曾留的"§10.6 字面表述需修订"注记——整节失效后无需再修订。）

**背景**：阴影错乱排查（BugFix_ShadowMap_SubMeshSplit_StaticArgs）暴露"偏移设计"反复出错的根因——桶编号与偏移表的多点重复设计。以下铁律约束所有构建器/渲染器/剔除层：

- ✅ **桶按材质（shaderType）全局划分，仅一份**：`RenderSlotCache.m_buckets[shaderType]`（材质段级），不存在"消费桶子集"——构建器按 `ForEachBucket(rendererName)` 路由**完整消费**桶（Opaque→opaque、Water→water 等，无重叠无拆分）
- ✅ **桶编号（bucketIndex）全局唯一，单点分配**：唯一分配点为 `FrameSync`（`assignedBucket` 全局递增，Editor.cpp），**禁止构建器/渲染器自行分配桶编号**（并行构建器各自分配会冲突 → CS 桶段语义分裂 → 偏移错位）
- ✅ **偏移表（桶偏移表前缀和）全局一份，共享 COPY**：`CullingDataStore::ComputeBucketOffsets` 遍历全局 `m_bucketMap` 生成一份前缀和；主视口 COPY 到 `gIndirectArgs` 尾部、阴影 COPY 到 `shadowIndirectArgs` 尾部（**数据同源，资源隔离**）——禁止每构建器独立偏移表
- ✅ **构建器并行颗粒度为构建器级，禁止构建器内部分块（chunk）**：各构建器为独立 Worker system（TaskFlow 线程池并行），内部**单次完整遍历**桶；分块设计（BuildTypedChunk/MergeChunk）每 chunk 遍历完整桶 → 遍历重复浪费（8 次完整桶遍历导致 CPU 60%），已删除
- ✅ **PendingBatch 是构建器→FrameSync 的完整数据载体（非旧遗留）**：`instances/entities/instanceRadii` → allInstances/cullData、`queueIndex` → 回填渲染项、`bucketIndex` → 分配桶编号——FrameSync 逐字段消费，撤销 chunk 后仍必需
- ✅ **构建器可自处理渲染项内部局部偏移**（tempSlot/queueIndex/实例段局部索引，§4.2 自包含）；**跨构建器聚合（桶编号/bucketMap/allInstances/cullData）必须单点 FrameSync**——主线程不做构建工作，只做全局聚合
- ❌ **禁止新构建器/渲染器引入独立桶编号空间或独立偏移表**（同一桶被编号两次 → CS 桶段错位，阴影错乱同源问题）
- ❌ **禁止在构建器内部做分块遍历**（每 chunk 遍历完整桶，遍历重复浪费——2026-08-15 实证 8 次完整桶遍历致 CPU 60%）

### 10.7 FrameSync 回调语义铁律（2026-08-15 用户定案）

> ⚠️ **状态（2026-08-27）**：无桶流程（2026-08-18 定案）后 FrameSync 回调**只需拼接**——stg1/stg2
> （accum 聚合/扁平化）工作大半消亡（两张段表运行时生成）。本节"主线程 + 仅上传、并行必须走 system/TaskFlow、
> 派生渲染项挂 `DerivedCollect` 显式阶段（已在 `RenderPhase.h` 枚举确认存在）"等核心语义**仍有效**；
> stg1/stg2 的具体描述为历史记录。

**背景**：FrameSync 主线程耗时 5.5ms（stg1 accum 聚合 + stg2 扁平化）曾被 std::async 并行化（负优化，已回退），后 system 化移入 `EditorFrameAccumFlatten`（PreRender/Worker，TaskFlow 并行）→ 主线程回调仅剩 0.33-0.47ms。**FrameSync 回调保留的语义是主线程 + 仅上传**，禁止改造为 system：

- ✅ **FrameSync 回调 = 主线程 + 仅上传语义**：立即回调保留因语义差异——主线程同步点（FrameDriver PreRender 之后执行）且**只包含上传语义**（RingBuffer Allocate/memcpy/SetCullData/SetFlatInstances/SetShadowBucketDrawArgs）——禁止将其计算逻辑改造为 system（上传必须在主线程同步点，GPU 消费顺序依赖）
- ✅ **计算逻辑（stg1 accum 聚合 + stg2 扁平化）应移入 Worker system**（`EditorFrameAccumFlatten`，PreRender/Worker + TaskFlow 并行）——主线程关键路径只留消费/上传；产出经共享成员（m_frame*）交回调消费
- ✅ **并行化必须复用引擎 system/TaskFlow 机制**：**禁止 std::async/裸线程**绕过 system 做并行（每帧线程创建 + 小数据分块 + 合并成本 → 负优化实证：FrameSync 5.5→6.7-9.9ms、帧率下降）；system 化后 FrameSync 5.5→0.33ms、帧率回升（18:26 段 58.3 FPS）
- ✅ **派生渲染项（阴影为首例）必须单开显式 RenderPhase 阶段**：`RenderPhase::DerivedCollect`（Dispatch 后、PrePass 前）——阴影贴图/未来贴花/程序化派生等派生渲染项统一挂此显式阶段，**禁止假定只有阴影存在此需求**；阴影渲染 system 的 `.renderPhase` 与 `SubmitRenderCommand(phase)` 必须一致（规则 24）
- ❌ **禁止 FrameSync 回调内做重计算**（聚合/扁平化/几何解析——5.1ms 已移出）；回调只消费 m_frame* 成员（SetCullData/ApplyBucketIndices/上传）

---

## 11. 已知缺陷登记

### #1 Editor 水渲染录制在 Opaque 阶段

- **文件**：`Editor/EditorLib/Core/Editor.cpp:1155-1190`
- **问题**：`DrawWater` 被写在 `EditorOpaqueRenderSystem` 的命令列表内，和实体 G-buffer 同一阶段、同一命令列表
- **影响**：透明物体被当不透明画进 G-buffer，混合/深度语义全错
- **修复方向**：拆出独立 `EditorWaterRenderSystem`，注册 `RenderPhase::Transparent`
- **优先级**：P1

### #2 Game 端透明实体队列无消费系统 ✅ 已修复（2026-08-27 用户确认）

- **问题**：`m_transparentQueue` 由 `TransparentRenderItemBuilder` 构建 + `FrameSync` 上传 ObjectConstants，但**没有任何渲染系统消费它**（搜不到 `TransparentRenderSystem`）
- **影响**：透明实体（玻璃等）JSON 定义后可见但永远不渲染
- **修复方向**：实现 `TransparentRenderSystem` 消费 `m_transparentQueue`，注册 `RenderPhase::Transparent`
- **优先级**：P1

### #3 WaterRenderItemBuilder 未迁移桶模式

- **问题**：`WaterRenderItemBuilder` 使用 `view<WaterComponent, MeshComponent, TransformComponent, TransparentTag>` 替代 `ForEachBucket("water")`
- **影响**：水实体必须额外挂 `TransparentTag`，材质槽 `shaderType=Water` 被忽略
- **修复方向**：迁移 `WaterRenderItemBuilder` 为桶消费模式，删除 `TransparentTag` 依赖
- **优先级**：P1（与 #1/#2 联动）

### #4 ProceduralGeometryDesc 未接入 MeshDesc

- **问题**：`MeshDesc.geometry` 只支持 string key（依赖 .dxmesh），不支持 object（程序化网格）
- **影响**：水实体 JSON 的 `mesh.procedural` 字段无法被 SceneConstructor 处理
- **修复方向**：`GeometryProceduralTask` 实现后，`SceneConstructor::ConstructEntity` 增加 `mesh.procedural` 分支（生成→注册→挂组件）
- **优先级**：P1

### #5 Game 与 Editor 渲染阶段不一致 ✅ 已修复（2026-08-27 用户确认）

- **问题**：Game 端水在 `RenderPhase::Transparent`，Editor 端水在 `RenderPhase::Opaque`
- **影响**：相同场景在 Game 和 Editor 中渲染结果不同
- **修复方向**：两端统一阶段语义
- **优先级**：P2

### #6 水渲染持久 CB 旁路（World 矩阵恒 0）

- **文件**：`SceneConstructor.cpp:800-818`（`WaterObjCB_Persistent`）
- **问题**：违反 §10.5 数据上传铁律——`ConstructEntity` 创建持久 UPLOAD CB 承载 `ObjectConstants`，`memset` 后只填 `MaterialIndex`，**`World`/`WorldInvTranspose` 恒 0**；`WaterRenderer::DrawWater` 收到 `worldMatrix` 参数却从未写入 CB
- **影响**：VS 中 `mul(posL, gWorld)=0`、`normalize(mul(N, gWorldInvTrans))=NaN`（RenderDoc 实测 VS 输出全 0/NaN），水体渲染几何塌缩在原点
- **修复方向**：删除持久 CB；`WaterRenderItemBuilder` 填 CPU 侧 `worldMatrix`/`materialIndex` → FrameSync `Allocate` 上传 → `DrawWater` 绑定地址（对齐 `OpaqueRenderItem.instanceBuffer` 模式）
- **优先级**：P1（水渲染不可见的直接根因）