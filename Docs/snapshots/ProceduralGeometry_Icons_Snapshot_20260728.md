# 程序化几何体 + 大纲图标系统快照 (2026-07-28)

> 程序化天空盒几何体（异步加载管线）+ Outliner 图标系统 + 实体模板图标

---

## 最终架构

### 程序化几何体描述

JSON 中 `skybox.geometry` 支持两种形式：

**程序化生成（推荐）**——object 类型：
```json
"geometry": { "type": "cube" }
"geometry": { "type": "sphere", "radius": 1.0, "rings": 16, "segments": 16 }
```

**外部文件引用**——string 类型（向后兼容）：
```json
"geometry": "skybox_mesh"
```

数据结构在 `SceneDescription.h` 中的 `ProceduralGeometryDesc` 定义。

### 异步加载管线

```
SceneConstructor::OnDependenciesLoaded()
  └─ 检测 skybox.geometry 为 object
        ├─ GeometryProceduralTask::Create(type, params, ...)  → 返回 LoadTask
        ├─ BackgroundExecutor::SubmitLoadTask(task)
        │     ├─ cpuWork (后台线程):  GeometryGenerator → CPU MeshData → UPLOAD VB/IB
        │     ├─ gpuWork (后台线程):  DEFAULT VB/IB → 录制 COPY+DIRECT 命令
        │     │     └─ 返回 GpuWorkItem（含 UPLOAD 句柄，GPU 完成后释放）
        │     └─ onComplete (主线程): GeometryResourceManager::RegisterGeometry → SetSkybox
        └─ 原有文件路径保持不变（从 geoMap 查 handle）
```

### Outliner 图标系统

```
OutlinerPanel::Draw()
  ├─ 实体树：ImGui::Selectable（不可见 ID）→ 固定宽度图标 + 名称
  │     ├─ EntityIcon(registry, entity) 检查组件类型决定图标
  │     │     ├─ CameraComponent → \ue9f5（24gf-videoCamera）
  │     │     ├─ LightComponent (type) → \ue664/\ue65b/\ue66c（方向/点/聚光）
  │     │     ├─ WaterComponent → \ue602（水面）
  │     │     ├─ MeshComponent/OpaqueTag → \ue878（立方体）
  │     │     └─ 默认 → \ue79c（Node）
  │     └─ 图标固定 20px 区域，文本从 PAD_LEFT(10) + ICON_AREA_W(28) 处开始
  │
  └─ Create Entity 弹窗：
        ├─ 左栏分类：GetCategoryIcon(category) → 固定图标 + 名称
        │     ├─ General → \ue79c（Node）
        │     ├─ Lighting → \ue644（灯光）
        │     ├─ Primitives → \ueb36（几何体）
        │     └─ Rendering / Environment → 无图标（纯文本）
        └─ 右栏模板：entity_templates.json 中 icon_unicode 字段驱动
```

图标字体由 `Content/Fonts/iconfont.ttf` 提供，在 `DebugUIManager::MergeIconFont` 中合并到 ImGui 默认字体，字形范围 `[0xe600, 0xec17]`。

---

## ✅ 已完成

| 模块 | 内容 | 关键文件 |
|:-----|:-----|:---------|
| **ProceduralGeometryDesc** | `{}` 结构体 + `from_json`/`to_json` 序列化 | `SceneDescription.h` |
| **SkyboxDesc 扩展** | geometry 字段支持 string 或 object 两种解析 | `SceneDescription.h` |
| **GeometryProceduralTask** | 三段式异步加载（cpuWork/gpuWork/onComplete） | `Background/GeometryProceduralTask.h` |
| **SceneConstructor** | 检测 procedural type → 提交异步任务 → 回调设置天空盒 | `Scene/SceneConstructor.cpp` |
| **测试场景 JSON** | test_scene.json 改为 `{"type": "cube"}`，移除 `skybox_mesh` 依赖 | `Content/Scenes/test_scene.json` |
| **Outliner 图标** | 实体树根据组件显示图标（固定宽度对齐） | `Panels/OutlinerPanel.cpp` |
| **Create 弹窗图标** | 分类+模板列表显示图标 | `Panels/OutlinerPanel.cpp` |
| **实体模板 icon_unicode** | JSON 中每个模板指定图标 codepoint | `Editor/Config/entity_templates.json` |
| **iconfont 范围扩展** | 从 `[0xeb91,0xec17]` → `[0xe600,0xec17]` | `DebugUIManager.cpp` |
| **实体树 Selectable 对齐** | 固定 PAD_LEFT(10) + ICON_AREA_W(28)，文本一致性 | `Panels/OutlinerPanel.cpp` |

---

## ❌ 待完成 / 已知问题

| # | 问题 | 状态 |
|:-:|:-----|:------|
| 1 | **Mesh 模板未实现** | `CreateEntityFromTemplate` 中 mesh 分支被注释，创建后无法渲染 |
| 2 | **Billboard/Rendering/Environment 缺图标** | iconfont 中暂无对应 glyph，降级为纯文本 |
| 3 | **GeometryProceduralTask onComplete 回调与场景 onAllComplete 时序** | 独立 SubmitLoadTask 可能晚于场景其余资产加载完成，需确认 SceneConstructor 的时序逻辑是否受影响 |
| 4 | **Sphere 天空盒 Visual Test** | 几何体生成和上传已完成，但实际渲染效果需引擎运行后验证 |

---

## 关键文件清单

```
Engine/Asset/IO/Loader/SceneDescription.h
  ├─ struct ProceduralGeometryDesc          ← 新增
  └─ struct SkyboxDesc 扩展                  ← geometry 支持 object/string

Engine/Background/GeometryProceduralTask.h   ← 新增

Engine/Scene/SceneConstructor.cpp            ← 异步任务提交逻辑

Engine/DebugUI/DebugUIManager.cpp            ← iconRanges 扩展

Editor/EditorLib/Panels/OutlinerPanel.cpp    ← 图标渲染 + 固定宽度对齐

Editor/Config/entity_templates.json          ← 每个模板加 icon_unicode

Content/Scenes/test_scene.json               ← 天空盒改为 procedural cube

Content/Fonts/iconfont.*                     ← iconfont 图标字体
```
