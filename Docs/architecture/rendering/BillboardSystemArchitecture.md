# 公告牌系统架构设计

> 目标：替代硬编码的 `CreateBillboardTrees`，提供 JSON 驱动的公告牌渲染。
> 公告牌没有 MeshComponent，是纯 Sprite/GS 驱动的渲染类型。

---

## 概述

公告牌系统需要覆盖三种使用场景：

| 场景 | 实例数 | 使用方式 |
|:-----|:-------|:---------|
| 手动放置 | 1~10 | 场景设计师拖放（路标、装饰物） |
| 程序化体积 | 100~1000 | 树林/草丛区域，用参数生成 |
| GPU 噪声 | 1000~10000+ | 大规模植被，用 Geometry Shader 采样噪声图 |

三者不互斥——同一场景中三种可以共存。

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

## 拓展：静态实体的通用噪声生成模式

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
