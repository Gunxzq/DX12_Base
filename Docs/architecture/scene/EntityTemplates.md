# 实体模板（Entity Templates）

> Outliner 右键菜单 → "Create Entity" 弹窗中使用的实体模板系统。
> JSON 驱动，无需修改 C++ 代码即可扩展模板类型。

---

## 一、文件位置

``` 
Editor/Config/entity_templates.json    ← 模板定义
OutlinerPanel.cpp                      ← 运行时加载与解析
```

## 二、JSON 结构

```json
{
    "version": 1,
    "templates": [
        {
            "name": "Camera",
            "category": "Rendering",
            "description": "透视或正交相机，定义场景的观察视角",
            "icon": "camera",
            "components": {
                "transform": { "position": [0, 5, -10], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1] },
                "camera": { "fov": 60, "nearPlane": 0.1, "farPlane": 1000.0, "projection": "perspective", "isMain": false }
            }
        }
    ]
}
```

### 顶层字段

| 字段 | 类型 | 必需 | 说明 |
|:-----|:------|:------|:------|
| `version` | uint32 | ✅ | 格式版本（当前 1） |
| `templates` | array | ✅ | 模板列表 |

### 模板条目字段

| 字段 | 类型 | 必需 | 说明 |
|:-----|:------|:------|:------|
| `name` | string | ✅ | 模板名称，显示在弹窗列表中。创建实体时也作为实体名 |
| `category` | string | ✅ | 分类名称（弹窗左栏分组依据）。同名字符串归为一组 |
| `description` | string | ❌ | 描述文字，鼠标悬浮时显示 tooltip |
| `icon` | string | ❌ | 图标标识（预留，当前未使用） |
| `components` | object | ✅ | ECS 组件配置（见 §三） |

---

## 三、组件映射

`components` 对象中的每个 key 对应一种 ECS 组件，value 为该组件的初始化参数。

### 支持的组件

| JSON key | ECS 组件 | 说明 |
|:---------|:---------|:------|
| `transform` | `TransformComponent` | 位置、旋转（四元数）、缩放 |
| `camera` | `CameraComponent` | FOV、裁剪面、投影类型、主相机标记 |
| `light` | `LightComponent` | 光源类型、颜色+强度（RGBA）、范围、阴影 |
| `water` | `WaterComponent` | 波浪参数（amplitude/frequency/speed/direction） |
| `mesh` | (暂未实现) | 网格 + 材质引用，当前仅占位 |
| `opaque` | `OpaqueTag` | 标记为不透明（值为 null） |
| `transparent` | `TransparentTag` | 标记为透明（值为 null） |

### transform

```json
"transform": {
    "position": [x, y, z],
    "rotation": [x, y, z, w],       // 四元数（单位长度）
    "scale": [x, y, z]
}
```

### camera

```json
"camera": {
    "fov": 60,
    "orthoSize": 10.0,
    "nearPlane": 0.1,
    "farPlane": 1000.0,
    "projection": "perspective",      // "perspective" | "orthographic"
    "isMain": false
}
```

### light

```json
"light": {
    "type": "directional",            // "directional" | "point" | "spot"
    "color": [1.0, 1.0, 1.0],         // RGB（[0,1]），强度由 alpha 通道控制
    "intensity": 1.0,                  // 强度乘数（shader 中 color.rgb * intensity 作为辐射度）
    "range": 10.0,                     // 点/聚光灯的有效范围
    "castsShadow": 1.0                 // 0=不投射阴影, 1=投射
}
```

> 注意：`color` 数组长度任意（建议 3 分量），`intensity` 为独立字段。`editor.cpp` 的 Immediate 回调中 `color.rgb * intensity` 合并为渲染时的辐射度（radiance），着色器仅读取 `Strength.xyz`。

### water

```json
"water": {
    "amplitude": 0.5,
    "frequency": 1.0,
    "speed": 1.0,
    "direction": [1.0, 0.0, 0.0]      // 风向（向量）
}
```

### 标记组件

```json
"opaque": null,
"transparent": null
```

值固定为 `null`，存在即表示添加对应标签。

---

## 四、运行时行为

### 创建流程

```
用户双击弹窗中的模板条目
  ↓
OutlinerPanel::CreateEntityFromTemplate(index)
  ├─ sceneMgr->CreateEntity()                          ← 分配 entt::entity
  ├─ 添加 NameComponent（name 来自模板、persistentId 自增）
  ├─ 遍历 components 对象，逐个添加 ECS 组件
  │     ├─ transform → TransformComponent
  │     ├─ camera   → CameraComponent
  │     ├─ light    → LightComponent
  │     ├─ water    → WaterComponent（波浪参数写入组件字段）
  │     ├─ mesh     → （暂未实现）
  │     ├─ opaque   → OpaqueTag
  │     └─ transparent → TransparentTag
  ├─ 添加 SceneTagComponent（当前活跃 sceneId）
  └─ 选中新创建的实体
```

### 重要约束

- **创建的实体属于当前活跃 Tab**（通过 SceneTagComponent.sceneId 标记），Builder 按 sceneId 过滤渲染
- **不暴露原子 ECS 组件给用户**——用户操作的是模板组合结果（Camera = Transform + CameraComponent 等）
- **Mesh/Water 等需要额外资源注册的组件**——模板仅创建 ECS 组件标记，实际的 GPU 资源注册（如 GeometryResourceManager::Retain、MaterialManager 等）需要后续由用户通过拖拽 AssetBrowser 资源完成

---

## 五、已知问题

| # | 问题 | 状态 |
|:-:|:-----|:------|
| 1 | **Water 模板波浪参数写入组件**：`water` 字段中的 amplitude/frequency/speed/direction 已由 `CreateEntityFromTemplate` 正确写入 `WaterComponent`，但需检查 WaterBuilder 是否能正确读取 | ⚠️ 待验证 |
| 2 | **Mesh 模板未实现**：`CreateEntityFromTemplate` 中 mesh 分支被注释，创建后无法渲染 | ❌ |
| 3 | **缺少默认网格/材质占位**：Mesh 模板需在创建时提供占位（如默认 cube + 默认材质）或从 AssetBrowser 拖拽 | ❌ |

---

## 六、扩展指南

添加新模板类型的步骤：

1. **定义模板 JSON**：在 `Editor/Config/entity_templates.json` 中添加新的 `templates[]` 条目
2. **实现组件创建**：在 `OutlinerPanel::CreateEntityFromTemplate()` 中新增 `if (key == "xxx")` 分支
3. **更新语言包**（如需要）：在 `editor_strings_*.json` 中添加弹窗相关的显示文本
4. **更新文档**：更新此文档的 §三 组件映射表
