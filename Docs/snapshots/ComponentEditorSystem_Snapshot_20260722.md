# 组件编辑器系统实现快照 (2026-07-23)

> 基于 ECS 组件注册制的属性卡系统 + EditorGizmoSystem 独立 Gizmo 系统

---

## 最终架构

### 组件驱动属性卡

```
EditorLayout::DrawProperties()
  │
  ├─ 无选中实体 → "No entity selected"
  │
  └─ 有选中实体 → 遍历 ComponentEditorRegistry::GetAll()
        │
        ├─ TransformComponent → TransformEditor::Draw()
        │     └─ DragFloat3 (Position/Rotation/Scale)  ← 纯数值输入，不含 ImGuizmo
        │
        ├─ LightComponent → LightEditor::Draw()
        │     └─ Type/Color/Intensity/Range/Falloff/Shadow
        │
        └─ "添加组件" 弹出菜单（占位，待完善工厂方法）

EditorGizmoSystem（独立 System）         ← 视口 3D 操纵器，不嵌入 EditorLayout
  ├─ DrawGizmo()                         ← 注册为 EditorLayout 视口叠加回调
  │     ├─ ImGuizmo::SetRect/SetDrawlist
  │     └─ ImGuizmo::Manipulate()
  └─ 操作结果写回 TransformComponent
```

### 注册机制

```cpp
// 各组件模块在 Editor::Initialize() 中注册
ComponentEditorRegistry::Register<TransformComponent>(
    "Transform", "Transform",
    [](ECS::Registry *registry, ECS::Entity entity) {
        auto *tc = registry->TryGetComponent<TransformComponent>(entity);
        if (!tc) return;
        ImGui::DragFloat3("Position", &tc->position.x, 0.1f);
        // ...
    }
);
```

### ImGuizmo 归属

```
DX12_Editor.exe                   ← ImGuizmo 源码编译（仅在此 exe 中）
  ├─ EditorGizmoSystem.cpp        ← ImGuizmo 绘制逻辑（DrawGizmo）
  ├─ Engine/ThirdParty/imguizmo/  ← 5 个源文件
  │
DX12EditorLib (static lib)        ← 不直接引用 ImGuizmo
  └─ EditorLayout.cpp             ← 仅提供通用视口叠加回调，不感知 ImGuizmo
  └─ Editor.cpp                   ← 创建 EditorGizmoSystem 并注册回调

Engine Core 库                    ← 完全不感知 ImGuizmo
  └─ DX12Core / DX12Renderer / ...
```

---

## ✅ 已完成

| 模块 | 内容 | 关键文件 |
|:-----|:-----|:---------|
| **注册器** | `ComponentEditorRegistry` 模板注册器，`Register<T>()` / `GetAll()` | `Properties/ComponentEditorRegistry.h` |
| **Transform 编辑** | 位置/旋转/缩放 DragFloat3，度↔弧度转换 | `Properties/Editors/TransformEditor.cpp` |
| **Light 编辑** | 类型 Combo、颜色、强度、范围、衰减、阴影参数 | `Properties/Editors/LightEditor.cpp` |
| **属性卡重构** | `DrawProperties()` 从硬编码改为遍历注册表 | `Core/EditorLayout.cpp` |
| **选中实体同步** | Editor 主循环每帧同步 Outliner 选中实体到 Layout | `Core/Editor.cpp` |
| **EditorGizmoSystem** | 独立 Gizmo 系统，从 EditorLayout 中抽离，视口叠加回调绘制 | `Viewport/Systems/EditorGizmoSystem.cpp` |
| **ImGuizmo 源码编译** | 替代 vcpkg 预编译库，匹配项目内 ImGui 1.92.9 WIP ABI | `Engine/ThirdParty/imguizmo/` (5 .cpp) |
| **ImGui API 兼容** | 修复 AddPolyline/AddRect 中 `true/false` → `ImDrawFlags_Closed/None` | `imguizmo/*.cpp` (4 文件) |
| **EditorLayout 清洁** | 移除所有 ImGuizmo 硬编码（m_showGizmo/m_gizmoOperation/SetGetGizmoOpCallback） | `Core/EditorLayout.h/.cpp` |

---

## 🔄 当前文件结构

```
Editor/EditorLib/
  ├─ Core/
  │   ├── Editor.h/.cpp               ← 持有 EditorGizmoSystem + 同步选中实体
  │   └── EditorLayout.h/.cpp         ← DrawProperties 遍历注册表 + 通用视口叠加回调
  ├─ Properties/                       ← 组件编辑器
  │   ├── ComponentEditorRegistry.h    ← 注册器
  │   ├── ComponentEditorRegistrations.h ← 注册函数声明
  │   └── Editors/                     ← 各组件编辑实现
  │       ├── TransformEditor.cpp      ← 纯数值输入（不含 ImGuizmo）
  │       └── LightEditor.cpp
  └── Viewport/
      ├── EditorViewportToolbar.h/.cpp ← 工具栏 + 工具模式状态
      └── Systems/
          ├── EditorCameraSystem.h/.cpp ← 相机控制
          └── EditorGizmoSystem.h/.cpp  ← 新增：Gizmo 视口叠加 + 变换回写

Engine/ThirdParty/
  └── imguizmo/                        ← ImGuizmo 源码
      ├── ImGuizmo.cpp/.h
      ├── ImSequencer.cpp/.h
      ├── ImCurveEdit.cpp/.h
      ├── ImGradient.cpp/.h
      ├── GraphEditor.cpp/.h
      └── ImZoomSlider.h
```

---

## ⏳ 待办

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 1 | **添加组件工厂** | P1 | 当前"添加组件"弹出菜单为空，需注册组件工厂方法（`CreateComponentFn`），通过 `type_index` → Emplace 组件到实体 |
| 2 | **组件移除** | P1 | 每个组件折叠面板尾部添加"移除组件"按钮 |
| 3 | **ImGuizmo 旋转回写** | P2 | 当前仅更新位置和缩放，旋转由 deltaMatrix 隐式处理，未完整写回 TransformComponent.rotation 欧拉角 |
| 4 | **ImGuizmo 快捷键** | P2 | W/E/R 快捷键切换操作模式（当前工具栏已支持，但 GizmoSystem 内无独立快捷键处理） |
| 5 | **更多组件编辑器** | P2 | 注册 CameraComponent / MeshComponent / TerrainComponent 等编辑方法 |
| 6 | **预览面板独立** | P3 | 当前预览内嵌在 Properties 面板中，应独立为 Dock 区域 |

---

## 架构决策记录

| 决策 | 选项 | 选择 | 理由 |
|:-----|:-----|:-----|:------|
| ImGuizmo 集成方式 | vcpkg 预编译 vs 源码编译 | **源码编译** | 项目 ImGui 为 Docking 分支 1.92.9 WIP，vcpkg 预编译包 ABI 不匹配 |
| ImGuizmo 归属 | EditorLayout vs 独立 System | **独立 EditorGizmoSystem** | EditorLayout 职责限定为布局分派，不应持有具体功能代码；Gizmo 可能被其他用途复用 |
| 属性卡驱动 | 硬编码 vs 注册表 | **注册表** | 每个组件独立注册编辑方法，控制逻辑分散到各组件 |
| 选中实体传递 | EditorLayout 直接访问 Outliner vs Editor 同步 | **Editor 每帧同步** | 保持 Layout 职责单一，不直接依赖面板 |
| 视口叠加机制 | 硬编码 ImGuizmo vs 通用叠加回调 | **通用 SetViewportOverlayCallback** | EditorLayout 不感知具体叠加内容，只提供回调插槽 |

---

## 相关文档

- `Docs/architecture/ComponentEditorSystem.md` — 架构设计文档
- `Docs/architecture/ViewportToolbar.md` — 工具栏与 Gizmo 集成设计
- `Docs/bugs/BugFix_ImGuizmo_ImGuiAPIVersionMismatch.md` — ImGuizmo 版本兼容记录
- `Docs/todos/remaining_issues.md` — 全局待办清单
- `Engine/ThirdParty/imguizmo/` — ImGuizmo 源码

---

## ✅ 2026-07-23 补充：序列化磁盘功能 + Rotation 格式统一

### Rotation 统一（欧拉角 → 四元数）

| 改动 | 文件 |
|:-----|:------|
| `TransformComponent::rotation`: `XMFLOAT3` → `XMFLOAT4{0,0,0,1}` | `Engine/ECS/Core/Components/Transform.h` |
| `GetMatrix()`: `XMMatrixRotationRollPitchYaw` → `XMMatrixRotationQuaternion` | 同上 |
| `ConstructEntity` 修复：读取四元数 4 元素 | `Engine/Scene/SceneConstructor.cpp` |
| Gizmo 回写：`XMStoreFloat4` 直接存四元数，零转换 | `Viewport/Systems/EditorGizmoSystem.cpp` |
| TransformEditor：四元数→欧拉角显示，编辑时转回 | `Properties/Editors/TransformEditor.cpp` |
| `UpdateEntityDesc`：旋转赋值改为 `XMFLOAT4` | `Scene/EditorSceneManager.cpp` |

### 序列化磁盘保存

| 改动 | 文件 |
|:-----|:------|
| 13 个 `*Desc` 结构体添加 `to_json` 操作符 | `Engine/Asset/IO/Loader/SceneDescription.h` |
| `MaterialParams/MaterialTextureSlots/MaterialDesc` 添加 `to_json` | `Engine/Asset/Definitions/Material/MaterialDesc.h` |
| `SceneLoader::SaveToJSON()` + `SceneLoader::SaveToFile()` | `Engine/Asset/IO/Loader/SceneLoader.h/.cpp` |
| `EditorSceneManager::SaveSceneAs()` 接入 `SceneLoader::SaveToFile` | `Editor/EditorLib/Scene/EditorSceneManager.cpp` |