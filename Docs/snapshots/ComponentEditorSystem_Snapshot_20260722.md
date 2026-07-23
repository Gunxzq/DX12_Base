# 组件编辑器系统实现快照 (2026-07-22)

> 基于 ECS 组件注册制的属性卡系统 + ImGuizmo 集成

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
        │     └─ DragFloat3 (Position/Rotation/Scale) + ImGuizmo 叠加
        │
        ├─ LightComponent → LightEditor::Draw()
        │     └─ Type/Color/Intensity/Range/Falloff/Shadow
        │
        └─ "添加组件" 弹出菜单（占位，待完善工厂方法）
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
  ├─ EditorLayout.cpp             ← ImGuizmo 绘制逻辑（DrawViewport）
  ├─ Engine/ThirdParty/imguizmo/  ← 5 个源文件
  │
DX12EditorLib (static lib)        ← 仅头文件引用（有 include path）
  └─ Editor.cpp                   ← 仅 #include "ImGuizmo.h"
  └─ EditorLayout.cpp             ← 使用 ImGuizmo::Manipulate 等

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
| **ImGuizmo 集成** | 视口叠加绘制，Camera View/Proj 矩阵，操作回写 Transform | `Core/EditorLayout.cpp` |
| **ImGuizmo 源码编译** | 替代 vcpkg 预编译库，匹配项目内 ImGui 1.92.9 WIP ABI | `Engine/ThirdParty/imguizmo/` (5 .cpp) |
| **ImGui API 兼容** | 修复 AddPolyline/AddRect 中 `true/false` → `ImDrawFlags_Closed/None` | `imguizmo/*.cpp` (4 文件) |

---

## 🔄 当前文件结构

```
Editor/EditorLib/
  ├─ Core/
  │   ├── Editor.h/.cpp               ← 注册编辑器 + 同步选中实体
  │   └── EditorLayout.h/.cpp         ← DrawProperties 遍历注册表 + ImGuizmo 叠加
  ├─ Properties/                       ← 新增
  │   ├── ComponentEditorRegistry.h    ← 注册器
  │   ├── ComponentEditorRegistrations.h ← 注册函数声明
  │   └── Editors/                     ← 各组件编辑实现
  │       ├── TransformEditor.cpp
  │       └── LightEditor.cpp

Engine/ThirdParty/
  └── imguizmo/                        ← 新增：ImGuizmo 源码
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
| 4 | **ImGuizmo 快捷键** | P2 | W/E/R 快捷键切换操作模式（当前仅菜单项） |
| 5 | **更多组件编辑器** | P2 | 注册 CameraComponent / MeshComponent / TerrainComponent 等编辑方法 |
| 6 | **预览面板独立** | P3 | 当前预览内嵌在 Properties 面板中，应独立为 Dock 区域 |

---

## 架构决策记录

| 决策 | 选项 | 选择 | 理由 |
|:-----|:-----|:-----|:------|
| ImGuizmo 集成方式 | vcpkg 预编译 vs 源码编译 | **源码编译** | 项目 ImGui 为 Docking 分支 1.92.9 WIP，vcpkg 预编译包 ABI 不匹配 |
| ImGuizmo 归属 | Engine Core vs Editor | **仅 Editor 可执行文件** | 编辑器专用，不污染引擎核心库 |
| 属性卡驱动 | 硬编码 vs 注册表 | **注册表** | 每个组件独立注册编辑方法，控制逻辑分散到各组件 |
| 选中实体传递 | EditorLayout 直接访问 Outliner vs Editor 同步 | **Editor 每帧同步** | 保持 Layout 职责单一，不直接依赖面板 |

---

## 相关文档

- `Docs/architecture/ComponentEditorSystem.md` — 架构设计文档
- `Docs/bugs/BugFix_ImGuizmo_ImGuiAPIVersionMismatch.md` — ImGuizmo 版本兼容记录
- `Docs/todos/remaining_issues.md` — 全局待办清单
- `Engine/ThirdParty/imguizmo/` — ImGuizmo 源码