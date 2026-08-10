# 渲染管线状态快照 (2026-08-04)

> 渲染管线规范落地 + 水渲染重构分阶段计划
> 关联：`Docs/architecture/rendering/RenderPipelineSpecification.md`（新建规范文档）、
> `Docs/todos/archived/remaining_issues.md §三`（水渲染重构分阶段计划）、
> `.atomcode.md`（第 24 条索引）
> 待办：水渲染重构 6 阶段（§三）

---

## 一、已落地：渲染管线规范

### 1.1 新建文档

| 文档 | 内容 |
|:--|:--|
| `Docs/architecture/rendering/RenderPipelineSpecification.md` | 5 层管线模型、材质槽机制、水实体 JSON 表达、扩展步骤 5 步、铁律约束、5 条已知缺陷 |
| `.atomcode.md` 第 24 条 | 索引指向新规范文档，不展开正文 |

### 1.2 材料槽机制现状

- **ECS 组件**：`MeshComponent`（lodMeshHandle）+ `RenderSlotComponent`（slots[]: {material, subMeshRanges, shaderType}）+ `TransformComponent`（cullDistance）
- **图案分桶**：`RenderSlotCache::Rebuild`（变更驱动）+ `DispatchAll`（每帧分桶）+ `ForEachBucket(rendererName)`（Builder 查询）
- **桶消费**：`OpaqueRenderItemBuilder` / `SkinnedRenderItemBuilder` 已走桶模式
- **材质路由**：`.mat` 的 `shader` 字段 → `ParseShaderType` → `ShaderType` → `FindMaterialRoute` → `rendererName`

### 1.3 水实体 JSON 表达（已定案）

```json
{
  "name": "Sea_WaterBody",
  "components": {
    "transform": { "position": [1500, 0, -1530], "cullDistance": 5000 },
    "mesh": {
      "procedural": { "type": "grid", "width": 840, "depth": 780, "widthSegments": 32, "depthSegments": 32 },
      "materials": ["Water"]
    },
    "water": { "amplitude": 0.5, "frequency": 1.0, "speed": 0.5, "direction": 0.0 }
  }
}
```

---

## 二、已知缺陷（未修复）

| # | 缺陷 | 优先级 | 影响 | 修复阶段 |
|:--|:--|:--:|:--|:--:|
| #1 | Editor 水渲染误录 Opaque 阶段（`Editor.cpp:1155-1190`） | P1 | 透明物体被当不透明画进 G-buffer | 阶段 3c |
| #2 | Game 透明队列无消费系统（`m_transparentQueue` 无人消费） | P1 | 透明实体（玻璃等）不可见 | 阶段 4 |
| #3 | WaterRenderItemBuilder 未迁移桶模式（用 ECS view 替代 `ForEachBucket`） | P1 | 水实体额外挂 TransparentTag，材质槽 shaderType=Water 被忽略 | 阶段 3a |
| #4 | ProceduralGeometryDesc 未接入 SceneConstructor（`MeshDesc.procedural` 未处理） | P1 | 水实体 JSON 的 `mesh.procedural` 无法被加载 | 阶段 2 |
| #5 | Game/Editor 渲染阶段不一致 | P2 | 相同场景两端渲染结果不同 | 阶段 3b/3c |

---

## 三、水渲染重构分阶段计划

| 阶段 | 内容 | 优先级 | 前置依赖 |
|:--:|:--|:--:|:--|
| 0 | 清理旧水渲染 System（删除旧代码） | 最高 | 无 |
| 1 | MPD .scene 二进制 → 水实体构建（SceneLoader + SceneConstructor） | 高 | 阶段 0 |
| 2 | 程序化网格 GeometryProceduralTask（grid 生成 + 注册 GeometryHandle） | 高 | 阶段 1 |
| 3 | 水渲染器桶模式迁移 + 重新实现（Builder + WaterRenderSystem） | 高 | 阶段 2 |
| 4 | 透明实体渲染系统实现（TransparentRenderer） | 中 | 阶段 3 |
| 5 | 清理（删除 TransparentTag 等标记组件） | 低 | 阶段 4 |

**分阶段原则**：每个阶段完成后人工编译验证，通过后再进入下一阶段。

---

## 四、架构图（数据流）

```
┌─────────────────────────────────────────────────────────────────────────┐
│ 第 1 层：ECS 组件                                                       │
│  MeshComponent + RenderSlotComponent + TransformComponent (+ WaterComp) │
└──────────────────────┬──────────────────────────────────────────────────┘
                       │ RenderSlotCache::Rebuild（变更驱动）
                       ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 第 2 层：缓存表 + 桶                                                    │
│  m_slotTable[entity] = slots[] → DispatchAll → b桶[ShaderType] = Entry│
└──────────────────────┬──────────────────────────────────────────────────┘
                       │ ForEachBucket(rendererName)
                       ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 第 3 层：构建器按桶处理                                                  │
│  OpaqueBuilder / SkinnedBuilder / [WaterBuilder] / [TransparentBuilder] │
│  → 精确筛选 + LOD → BatchKey 分组 → 自包含 RenderItem                   │
└──────────────────────┬──────────────────────────────────────────────────┘
                       │ RenderItem 队列
                       ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 第 4 层：渲染器 + RenderSystem                                          │
│  OpaqueRenderSystem / [WaterRenderSystem] / [TransparentRenderSystem]   │
│  → 录制命令列表 → SubmitRenderCommand(phase)                            │
└──────────────────────┬──────────────────────────────────────────────────┘
                       │ 命令列表
                       ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 第 5 层：FrameDriver 阶段执行                                            │
│  PrePass → Opaque → DynamicAOcclusion → Lighting → Billboard            │
│  → Transparent → PostProcess → FSR3_Upscale → UI                       │
└─────────────────────────────────────────────────────────────────────────┘
```

方括号 `[ ]` 表示待实现（桶模式迁移后）。

---

## 五、相关文档

- `Docs/architecture/rendering/RenderPipelineSpecification.md` — 渲染管线规范（新建）
- `Docs/architecture/rendering/SubMeshMaterialSlots.md` — 材质槽机制设计
- `Docs/architecture/rendering/WaterSystemArchitecture.md` — 水渲染架构（需更新阶段：PostProcess → Transparent）
- `Docs/architecture/rendering/RendererDataDriven.md` — 渲染器数据驱动架构
- `Docs/todos/archived/remaining_issues.md §三` — 水渲染重构分阶段计划
- `.atomcode.md` 第 24 条 — 渲染管线规范索引