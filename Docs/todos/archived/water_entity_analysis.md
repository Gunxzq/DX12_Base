# 水实体重构阶段 1-2 分析清单

> 日期：2026-08-04
> **状态（2026-08-27 实现细节核对 + 复订）：✅ 阶段 1/2 已投入使用**（部分形态与本清单计划不同，见分节注记）：
> - **总体定案（2026-08-27）**：`waterBlocks[]` 已废弃——旧二进制 .scene 格式极其落后，**不考虑使用、不考虑兼容性**（用户定案）。
>   水实体现行主路径 = **JSON 标准实体模式**（复合标准实体：`mesh.geometry = "procedural://grid/W/H/segX/segZ"` + `materials: ["Water"]` + `water` 组件），
>   走 Engine `SceneConstructor` **标准构建流**（procedural URI → AssetManager 虚拟资产 → 标准组装 MeshComponent/RenderSlotComponent + 水组件）
> - **阶段 1（waterBlocks 解析 + JSON 往返）**：曾按本清单位置实施（§3.1）——但 to_json 缺口以**不同形式**闭合：`WaterBlockDesc` 仍无 struct to_json，`SceneLoader.cpp` 手工构建 waterBlocks JSON；**2026-08-27 该 JSON 解析/序列化已随 N11 清理删除**（废弃格式，二进制侧读取保留、属 N12 无限延期）
> - **水实体构建路径**：⚠️ **非**本清单设想的 waterBlocks[] 转换——JSON 标准实体走 Engine SceneConstructor 标准路径；
>   `EditorSceneManager.cpp:1130` 的 waterBlocks 消费循环（同步建实体+TransformComponent，异步 `GeometryProceduralTask` 生成网格后 onComplete 组装 MeshComponent+RenderSlotComponent）
>   是**不走标准构建流的旧二进制格式遗留路径**（JSON 场景 waterBlocks 恒空、循环永不触发），属非标准行为——**2026-08-27 已清理**（`remaining_issues.md` N11 ✅；二进制格式本体见 N12 无限延期）
> - **阶段 2（GeometryProceduralTask）**：✅ 已实施——实际文件 `Engine/Background/GeometryProceduralTask.h`（非 `Resource/Procedural/` 路径）；接口 = `Create()` **三段式异步任务**（cpuWork 生成 CPU 网格 → gpuWork 建 GPU 资源+COPY → onComplete 注册 GeometryResourceManager），非 §4.1a 的同步 `Execute()` 设计
> - **阶段 2（程序化几何接入）**：⚠️ 形态变化——`MeshDesc.procedural` 字段 + `geometry: string|object` oneOf 方案**未采用**，改为 **`procedural://` URI 约定**（`MeshDesc.geometry` 保持 string，SceneConstructor 收集 URI → AssetManager 虚拟资产 → 标准组装）
> - **Schema（§4.3）**：`ProceduralGeometryDesc` 定义 ✅（scene.schema.json:448）；`water` 组件 / `waterBlocks` 场景级属性 / `cullDistance` 定义**未补**
> - 本清单已归档为历史分析文档，调试日志插入（§6）随当时实施。
> 目标：分析当前 City.scene.json 与 schema 的 gap，明确阶段 1（MPD .scene 解析→水实体构建）
> 和阶段 2（GeometryProceduralTask）需要的改动点，以及调试日志的插入位置。
> 关联：`Docs/architecture/rendering/RenderPipelineSpecification.md` §8、
> `Docs/todos/archived/remaining_issues.md` §三

---

## 一、目标状态回顾（推荐方案）

### 1.1 水实体 JSON 表达（阶段 2 完成后）

```json
{
  "name": "City_Water_0",
  "components": {
    "transform": {
      "position": [1500.0, 0.0, -1530.0],
      "rotation": [0.0, 0.0, 0.0, 1.0],
      "scale": [840.0, 1.0, 780.0],
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
    }
  }
}
```

### 1.2 目标 ECS 组件序列

| 组件 | 数据来源 | 关键字段 |
|:--|:--|:--|
| `TransformComponent` | world 的 pos/scale + cullDistance | position, rotation, scale, cullDistance |
| `MeshComponent` | GeometryProceduralTask 注册的 GeometryHandle | lodMeshHandle, localBounds |
| `RenderSlotComponent` | 材质槽：shaderType=Water, subMeshRanges=[{0, indexCount}] | slots[0].material, slots[0].shaderType |
| `WaterComponent` | 波浪参数（旧格式无此字段，设默认值） | amplitude, frequency, speed, direction |

---

## 二、现状分析

### 2.1 City.scene.json 的 waterBlocks 表达

**文件**：`Content/City/City.scene.json`（末尾 247513-247540 行）

```json
"waterBlocks": [
  {
    "max":  [1920.0, -1140.0],
    "min":  [1080.0, -1920.0],
    "tiling": [28.0, 26.0],
    "world": [1500.0, 0.0, -1530.0, 0.0, 0.0, 0.0, 1.0, 840.0, 1.0, 780.0]
  }
]
```

**`world` 数组解码**：`[posX, posY, posZ, rotX, rotY, rotZ, rotW, scaleX, scaleY, scaleZ]`

| 索引 | 字段 | 值 | 说明 |
|:--:|:--|:--:|:--|
| 0-2 | pos | 1500, 0, -1530 | 世界位置 |
| 3-6 | rot | 0, 0, 0, 1 | 单位四元数（无旋转） |
| 7-9 | scale | 840, 1, 780 | 缩放（X 宽 840, Z 深 780, Y 高度 1） |

**`min/max`**：角点坐标 `[1080, -1920] ~ [1920, -1140]`，用于验证 tiling 计算
**`tiling`**：`[28, 26]` = 宽/30=28, 深/30=26（每 30 单位重复一次）

**格式差异**：

| 维度 | 旧格式（waterBlocks） | 推荐方案（实体） | 转换方式 |
|:--|:--|:--|:--|
| 位置 | `world[0:3]` | `transform.position` | 直接赋值 |
| 旋转 | `world[3:7]` | `transform.rotation` | 直接赋值 |
| 缩放 | `world[7:10]` | `transform.scale` | 直接赋值 |
| 剔除 | 无 | `transform.cullDistance` | 设默认值 5000 |
| 几何 | 隐含四边形（min/max 角点） | `mesh.procedural` | grid 的 width=depth=scaleX, depth=scaleZ |
| 材质 | 无 | `mesh.materials[]` | 固定 `["Water"]` |
| 波浪参数 | 无 | `water` | 设默认值 |
| 标记 | 无 | — | 不走 `transparent: null`（材质槽 shaderType 替代） |

### 2.2 Schema 现状

**文件**：`Schemas/scene.schema.json`

| 定义 | 现状 | 缺口 |
|:--|:--|:--|
| **Entity.components** | 无 `water` | 缺少 `water` 组件定义 |
| **MeshComponent** | `geometry` 是 string only（`required: ["geometry"]`） | 不支持 `geometry: { type: "grid", ... }` 程序化描述 |
| **waterBlocks** 场景级 | schema 无 `waterBlocks` 属性定义 | 缺场景级 waterBlocks 数组定义 |
| **opaque/transparent** | TagComponent（`type: "null"`） | 保留兼容，但应标注"已废弃" |
| **Entity.allOf** 约束 | 无 water 条件 | 缺 `mesh + water` 组合的约束 |
| **TransformComponent** | 无 `cullDistance` | 缺剔除范围字段 |

---

## 三、阶段 1 改动清单

### 3.1 SceneLoader 改动

> **2026-08-27 核对**：3.1a（二进制解析 `SceneLoader.cpp:70-82`）/ 3.1b（JSON 解析 `SceneLoader.cpp:181-195`）✅ 已实施；
> 3.1c（to_json）以不同形式闭合——`WaterBlockDesc` 仍无 struct to_json，`SceneLoader.cpp:255-267` 手工构建 waterBlocks JSON（往返可用）。

**目标**：二进制 .scene 的 waterBlocks 解析 + JSON 序列化

#### 3.1a 验证二进制 .scene 的 waterBlocks 解析

**文件**：`Engine/Asset/IO/Loader/SceneLoader.cpp`

**已有代码**（`LoadSceneBinary`，~70-80 行）：
```cpp
for (uint32_t i = 0; i < header.waterBlockCount; ++i) {
    DxSceneWaterBlock wb{};
    ifs.read(reinterpret_cast<char *>(&wb), sizeof(DxSceneWaterBlock));
    // ... 填充 WaterBlockDesc
    desc.waterBlocks.push_back(std::move(wd));
}
```

**需验证**：`DxSceneWaterBlock` 的 min/max/world 字段与 City.scene.json 的 waterBlocks 数据一致。

#### 3.1b 验证 JSON 场景的 waterBlocks 解析

**文件**：`Engine/Asset/IO/Loader/SceneLoader.cpp`

**`ParseSceneJSON` 分析**：检查 `contains("waterBlocks")` 分支是否存在。

**缺口**：如果不存在，需新增：
```cpp
if (j.contains("waterBlocks") && j["waterBlocks"].is_array()) {
    for (const auto &wb : j["waterBlocks"]) {
        WaterBlockDesc wd;
        // 解析 min/max/world/tiling 数组
        desc.waterBlocks.push_back(std::move(wd));
    }
}
```

#### 3.1c 新增 `waterBlocks` 的 JSON 序列化（to_json）

**文件**：`Engine/Asset/IO/Loader/SceneDescription.h`

**缺口**：`WaterBlockDesc` 有 `struct` 和 `from_json`，但**缺少 `to_json`**。

需新增：
```cpp
inline void to_json(nlohmann::json &j, const WaterBlockDesc &w) {
    j = nlohmann::json::object();
    j["min"] = w.min;
    j["max"] = w.max;
    j["world"] = w.world;
    if (!w.tiling.empty())
        j["tiling"] = w.tiling;
}
```

同时 `SceneDescription` 的 `to_json` 需增加：
```cpp
if (!waterBlocks.empty())
    j["waterBlocks"] = waterBlocks;
```

#### 3.1d 调试日志

**插入位置**：`SceneLoader::LoadSceneBinary` 和 `ParseSceneJSON` 中 waterBlocks 解析后

```cpp
log->Info("[SceneLoader] Loaded {} water blocks", desc.waterBlocks.size());
for (size_t i = 0; i < desc.waterBlocks.size(); ++i) {
    const auto &wb = desc.waterBlocks[i];
    log->Info("[SceneLoader] waterBlock[{}]: pos({},{},{}) scale({},{},{}) min({},{}) max({},{})",
              i, wb.world[0], wb.world[1], wb.world[2],
              wb.world[7], wb.world[8], wb.world[9],
              wb.min[0], wb.min[1], wb.max[0], wb.max[1]);
}
```

### 3.2 SceneConstructor 改动

> **2026-08-27 核对**：⚠️ 本节的 waterBlocks[] 转换方案**已废弃**（2026-08-27 用户定案：二进制 .scene 格式极其落后，
> 不考虑使用、不考虑兼容性）——水实体现行主路径 = **JSON 标准实体模式**（procedural:// URI + water 组件，
> 走 Engine SceneConstructor 标准构建流）。核查中发现的"水实体构建"即 `Editor/EditorLib/Scene/EditorSceneManager.cpp:1130` 的
> waterBlocks 循环（同步建实体+TransformComponent，异步 `GeometryProceduralTask` 后 onComplete 组装 MeshComponent+RenderSlotComponent）——
> 属**旧二进制 .scene 格式的遗留路径、不走标准构建流**（JSON 场景 waterBlocks 恒空、循环永不触发），非标准行为，清理候选（`remaining_issues.md` N11）。

**目标**：`OnSceneConstructReady` 中将 `waterBlocks[]` 转换为标准实体

#### 3.2a 水实体构建逻辑

**文件**：`Engine/Scene/SceneConstructor.cpp`

**位置**：`OnSceneConstructReady` 末尾（entity 构建循环之后，或单独循环）

```cpp
// 将 waterBlocks[] 转换为标准水实体
for (size_t i = 0; i < sceneData->waterBlocks.size(); ++i) {
    const auto &wb = sceneData->waterBlocks[i];
    
    // 1. 创建实体
    auto entity = registry->CreateEntity();
    std::string name = "WaterBlock_" + std::to_string(i);
    registry->AddComponent<ECS::NameComponent>(entity, name);
    
    // 2. TransformComponent
    DirectX::XMFLOAT3 pos(wb.world[0], wb.world[1], wb.world[2]);
    DirectX::XMFLOAT4 rot(wb.world[3], wb.world[4], wb.world[5], wb.world[6]);
    DirectX::XMFLOAT3 scl(wb.world[7], wb.world[8], wb.world[9]);
    registry->AddComponent<ECS::TransformComponent>(entity, pos, rot, scl, 5000.0f);
    
    // 3. MeshComponent（← GeometryProceduralTask 注册的网格，阶段 2 才就绪）
    // 阶段 1 暂不挂 MeshComponent，先验证 Transform + WaterComponent 完整性
    
    // 4. RenderSlotComponent（依赖 MeshComponent 的 subMeshInfo，阶段 2 才完整）
    
    // 5. WaterComponent（波浪参数：旧格式无此字段，设默认值）
    ECS::WaterComponent waterComp;
    waterComp.amplitude = 0.5f;
    waterComp.frequency = 1.0f;
    waterComp.speed = 0.5f;
    waterComp.direction = 0.0f;
    // objectCBAddress 由 WaterManager 运行时分配
    registry->AddComponent<ECS::WaterComponent>(entity, waterComp);
    
    log->Info("[SceneConstructor] Created water entity '{}': pos({},{},{}) scale({},{},{})",
              name, pos.x, pos.y, pos.z, scl.x, scl.y, scl.z);
}
```

#### 3.2b 调试日志

**插入位置**：`OnSceneConstructReady` 中 waterBlocks 转换后

```cpp
log->Info("[SceneConstructor] Created {} water entities from waterBlocks", sceneData->waterBlocks.size());
// 验证组件完整性
auto waterView = registry->view<ECS::WaterComponent>();
for (auto ent : waterView) {
    auto &wc = waterView.get<ECS::WaterComponent>(ent);
    auto *name = registry->TryGetComponent<ECS::NameComponent>(ent);
    auto *tf = registry->TryGetComponent<ECS::TransformComponent>(ent);
    auto *mesh = registry->TryGetComponent<ECS::MeshComponent>(ent);
    auto *slot = registry->TryGetComponent<ECS::RenderSlotComponent>(ent);
    log->Info("[SceneConstructor] Water entity '{}': Transform={} Mesh={} Slot={}",
              name ? name->name.c_str() : "?",
              tf ? "✓" : "✗",
              mesh ? "✓" : "✗",
              slot ? "✓" : "✗");
}
```

### 3.3 阶段 1 验证条件

| 条件 | 验证方式 |
|:--|:--|
| 二进制 .scene 的 waterBlocks 解析正确 | 日志输出 waterBlock 数量 + 各字段值 |
| JSON 场景的 waterBlocks 解析正确 | 日志输出同上 |
| waterBlocks 的 to_json 序列化正确 | 编辑器导出场景后验证 JSON 字段 |
| SceneConstructor 创建水实体 | 日志输出"Created N water entities from waterBlocks" |
| 组件序列完整 | 日志输出 Transform/Mesh/Slot 的 ✓/✗ |

---

## 四、阶段 2 改动清单

### 4.1 GeometryProceduralTask（新建）

> **2026-08-27 核对**：✅ 已实施——实际文件 `Engine/Background/GeometryProceduralTask.h`（非下文 `Resource/Procedural/` 路径）；
> 接口 = `static LoadTask Create(type, params, device, cmdMgr, geoMgr, outResult)` **三段式异步任务**
> （cpuWork: GeometryGenerator → CPU MeshData；gpuWork: 建 DEFAULT VB/IB + UPLOAD 中转 → 录 COPY + barrier；
> onComplete: 注册 GeometryResourceManager），非 §4.1a 的同步 `Execute()` 接口设计。

**文件**：`Engine/Resource/Procedural/GeometryProceduralTask.h/.cpp`

#### 4.1a 接口设计

```cpp
class GeometryProceduralTask {
public:
    // 生成程序化网格并注册到 GeometryResourceManager
    // @param type "grid" | "sphere" | "cube"
    // @param params 几何参数（width/depth/segments 等）
    // @return 注册后的 GeometryHandle
    static Resource::GeometryHandle Execute(
        const std::string &type,
        const ProceduralGeometryDesc &params,
        Resource::GeometryResourceManager *geoMgr,
        ID3D12Device *device);
};
```

#### 4.1b grid 类型生成逻辑

```cpp
if (type == "grid") {
    float w = params.width / 2.0f;
    float d = params.depth / 2.0f;
    uint32_t ws = std::max(1u, params.widthSegments);
    uint32_t ds = std::max(1u, params.depthSegments);
    
    uint32_t vertexCount = (ws + 1) * (ds + 1);
    uint32_t indexCount = ws * ds * 6;
    
    // 生成顶点（位置 + 法线 + 切线 + UV）
    // 生成索引（三角形条带）
    // 填充 SubMeshInfo: { startIndex=0, indexCount=indexCount, startVertex=0 }
    // 计算 localBounds: min(-w, 0, -d), max(w, 0, d)
    
    // 注册到 GeometryResourceManager
    // 返回 GeometryHandle
}
```

#### 4.1c 调试日志

```cpp
log->Info("[GeometryProceduralTask] Generated grid: {}x{} ({} segments x {} segments) -> {} vertices, {} indices",
          w, d, ws, ds, vertexCount, indexCount);
log->Info("[GeometryProceduralTask] Registered geometry handle: index={} generation={}",
          handle.index, handle.generation);
```

### 4.2 SceneConstructor 改造（MeshComponent 支持 procedural）

> **2026-08-27 核对**：⚠️ 形态变化——§4.2c 的 `MeshDesc.procedural` 字段 + `geometry: string|object` oneOf 方案**未采用**，
> 改为 **`procedural://` URI 约定**：`MeshDesc.geometry` 保持 string（`SceneDescription.h:96-98`），
> SceneConstructor 收集实体中的 `procedural://` URI（`SceneConstructor.cpp:103-109/184-190`）→ AssetManager 虚拟资产 → 标准组装；
> oneOf（string|object）形态目前仅 `SkyboxDesc` 使用（`SceneDescription.h:420-426`）。

**文件**：`Engine/Scene/SceneConstructor.cpp`，`ConstructEntity` 的 mesh 分支

#### 4.2a 当前逻辑（~604 行）

```cpp
auto it = geoMap.find(m.geometry);  // ← 只支持 string key
if (it != geoMap.end() && it->second.IsValid()) {
    GeometryHandle geoHandle = it->second;
    // 构建 MeshComponent + RenderSlotComponent
}
```

#### 4.2b 新增程序化分支

```cpp
if (m.procedural) {
    // 调用 GeometryProceduralTask::Execute(type, params, geoMgr, device)
    GeometryHandle geoHandle = GeometryProceduralTask::Execute(m.procedural->type, *m.procedural, ...);
    if (geoHandle.IsValid()) {
        // 构建 MeshComponent + RenderSlotComponent（同标准分支）
        // subMeshRanges: [{0, indexCount}] — 程序化网格只有 1 个子网格
        // shaderType: 从 materials[0] 的材质名解析
    }
} else {
    // 现有逻辑：geoMap.find(m.geometry)
}
```

#### 4.2c MeshDesc 结构体改动

**文件**：`Engine/Asset/IO/Loader/SceneDescription.h`，`MeshDesc`

```cpp
struct MeshDesc {
    std::string geometry;                                              // 旧：string key
    std::optional<ProceduralGeometryDesc> procedural;                  // 新：程序化几何
    std::vector<std::string> materials;
    bool receivesShadow = true;
};
```

`from_json` 改动：
```cpp
if (j.contains("geometry")) {
    const auto &g = j["geometry"];
    if (g.is_string()) {
        m.geometry = g.get<std::string>();
    } else if (g.is_object()) {
        m.procedural = g.get<ProceduralGeometryDesc>();
    }
}
```

`to_json` 改动：
```cpp
if (!m.geometry.empty()) {
    j["geometry"] = m.geometry;
} else if (m.procedural) {
    j["geometry"] = *m.procedural;
}
```

### 4.3 Schema 改动

> **2026-08-27 核对**：部分落地——`ProceduralGeometryDesc` 定义 ✅（scene.schema.json:448）；
> 4.3a/4.3b（`water` 组件定义）/ 4.3d（`waterBlocks` 场景级属性）/ 4.3e（WaterBlockDesc 定义）/
> 4.3f（allOf water 约束）/ 4.3g（`cullDistance`）**schema 未见、未补**（C++ 端 `WaterDesc` 序列化已落地；
> schema 补齐对齐项目约定"JSON 资产 schema 用于校验、后续处理"）。

**文件**：`Schemas/scene.schema.json`

#### 4.3a Entity.components 新增 `water`

```json
"water": { "$ref": "#/definitions/WaterComponent" }
```

#### 4.3b 新增 WaterComponent 定义

```json
"WaterComponent": {
    "type": "object",
    "properties": {
        "amplitude": { "type": "number", "default": 0.5, "minimum": 0 },
        "frequency": { "type": "number", "default": 1.0, "minimum": 0 },
        "speed": { "type": "number", "default": 0.5, "minimum": 0 },
        "direction": { "type": "number", "default": 0.0, "description": "风向（弧度）" }
    }
}
```

#### 4.3c MeshComponent.geometry 支持 string 或 object

```json
"geometry": {
    "description": "几何体：\"mesh_key\"（文件引用）或 {\"type\": \"grid\", ...}（程序化生成）",
    "oneOf": [
        { "type": "string" },
        { "$ref": "#/definitions/ProceduralGeometryDesc" }
    ]
}
```

同时 `required: ["geometry"]` 保留（因为 geometry 字段始终存在，只是类型不同）。

#### 4.3d 场景级 `waterBlocks` 属性定义

```json
"waterBlocks": {
    "type": "array",
    "items": { "$ref": "#/definitions/WaterBlockDesc" },
    "description": "水块数组（邻接 Sea 合并——程序化水面四边形）。旧格式，新场景推荐使用 entities 中的 water 实体。"
}
```

#### 4.3e 新增 WaterBlockDesc 定义

```json
"WaterBlockDesc": {
    "type": "object",
    "properties": {
        "min": { "type": "array", "items": { "type": "number" }, "minItems": 2, "maxItems": 2 },
        "max": { "type": "array", "items": { "type": "number" }, "minItems": 2, "maxItems": 2 },
        "world": { "type": "array", "items": { "type": "number" }, "minItems": 10, "maxItems": 10 },
        "tiling": { "type": "array", "items": { "type": "number" }, "minItems": 2, "maxItems": 2 }
    }
}
```

#### 4.3f Entity allOf 约束新增 water 条件

```json
{
    "description": "水体: transform + mesh + water",
    "if": { "required": ["water"] },
    "then": { "required": ["transform", "mesh"] }
}
```

#### 4.3g TransformComponent 新增 cullDistance

```json
"cullDistance": {
    "type": "number",
    "default": 0,
    "minimum": 0,
    "description": "剔除距离，0=无穷远"
}
```

#### 4.3h opaque/transparent 标注废弃

```json
"opaque": { "$ref": "#/definitions/TagComponent", "description": "【已废弃】由材质槽 shaderType 替代" },
"transparent": { "$ref": "#/definitions/TagComponent", "description": "【已废弃】由材质槽 shaderType 替代" },
```

---

## 五、改动的依赖关系图

```
Phase 1:
  SceneLoader 3.1a/3.1b/3.1c (waterBlocks 解析 + to_json)
    │ 无依赖
    ▼
  SceneConstructor 3.2a (waterBlocks → 实体，不含 MeshComponent)
    │ 依赖 SceneLoader 的解析结果
    ▼
  【验证】日志输出组件序列（含水 WaterComponent 无 MeshComponent）

Phase 2:
  GeometryProceduralTask 4.1 (grid 生成 + 注册)
    │ 独立新建
    ▼
  SceneConstructor 4.2 (ConstructEntity 的 procedural 分支)
    │ 依赖 GeometryProceduralTask + MeshDesc 改造
    ▼
  MeshDesc 4.2c (geometry 支持 object)
    │ 依赖 SceneDescription.h 改动
    ▼
  Schema 4.3 (同步更新)
    │ 依赖所有以上改动
    ▼
  【验证】日志输出完整组件序列（含 MeshComponent + RenderSlotComponent）
```

---

## 六、调试日志总览

| 位置 | 日志内容 | 验证目的 |
|:--|:--|:--|
| `SceneLoader::LoadSceneBinary` | `waterBlock[0]: pos(...) scale(...)` | 二进制解析正确 |
| `SceneLoader::ParseSceneJSON` | `waterBlock[0]: pos(...) scale(...)` | JSON 解析正确 |
| `SceneConstructor::OnSceneConstructReady` | `Created N water entities from waterBlocks` | 实体创建成功 |
| `SceneConstructor::OnSceneConstructReady` | `Water entity 'X': Transform=✓ Mesh=✗ Slot=✗` | 组件序列完整性 |
| `GeometryProceduralTask::Execute` | `Generated grid: ... -> N vertices, M indices` | 网格生成正确 |
| `GeometryProceduralTask::Execute` | `Registered geometry handle: ...` | 注册成功 |
| `SceneConstructor::ConstructEntity` | `Entity 'X' slot#0: 'Water' -> handleIndex=... shaderType=Water` | 材质槽填充正确 |
| `SceneConstructor::ConstructEntity` | `WaterComponent added to 'X': wave=(...)` | 水组件完整 |