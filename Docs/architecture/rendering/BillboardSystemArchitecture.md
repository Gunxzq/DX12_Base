# 公告牌系统架构设计

> 目标：替代硬编码的 `CreateBillboardTrees`，提供 JSON 驱动的公告牌渲染。
> 公告牌没有 MeshComponent，是纯 Sprite/GS 驱动的渲染类型。
>
> **状态（2026-08-26 更新）**：方案四（XCross 环境公告牌 + City 树接入）的已实施内容（XCross 模式/
> 渲染管线/资产序列化）**已全部回退**。回退背景：原将帧率下降归因于公告牌管线属**误判**——实际根因是
> **CPU 侧 FrameSync（间接绘制参数预备）**：每帧为生成 IndirectArgs，CPU 需逐实体（或逐桶）执行
> `SetBucketDrawArgs`（Map/memcpy）+ `Allocate`，线性增长的 CPU 开销（**不是 ECS 遍历，也不是 GPU 采样纹理**）；
> 剔除系统已完成重构，现以最高效模式运行。`EnvironmentBillboardRenderer` 已废弃、**待重新调整设计**
> （#708 跨描述符堆根因已定位，恢复时可参考）。
> 另 2026-08-26 **方向定案**：4 行矩阵位置纹理（数据纹理 → 着色器实时采样解码）判定为**错误方向**，
> 资产确定要调整——见「位置纹理定案」节。本文其余章节（二分法/方案一~三）保留为重新设计时的设计储备。

---

## 概述

公告牌系统需要覆盖三种使用场景：

| 场景 | 实例数 | 使用方式 |
|:-----|:-------|:---------|
| 手动放置 | 1~10 | 场景设计师拖放（路标、装饰物） |
| 程序化体积 | 100~1000 | 树林/草丛区域，用参数生成 |
| GPU 噪声 | 1000~10000+ | 大规模植被，用 Geometry Shader 采样噪声图 |

三者不互斥——同一场景中三种可以共存。

> **2026-08-15 架构更新（材质槽模式 + GS 扩展）**：
> - **系统已变材质槽模式**：公告牌应走 RenderSlotCache 5 层模型（ECS 组件 → 材质槽 → 桶 → 构建器 → 渲染器），与 Opaque/阴影构建器一致——`BillboardComponent` 挂材质槽（shaderType 路由），`BillboardRenderItemBuilder` 走 `ForEachBucket` 消费桶（对齐 RenderPipelineSpecification.md §4.4 Builder 清单），**不再用 ECS view 直接遍历**（旧 Builder 用 `view<BillboardComponent, TransformComponent>()`，需迁移桶模式）
> - **GS 扩展公告牌不需要程序化几何体**：公告牌用 GS（Geometry Shader）把点展开成四边形/双交叉平面（XCross，见方案四）后，**方案二/三的程序化体积/噪声生成对公告牌无必要**——GS 直接发射顶点，无需 CPU 体积生成或噪声图程序化实例（方案二/三降级为"静态实体通用程序化"参考，非公告牌路径；见各方案注记）

---

## 公告牌二分法（2026-08-15 用户定案：LOD 代理 vs 环境装饰）

公告牌技术在游戏开发中有两种**截然不同**的应用本质——设计哲学、实现方式、资源管理均不同：

| 维度 | **LOD 代理公告牌** | **环境装饰公告牌** |
|:--|:--|:--|
| 本质 | 3D 模型**替身**（性能优化） | 2D 场景**布景**（美术表现） |
| 核心目标 | 性能优化（廉价面片替代远距离 3D 模型） | 美术表现（背景填充/氛围营造） |
| 生命周期 | **动态**（随 LOD 切换：近 3D → 远公告牌） | **静态**（始终存在，无模型切换） |
| 图像来源 | 3D 模型**预渲染生成**（SpeedTree 等） | 美术**手绘**/照片处理 |
| 是否有"本体" | **有**（对应 3D 模型，交互由碰撞体负责） | **没有**（自身即本体） |
| 逻辑关系 | 间接（可能代理可交互实体） | 完全无关（纯视觉布景） |
| 引擎体现 | Unity `BillboardAsset`/`BillboardRenderer`、UE HLOD | Unity Quad + Billboard Shader、UE `MaterialBillboardComponent` |

**架构归属**：
- **LOD 代理公告牌** = 性能优化模块产物——归属 `LODComponent`（渲染系统按距离动态启用）
- **环境装饰公告牌** = 场景美术资源——**视为一种资源**（公告牌管理器管理），非实体


---

## 方案一：手动放置（ECS 标准模式）

### JSON
```json
{
    "name": "Signpost",
    "components": {
        "transform": { "position": [10, 0, 5] },
        "billboard": {
            "texture": "sign_tex",
            "material": "billboard_mat",
            "width": 1.0,
            "height": 2.0,
            "mode": "axisY"
        }
    }
}
```

### 数据流
```
SceneLoader::ParseEntity → BillboardDesc
  ↓
ConstructEntity → BillboardComponent + TransformComponent（标准 ECS）
  ↓ (PreRender, Worker)
BillboardRenderItemBuilder::BuildTyped
  └─ view<BillboardComponent, TransformComponent>()
  ↓
BillboardRenderSystem (PostProcess)
  └─ 已有 BillboardRenderer，只需接入队列
```

### 架构状态
- `BillboardComponent` ✅ 已存在
- `BillboardRenderItemBuilder` ✅ 已存在
- `BillboardDesc` / `SceneLoader` 解析 ✅ 已有
- 队列 + RenderSystem：注册 `BuildBillboard` + `BillboardRenderSystem` 即可恢复

---

## 方案二：程序化体积（CPU 生成）

> ⚠️ **2026-08-15 重新评估**：公告牌用 GS 扩展（XCross/点展开，见方案四）后**不需要程序化几何体**——本方案（CPU 体积生成 N 个实例）降级为"静态实体通用程序化"参考（石头/灌木等静态网格），**非公告牌路径**（GS 直接发射顶点，无需 CPU 生成实例坐标）

### JSON
```json
{
    "name": "Forest",
    "components": {
        "transform": { "position": [-20, 30, -20] },
        "billboard_volume": {
            "texture": "tree_array",
            "material": "billboard_mat",
            "area": [40, 0, 40],
            "density": 0.1,
            "minWidth": 1.0, "maxWidth": 3.0,
            "minHeight": 2.0, "maxHeight": 5.0,
            "randomSeed": 42
        }
    }
}
```

### 新增

| 文件 | 说明 |
|:-----|:------|
| `SceneDescription.h` | `BillboardVolumeDesc` 结构体 |
| `SceneLoader.cpp` | `ParseBillboardVolume` 解析 |
| `SceneConstructor.cpp` | `ConstructEntity` 遇到 `billboard_volume` 时按密度+种子生成 N 个位置 → 创建 N 组 `BillboardComponent + TransformComponent` |

### 数据流
```
JSON "billboard_volume"
  ↓ ConstructEntity
for i in 0..N:
  pos = random_in_area(area, density, seed)
  w = random_range(minWidth, maxWidth, seed+i)
  h = random_range(minHeight, maxHeight, seed+i)
  entity = registry->CreateEntity()
  registry->AddComponent<TransformComponent>(entity, pos, ...)
  registry->AddComponent<BillboardComponent>(entity, { texture, material, w, h })
  ↓ (PreRender, Worker，同一方案一的 Builder)
BillboardRenderItemBuilder → TRenderQueue<BillboardRenderItem>
```

### 特点
- JSON 只有几行，不存 N 个实例坐标
- 同一种子总是生成相同分布，可复现
- N 个 ECS 实体，适合编辑器导出
- N 较大时（万级）CPU 分配和 entt 存储有开销

---

## 方案三：GPU 噪声生成（Geometry Shader）

> ⚠️ **2026-08-15 重新评估**：公告牌用 GS 扩展（XCross/点展开，见方案四）后**不需要程序化几何体**——本方案（GS 采样噪声图发射实例）对公告牌无必要（GS 已直接展开四边形，无需噪声图程序化实例分布）；降级为"静态实体通用程序化"参考（大规模植被的密度分布控制可保留），**非公告牌路径**

### JSON
```json
{
    "name": "Forest",
    "components": {
        "transform": { "position": [-20, 30, -20] },
        "billboard_volume": {
            "texture": "tree_array",
            "material": "billboard_mat",
            "area": [40, 0, 40],
            "noiseMap": "forest_noise.dds",
            "densityThreshold": 0.4,
            "width": 2.0,
            "height": 4.0,
            "maxCount": 5000
        }
    }
}
```

### 新增

| 文件 | 说明 |
|:-----|:------|
| `ECS/Core/Components/BillboardVolume.h` | `BillboardVolumeComponent`（存参数，不含实例） |
| `RenderItemBuilder/BillboardVolumeRenderItem.h/.cpp` | Builder 扫描 `BillboardVolumeComponent` |
| `Pipeline/BillboardVolumeRenderer.h/.cpp` | 新的 GS 渲染器 |
| `Shaders/BillboardGS.hlsl` | Geometry Shader（采样噪声图→发射顶点） |

### 数据流
```
ConstructEntity → BillboardVolumeComponent（1 个实体）
  ↓ (PreRender)
BillboardVolumeBuilder → BillboardVolumeRenderItem { area, noiseMapSRV, params }
  ↓ (PostProcess)
BillboardVolumeRenderer:
  ├─ VS: 传入四边形（AABB）
  ├─ GS: 采样 noiseMap → 密度值 > threshold → 在 area 内发射实例
  │       每个纹素对应 area 内的一个位置
  │       支持 maxCount 限制
  └─ PS: 常规公告牌着色（图集采样 + axisY 旋转）
```

### 特点
- **1 个 ECS 实体** = 成千上万的公告牌
- 不需要 N 次 entt 存储分配
- 密度修改只需调 `densityThreshold`，无需重建场景
- 适合大规模（10K+）但每帧 GS 有顶点发射开销

---

## 方案四：XCross 交叉平面树（场景树免 mesh，2026-08-15）

> **状态（2026-08-26）**：本节曾实施内容（XCross 模式 + 渲染管线 + 资产序列化）**已全部回退**（用户确认）。
> 帧率下降根因误判——实际为 **CPU 侧 FrameSync（间接绘制参数预备）**：每帧逐实体（或逐桶）执行
> `SetBucketDrawArgs`（Map/memcpy）+ `Allocate` 生成 IndirectArgs，线性增长的 CPU 开销
> （不是 ECS 遍历，也不是 GPU 采样纹理）；剔除系统已重构至最高效模式，与公告牌管线无关；
> `EnvironmentBillboardRenderer` 需**重新调整设计**后再推进本节路径。

### 背景

City 场景大量树（`tree`/`tree2`/`tree5` 共 **3767 棵**）当前走 `MeshComponent` 网格渲染——迁移为**公告牌 XCross 渲染**（免 mesh，GS 点展开双交叉平面，**不朝向相机旋转**）。

原始资产：树由几组构成（**两个树贴图交叉** + 一部分旋转）——X 交叉双平面任意角度至少一面朝向，**无需朝向相机旋转**（区别于 AxisY/Full/Spherical 的朝向相机模式）。

**City 树定位（2026-08-15 用户定案）：环境装饰公告牌**——树是装饰品（静态布景，无 3D 真身/LOD 切换/交互），归属"环境装饰公告牌"类（见上文二分法）：

```
场景 JSON 位置：sceneEnvironment（环境级顶层，与天空盒 skybox/ambient 同级）
  → 树群/植被/远景布景作为【环境资源】配置（非 entities 实体级）
  → 环境装饰公告牌 = 场景美术资源（公告牌管理器管理），视为一种资源处理
```

**数据化处理收益**：树（3767，27.5% 实体）移出实体系统 → 剔除（逐实体 → 块级）、同步（免逐实例 World 上传，GPU buffer/纹理化）大幅优化。

### XCross 模式（2026-08-15 曾实施，2026-08-26 已回退）

| 位置 | 改动 |
|:--|:--|
| `Engine/ECS/Core/Components/Render.h` | `BillboardMode` 枚举加 **`XCross`** |
| `Shaders/Billboard.hlsl` | GS `maxvertexcount(4)→(8)` + XCross 分支（Mode==3）：0°（XZ 平面）+ 90°（YZ 平面）双交叉平面，`RestartStrip` 每平面独立 strip |

### 大型引擎参考（Unity）

- **Cross-plane billboards**（交叉平面）= 本方案同款——适合几百~几千棵中小场景
- **Camera facing**（面向相机）= 百万级森林唯一可行（本项目树 X 交叉已覆盖任意角度，不需）
- **Impostor / LOD 过渡** = 远期可选（运行时多角度贴图 / 近 3D 远 billboard 淡化）

**光照限制**：billboard 树只接收**方向光**（不接点光/聚光，光照随相机旋转更新）——City 方向光已满足。

### City 树分布稀疏性（上限难达，2026-08-15 用户定案）

**虽然 City 包含大量树（3767 棵），但并非集中分布**——树分散在场景各处：

- 交叉平面（Cross-plane）方案的上限瓶颈（百万级集中森林灾难——Unity URP 实测 1.8 FPS）只在**集中分布**场景（单一视锥内同时可见巨量树）触发
- City 树**稀疏分布**：任意视锥内可见的树远低于 3767 总量 → **交叉平面上限难以达到** → XCross 方案足够，无需 camera facing / impostor

### 接入路径（2026-08-26：曾实施部分已回退，待重新设计）

| 优先级 | 项 |
|:--|:--|
| **P1** | tree 实体（3767）→ `BillboardComponent(XCross)` + 树贴图（SceneConstructor 识别 tree 网格生成 billboard 组件） |
| **P2** | BillboardBuilder 批量实例化（3767 实例一次 drawcall 级）+ **可见性剔除（路线 1 Compaction：CS 读 row3 → AppendBuffer → Compact 到紧凑目标 → DrawInstanced(0, 存活数) 连续区间）** |
| **P3** | LOD 过渡（近 3D 远 billboard 淡化，对齐 Unity fade） |
| **流式（后续）** | 路线 2 分块连续区间（空间有序布局 + 视锥覆盖区块每块连续命令）——VT/流式加载时启用 |

**注意**：树 `castsShadow=false`（此前阴影排除项）——billboard 树同样不投射（方向光阴影用 3D 替代或后续）；树贴图复用 tree 实体 materials 引用的贴图（Texture2DArray）。

---

## 公告牌管理器编辑器模式（2026-08-15 用户定案）

### 模式结构

公告牌管理器（块纹理资源，非 ECS 实体）——编辑器/运行时双视图：

```
公告牌管理器（块纹理 + sceneEnvironment 环境参数，非 ECS）
  ├─【大纲视口】纹理反序列化数据输入——大纲显示公告牌资源数据（非 ECS 驱动）
  │    → 数据视图：块/树群/单树列表 + 参数（位置/旋转/尺寸/贴图/模式）
  ├─【选中 → 属性卡】选中公告牌 → 序列化 ECS 组件 → 驱动属性卡编辑（临时实体化）
  │    → 复用属性卡（ComponentEditorSystem：选中实体 → 属性编辑）
  └─【运行时下游】剔除-构建器-渲染器 → 依赖纹理/环境参数（块纹理 + sceneEnvironment），非 ECS
```

### 关键设计

| 设计点 | 内容 | 依据 |
|:--|:--|:--|
| **大纲非 ECS 驱动**（数据视图） | 大纲显示公告牌资源数据（纹理反序列化），非 ECS 实体 | 对齐 AnimationViewport 资产级大纲（大型引擎模式：资产级大纲非场景实体）；免建 3767 实体 |
| **选中时序列化 ECS 驱动属性卡** | 选中公告牌 → 反序列化 ECS 组件 → 属性卡编辑（临时实体化，复用现有能力） | 资产 ↔ ECS 双视图：编辑时实体化、运行时资源化；无常驻 ECS |
| **下游依赖纹理/环境参数非 ECS** | 剔除（块级纹理采样）/构建器（块纹理渲染项）/渲染器（GS 采样）零 ECS 依赖 | sceneEnvironment"不进入 ECS Registry"语义（SceneDescription.h:727）；树移出实体系统 |

### 双向同步链路（编辑 → 纹理 → 下游）

```
属性卡编辑（选中公告牌，序列化 ECS 组件）
  → 回写块纹理数据（位置/旋转/尺寸/贴图变化）
  → 重生成块纹理（分布编辑/单树修改）
  → 运行时下游（剔除-构建器-渲染器）采样新纹理
```

### 注意点（实施时）

- **大纲粒度**：3767 树按块/树群聚合显示（非 3767 行）
- **字段映射**：公告牌参数（位置/旋转/尺寸/贴图/模式）↔ ECS 组件字段（临时序列化）
- **编辑器参照**：AnimationViewport 资产级大纲模式（编辑器独立于运行时）+ EditorPanelSystem（IEditorPanel/Dock/语言包）

---

## 位置纹理定案（2026-08-15：解耦 + 容量 + 紧凑化）

> **方向调整（2026-08-26）**：本节 4 行矩阵纹理方案判定为**错误方向**——在着色器中实时解码空间矩阵数据
> （数据纹理 → GS 采样 → 重组矩阵 → 生成顶点）增加着色器复杂度、带宽消耗，且难以调试。**资产确定要调整**
> （4 行 `positionTexture` 不再沿用）。
> - **错误方向**：数据纹理（如矩阵纹理）→ 着色器实时解码 → 生成顶点/矩阵（复杂度↑/带宽↑/难调试）
> - **正确方向**：数据纹理（如灰度图、高度图）→ CPU/离线工具 → 转换为标准几何数据（顶点、实例缓冲区）→ 着色器直接使用
> - 数据可以存到纹理中，但一般以**灰度图模式**（密度/高度分布）使用、由 CPU/离线工具消费——而非数据纹理供着色器采样解码
>
> 以下小节（4 行纹理/序列化产物/连续区间策略/紧凑目标）均属错误方向，保留为历史参考，
> 待按"标准几何数据（实例缓冲区）"新方向重新论证。

### 解耦结论（块纹理对齐剔除系统 = 耦合化，用户定案）

- **块纹理方案（废弃）**：位置纹理按剔除块（blocksPerAxis/cellSize）拆分——公告牌资源【耦合】剔除系统（块配置变更 → 位置纹理重建）
- **全局位置纹理（定案）**：位置纹理全局一张（不按剔除块拆分），采样按【视锥可见性】（VT/流式方向）——**公告牌资源独立于剔除系统**（解耦）

### 容量分析（City 地图实测：1875 × 1905 米 = 357 万平方米）

| 指标 | 值 | 判定 |
|:--|:--|:--|
| 2048×2048 纹素 | 4,194,304（419 万实例容量） | ✅ 远大于 City 树 3767（0.09%） |
| 地图匹配分辨率（若按地图布局） | 0.916 × 0.930 米/纹素 | ⚠️ 地图匹配 = 灰度图问题（见下） |
| City 实体全量 | 6854（persistentId 13708） | ✅ 也不足容量 0.4% |

### 紧凑化定案（用户期望：纹素 = 实例，非地图匹配）

**位置纹理不与地形地图匹配**（若匹配 = 每纹素对应地图格子 = 灰度图——稀疏位置大量空纹素，浪费）：

- **紧凑布局**：纹素 = 实例（实例密集排列），纹理尺寸 = f(实例数)，非地图面积
- City 树 3767 → **64×64 纹理**即可（或 128×32——按实例数取矩形）
- 容量余量巨大（419 万 vs 3767 = 1111×）——支持更大地图/更密植被扩展，但**纹理始终紧凑**（按实际实例数）
- 采样：纹素索引 = 实例索引（GS 按索引采样，非按地图 UV）

> **2026-08-15 规划变更（float4x4 → 4 张行纹理）**：位置纹理存储完整变换矩阵（实例可独立旋转——XCross 固定朝向 ≠ 实例不可旋转，用户定案）。float4x4 64B/纹素**非硬件原生格式**（RGBA32F 纹素原生 16B），拆为 **4 张 RGBA32F 行纹理**（各存矩阵一行，16B/纹素原生格式）：
> - 索引 0/1/2 = 矩阵 row0/1/2（旋转缩放行，行主序 R*S）
> - 索引 3 = row3（位置行 (px,py,pz,1)）——**可见性剔除/流式只采样此张**（位置单独一张，剔除零矩阵重组）
> - 显存总量不变（4×4096×16B = 256KB）；GS 顺序采样 4 次重组矩阵（4096×4 ≈ 16K 次采样，可忽略）
>
> ⚠️ **2026-08-26**：本注记的 4 行矩阵纹理（数据纹理 → 着色器实时采样解码）已判定为**错误方向**，资产确定要调整——见节首方向调整。

```json
// 公告牌复合资产（billboardGroups 数组元素）
{
    "texture": "tree_tex",
    "positionTexture": ["trees_pos_row0", "trees_pos_row1", "trees_pos_row2", "trees_pos_row3"],
    "aspectRatio": [1.0, 2.0],  // 2026-08-15 定案：宽高 [width, height]（世界单位），非宽高比
    "mode": "xcross",
    "castShadow": false,
    "instanceCount": 3767
}
// positionTexture：4 张 RGBA32F 行纹理（纹素=实例，16B/纹素原生格式，紧凑布局）
// 尺寸 64×64（City 树 3767 → 4096 纹素容量）；instanceCount 显式限定 GS 采样范围（尾部零纹素不误渲染）
```

### 序列化产物（2026-08-15 曾落地，2026-08-26 随方向回退、4 行 DDS 判错误方向——资产确定要调整）

| 文件 | 内容 |
|:--|:--|
| `Schemas/billboard_groups.schema.json` | 复合资产 Schema（draft-07，billboardGroups：texture/positionTexture[4]/aspectRatio/mode/castShadow/instanceCount） |
| `Content/City/city_trees_billboard.json` | 复合资产（树 3767：texture=tree / positionTexture=4 行纹理 / aspectRatio=[1,2] / instanceCount=3767） |
| `Content/City/city_trees_pos_row0.dds` | 位置纹理 row0（64×64 RGBA32F，旋转缩放行） |
| `Content/City/city_trees_pos_row1.dds` | 位置纹理 row1（64×64 RGBA32F，旋转缩放行） |
| `Content/City/city_trees_pos_row2.dds` | 位置纹理 row2（64×64 RGBA32F，旋转缩放行） |
| `Content/City/city_trees_pos_row3.dds` | 位置纹理 row3（64×64 RGBA32F，位置行 (px,py,pz,1)——剔除只采样此张） |

> ⚠️ **node 可用性**：当前 bash 会话 node 不可用（PATH 未刷新），node 在 D 盘（新终端 PATH 自然可用）。本次序列化用 perl 备用方案完成（JSON::PP + DDS 二进制）；node 可用后重写脚本（见 `Docs/todos/BillboardManager_Replan.md` P1）。

### 连续区间策略（2026-08-15 用户定案：数据纹理 vs Buffer 的适用边界）

> ⚠️ **2026-08-26**：本节前提（4 行矩阵位置纹理 + 着色器实时采样）已判定为**错误方向**（见节首方向调整）——
> 下文"位置纹理方案成立、不迁移 StructuredBuffer"结论与路线 1/路线 2/紧凑目标定案全部**失效**，保留为历史参考；
> 新方向为 CPU/离线工具将数据纹理（灰度图/高度图）转换为标准几何数据（实例缓冲区），着色器直接使用。

**背景**：业界观点认为 GPU-Driven + Indirect Draw 下应弃纹理转 StructuredBuffer（乱序 Gather 缓存抖动）。但该观点适用于"**百万级集中实例 + 乱序索引列表 + 数据超出 L2 缓存**"的场景。公告牌恰好全部避开：

| 条件（缓存抖动成立前提） | 公告牌场景 | 判定 |
|:--|:--|:--|
| 数据量超出缓存 | 4×64×64×16B = **256KB**，现代 GPU L2 4-8MB——**全量常驻 L2**，任意访问均缓存命中 | ❌ 不满足 |
| 乱序 Gather 索引（AppendBuffer） | 公告牌**解耦剔除系统**（VT/流式方向），**不接入 L2c 乱序路径**；且 GS 点展开 `SV_InstanceID` 本身就是实例索引，无需 AppendBuffer 中间层 | ❌ 不满足 |
| 数据纹理用 Sample 过滤（采样器开销） | 位置纹理按纹素索引访问——**用 `Load()` 而非 `Sample()`**，硬件路径与 Buffer 随机访问一致 | ❌ 不满足 |

**结论**：位置纹理方案（4 张行纹理）成立，**不迁移 StructuredBuffer**。

**关键事实**：公告牌 GS 路径比网格"天生连续"——网格 ExecuteIndirect 必须从 `gAppendBuffer[SV_InstanceID]` 读存活 ID 再间接读矩阵（乱序 Gather）；公告牌每实例 1 个点，`SV_InstanceID` 即实例索引，只要数据排列到 `[0..N-1]` 连续区间，GS 顺序采样即顺序访问。

**确保 IndirectArgs 连续区间的两条路线**：

```
路线 1（主选）：Compaction 重打包——每帧一次 copy，IndirectArgs 单命令 [0..N-1]
  CS 剔除：读 row3 位置行（+半径）→ 视锥判断 → AppendBuffer 存活 ID
  CS Compaction：按 AppendBuffer 顺序把 4 行矩阵从位置纹理 copy 到紧凑目标
  绘制：DrawInstanced(0, 存活数) ← 索引 [0..N-1] 完全连续，GS 顺序采样缓存完美
  开销：每帧 3767×64B ≈ 239KB copy（微秒级）；工业标准（UE5 Nanite/Unity 实例压缩）

路线 2（流式加载用）：空间有序布局 + 分块连续区间——零 copy
  位置纹理按空间排序布局（Morton/Z-order：空间邻近 → 纹素邻近，仍全局一张、紧凑布局）
  视锥 → 覆盖区块列表（每块连续纹素区间）→ 每块 ExecuteIndirect：StartInstance=块偏移, InstanceCount=块实例数
  注意：非"块纹理方案"（废弃的按块物理拆分 = 耦合剔除）；此为布局有序 + 每块一个连续命令，纹理仍全局一张、不耦合
```

| 维度 | 路线 1 Compaction | 路线 2 分块连续 |
|:--|:--|:--|
| IndirectArgs 连续性 | 单命令 [0..N-1]，**最优** | 每块连续，块数 = 视锥覆盖区块数 |
| 每帧开销 | 一次 239KB copy | 零 copy，纯布局 |
| 剔除粒度 | 实例级（精确） | 区块级（保守，块内可能含视锥外实例） |
| 实现复杂度 | 中（多一个 CS + 紧凑目标资源） | 中（位置纹理需按空间排序，生成脚本改） |
| 与位置纹理解耦定案 | 兼容 | 兼容（纹理仍全局一张） |
| 适用阶段 | **当前/常规渲染（P2 主选）** | **后续 VT/流式加载**（区块流式天然按块连续） |

> **2026-08-15 用户定案**：当前使用**路线 1（Compaction）**——IndirectArgs 单命令连续区间，GS 顺序采样，纹理方案完全成立；**路线 2 预留给后续流式加载**（流式按区块粒度加载时天然需要分块连续区间）。

> **2026-08-15 定案（实现细节）**：
> - **紧凑目标载体 = A 紧凑纹理**（4 张 `RWTexture2D<float4>` 复用 64×64 容量 4096 ≥ 3767，**零重建**；CS-2 写前 N 纹素，GS 继续 `Load()` 路径与源位置纹理一致）。依据：GPU 效率——数据 256KB 全量常驻 L2、Load 与 Buffer 随机访问同路径，纹理方案成立（§连续区间策略论证）；**以实测 GPU 效率为准**，若性能不达预期再评估迁移 StructuredBuffer。
> - **aspectRatio 语义 = 宽高，非宽高比**（2026-08-15 用户定案）：字段直接存公告牌实际宽高 `[width, height]`（世界单位，组级参数——同组所有实例共享），转换器生成资产时写入实际尺寸，不再当比例乘缩放。
> - **包围盒推算（2026-08-15 用户定案）**：**CS 剔除端直接用宽高推算**（无需转换器写半径资产）：XCross 双交叉平面（XZ 各跨 W、Y 跨 H）取**统一球包围盒** `r = sqrt(2W² + H²) / 2`（覆盖两平面外接球；若轴对称 AABB 则半尺寸 `(W/2, H/2, W/2)`）。宽高为组级参数经 cbuffer 传入 CS，每实例按组统一半径。

---

---

## 方案对比

| 维度 | 手动放置（ECS） | 体积生成（CPU） | 噪声生成（GPU） |
|:-----|:--------------|:--------------|:--------------|
| 实例数 | 1~10 | 100~1000 | 1000~10000+ |
| ECS 实体 | 每个实例一个 | 每个实例一个 | 一个 Volume |
| 随机性 | N/A | 确定性种子 | 纹素采样 |
| 编辑友好 | 逐个编辑 | 改参数即可 | 改参数+纹理 |
| CPU 开销 | 低 | 中等（N 次 CreateEntity） | 低 |
| GPU 开销 | 低 | 低 | 中等（GS 发射） |
| 实现成本 | 低（已有组件/Builder） | 中（新增 Desc + 生成逻辑） | 高（GS 渲染器 + Shader） |

---

## 拓展：静态实体的通用噪声生成模式（待讨论，2026-08-26）

### 核心观察

公告牌、草、石头、灌木、装饰建筑等**不会移动的静态实体**，都可以用同一模式处理：
**不存 N 个实例 → 用噪声图/哈希在 GPU 端程序化生成**

| 存储方式 | CPU 内存 | JSON 大小 | 编辑灵活性 |
|:---------|:---------|:----------|:-----------|
| N 个 Transform | N × 64 字节 | N 行 | 逐个编辑 |
| 噪声图 + 区域 | 1 张纹理 | 1 组参数 | 改图/改参数 |

### 统一渲染管线

```
StaticVolumeComponent（ECS）
  ├─ noiseMap: 灰度纹理（密度分布，可手绘编辑）
  ├─ mesh: 要用什么网格（草片、石头模型、建筑）
  ├─ area: 范围
  ├─ density: 每平方米实例数
  ├─ randomSeed: 程序化变化
  └─ lod: 距离分段

StaticVolumeBuilder（PreRender）
  └─ 扫描 StaticVolumeComponent → StaticVolumeRenderItem

StaticVolumeRenderer（PostProcess）
  ├─ Compute Shader: 采样噪声图 → 生成 Indirect Args Buffer
  ├─ DrawIndexedInstancedIndirect: 一次提交，N 个实例
  └─ 无 CPU 逐实例数据，无需环形缓冲区上传
```

### 噪声图 vs 纯哈希

| | 噪声图（灰度） | 程序化 Hash |
|:--|:--------------|:------------|
| 宏观分布 | 可绘制（手绘密度） | 均匀随机 |
| 编辑 | PS 笔刷涂抹 | 改 seed |
| 运行时修改 | 可替换纹理 | 不可控 |
| 文件大小 | 1 张 DDS（可压缩） | 0 |
| 典型场景 | 森林、草地（需美术控制） | 碎石、装饰（纯随机） |

### 适用性

- **公告牌**：草、树叶、广告牌 ✅
- **静态网格**：石头、灌木、建筑 ✅
- **可移动实体**：角色、载具 ❌（需 Transform 更新）
- **物理交互**：可破坏物 ⚠️（需实例 ID 回读）

| 阶段 | 内容 | 优先级 |
|:-----|:------|:-------|
| 1 | 恢复 `BuildBillboard` + `BillboardRenderSystem`，接入手动放置 | 高（低风险） |
| 2 | `BillboardVolumeDesc` + CPU 生成 | 中 |
| 3 | GS 噪声渲染器 | 低（视需求） |

阶段 1 只需要：重新注册 `BuildBillboard` 系统 + 取消注释 `RegisterBillboardRenderSystem` + 验证 `BillboardDesc` JSON 解析即可。
