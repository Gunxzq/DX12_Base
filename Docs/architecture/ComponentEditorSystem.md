# 组件驱动属性卡系统设计

> 基于 ECS 组件注册制的编辑器属性卡体系，替代当前硬编码的 Properties 面板

---

## 一、设计目标

1. **ECS 组件驱动** — 属性卡的内容由实体挂载的组件类型决定，而非硬编码
2. **外部注册机制** — 每个组件类型独立注册自己的编辑方法，控制逻辑分散到各组件
3. **ImGuizmo 集成** — 3D 变换操作使用 ImGuizmo 替代手动 DragFloat3
4. **可扩展** — 新增组件时只需注册编辑方法，无需修改属性卡框架
5. **预览面板独立** — 预览从属性卡中分离，作为独立 Dock 区域

---

## 二、整体架构

```
选中 Entity
  │
  ├─ 获取 Entity 的所有 Component
  │     ├─ 通过 Registry::type() 或遍历 component 类型列表
  │     │
  │     ├─ TransformComponent ──────→ ComponentEditorRegistry::Get<TransformComponent>()
  │     │                                └─ ImGuizmo::Manipulate()  ← 3D 操作
  │     │                                    └─ 回退到 DragFloat3（无 ImGuizmo 时）
  │     │
  │     ├─ LightComponent ──────────→ ComponentEditorRegistry::Get<LightComponent>()
  │     │                                └─ 光源类型、颜色、强度、衰减
  │     │
  │     ├─ CameraComponent ─────────→ ComponentEditorRegistry::Get<CameraComponent>()
  │     │                                └─ FOV、近远平面、投影类型
  │     │
  │     ├─ RenderMeshComponent ─────→ ComponentEditorRegistry::Get<RenderMeshComponent>()
  │     │                                └─ 网格资源、材质槽位
  │     │
  │     └─ ... 其他组件
  │
  └─ "添加组件" 按钮
        └─ 弹出已注册组件列表 → 附加到实体
```

### 分层职责

```
EditorLayout::DrawPropertiesPanel()     ← 调度层：遍历组件，调用注册器
  │
  ├─ ComponentEditorRegistry            ← 注册层：存储组件类型 → 编辑方法的映射
  │     ├─ Register<T>(name, drawFn)    ← 各模块在初始化时调用
  │     └─ Get<T>() → drawFn           ← 属性卡遍历时查询
  │
  └─ IComponentEditor<T>               ← 实现层：每个组件独立实现自己的 UI
        └─ Draw(component, entityId)
              └─ ImGui 控件 / ImGuizmo
```

---

## 三、核心接口设计

### 注册器

```cpp
// Editor/EditorLib/Properties/ComponentEditorRegistry.h

class ComponentEditorRegistry {
public:
    /// 组件编辑回调签名
    /// @param registry  ECS Registry（用于读写组件数据）
    /// @param entity    当前选中的实体 ID
    using EditorFn = std::function<void(ECS::Registry*, ECS::Entity)>;

    /// 注册组件类型的编辑方法
    /// @param typeName  组件显示名称（如 "Transform", "Light"）
    /// @param category  分组类别（用于折叠）
    /// @param drawFn    绘制回调
    template<typename T>
    static void Register(const char* typeName, const char* category, EditorFn drawFn);

    /// 获取组件的编辑方法
    static EditorFn* Get(ECS::ComponentType typeId);

    /// 获取所有已注册的组件类型列表（用于"添加组件"菜单）
    static const auto& GetAllRegistered();
};
```

### 使用示例

```cpp
// === Engine/Scene/Components/TransformComponent.cpp ===
// 在 Editor 模式下注册 Transform 编辑方法

void RegisterTransformEditor() {
    ComponentEditorRegistry::Register<TransformComponent>(
        "Transform", "Transform",
        [](ECS::Registry* registry, ECS::Entity entity) {
            auto& tc = registry->GetComponent<TransformComponent>(entity);

            // 方案 A：ImGuizmo 3D 操作（优先）
            if (ImGuizmo::IsOver()) {
                ImGuizmo::Manipulate(
                    viewMatrix, projMatrix,
                    ImGuizmo::OPERATION::TRANSLATE,
                    ImGuizmo::MODE::WORLD,
                    &tc.position.x
                );
            }

            // 方案 B：回退到数值输入
            ImGui::DragFloat3("Position", &tc.position.x, 0.1f);
            ImGui::DragFloat3("Rotation", &tc.rotation.x, 0.1f);
            ImGui::DragFloat3("Scale", &tc.scale.x, 0.1f);
        }
    );
}

// === Engine/Renderer/Light/LightComponent.cpp ===
void RegisterLightEditor() {
    ComponentEditorRegistry::Register<LightComponent>(
        "Light", "Lighting",
        [](ECS::Registry* registry, ECS::Entity entity) {
            auto& lc = registry->GetComponent<LightComponent>(entity);

            const char* types[] = {"Directional", "Point", "Spot"};
            ImGui::Combo("Type", (int*)&lc.type, types, 3);
            ImGui::ColorEdit3("Color", &lc.color.x);
            ImGui::SliderFloat("Intensity", &lc.intensity, 0.0f, 100.0f);
            if (lc.type != LightType::Directional) {
                ImGui::SliderFloat("Range", &lc.range, 0.1f, 100.0f);
            }
        }
    );
}
```

---

## 四、ImGuizmo 集成方案

### 交互流程

```
Viewport 鼠标点击
  │
  ├─ 选中实体（Raycast → 拾取）
  │
  ├─ Gizmo 操作模式切换（快捷键 / Toolbar）
  │     ├─ W → TRANSLATE
  │     ├─ E → ROTATE
  │     └─ R → SCALE
  │
  └─ ImGuizmo::Manipulate() 在 Viewport 渲染中绘制
        ├─ 需要 View/Proj 矩阵
        ├─ 操作结果写回 TransformComponent
        └─ 操作结束后触发 Undo/Redo 记录
```

### 关键集成点

| 集成点 | 说明 |
|:-------|:------|
| 矩阵输入 | ViewMatrix（CameraManager）+ ProjMatrix（视口参数） |
| 操作模式 | ImGuizmo::OPERATION + ImGuizmo::MODE |
| 回写时机 | ImGuizmo::IsUsing() 为 true 时每帧更新 TransformComponent |
| 撤销支持 | 操作开始前快照，操作结束后 Push Undo |

### Gizmo 渲染位置

应在 `EditorViewport` 渲染流程中，**叠加**在场景渲染之上：
- 场景渲染完成 → 切换到 UI 命令列表 → ImGuizmo::Draw()
- 不写入场景深度缓冲，始终在最上层

---

## 五、属性卡 UI 布局

```
┌─────────────────────────────────┐
│ Properties  [预览] [Lock] [Menu] │  ← 窗口标题栏
├─────────────────────────────────┤
│                                 │
│  ┌─ Entity Name ──────────────┐ │
│  │ [选中实体名称]              │ │  ← 顶部：实体名称 + 显隐/静态
│  └────────────────────────────┘ │
│                                 │
│  ┌─ Transform ──▼────────────┐ │  ← 可折叠分组
│  │  Position  [X] [Y] [Z]    │ │
│  │  Rotation  [X] [Y] [Z]    │ │
│  │  Scale     [X] [Y] [Z]    │ │
│  │  [Reset]                   │ │
│  └────────────────────────────┘ │
│                                 │
│  ┌─ Light ────▼──────────────┐ │  ← 组件分组，按 Category 折叠
│  │  Type     [Directional▼]  │ │
│  │  Color    [████████]      │ │
│  │  Intensity [====○====]    │ │  ← 0-100 Slider
│  │  [Remove Component]       │ │  ← 右键/按钮移除
│  └────────────────────────────┘ │
│                                 │
│  ┌─ Camera ────▼─────────────┐ │
│  │  FOV      [====○====]     │ │
│  │  Near     [0.1]           │ │
│  │  Far      [1000]          │ │
│  │  [Remove Component]       │ │
│  └────────────────────────────┘ │
│                                 │
│  [+ Add Component]              │  ← 弹出已注册组件列表
│                                 │
│  ┌──── Preview ────▼─────────┐ │  ← 独立区域，可选显示
│  │  [ 预览渲染图像 ]          │ │
│  │  [Orbit 控制]              │ │
│  └────────────────────────────┘ │
└─────────────────────────────────┘
```

### 组件分组折叠

- 按 `Category` 分组，默认展开 Transform，其余折叠
- 提供 `[Remove Component]` 按钮
- 组件列表可拖拽排序（后续）

---

## 六、与 Unity/Cocos 的对比

| 维度 | Unity | Cocos | 我们的方案 |
|:-----|:------|:------|:----------|
| UI 驱动 | 反射 CustomEditor | 装饰器 @inspector | **注册回调** Register<T>(drawFn) |
| 字段生成 | 自动反射 public | 自动 @property | **手动** 每个组件需写编辑回调 |
| 自定义覆盖 | PropertyDrawer | @inspector | 注册回调本身就是自定义 |
| 组件管理 | 扁平列表，可排序 | 扁平列表 | 扁平列表，按 Category 折叠 |
| Transform | 内置，不可移除 | 内置，不可移除 | 注册为普通组件，可替换 |
| Gizmo | 内建 | 内建 | **ImGuizmo** 集成 |
| 添加组件 | 搜索所有 MonoBehaviour | 搜索 Component | 遍历注册表 |
| 语言 | C# 反射 | TypeScript 装饰器 | C++ 模板注册 |

### 我们的优势

1. **无反射依赖** — 不需要 C++ 反射基础设施，Register<T> 模板即可
2. **ImGui 原生** — 所有控件都是 ImGui，自由度极高
3. **ImGuizmo 天然集成** — 3D 操纵器与 ImGui 同一体系
4. **组件编辑与渲染无关** — 编辑方法在编辑器进程，不增加运行时开销

### 我们的劣势

1. **手动编写编辑回调** — 每个组件需要手动写 ImGui 代码，Unity 反射自动生成 90% 的简单字段
2. **无自动类型推断** — 不能自动把 `float` 映射为 Slider，需要手写

---

## 七、实施计划

### 阶段 1：基础设施（P1）

| 步骤 | 内容 | 依赖 |
|:----:|:-----|:------|
| 1.1 | 创建 `ComponentEditorRegistry` 注册器 | 无 |
| 1.2 | 重构 `EditorLayout::DrawPropertiesPanel()` → 遍历注册表 | 1.1 |
| 1.3 | ImGuizmo 集成：Viewport 中绘制 Gizmo + 变换回写 | 无 |
| 1.4 | 注册 `TransformComponent` 编辑方法（ImGuizmo + 数值回退） | 1.1, 1.3 |
| 1.5 | 注册已有组件的编辑方法（Light、Camera 等） | 1.1 |

### 阶段 2：增强（P2）

| 步骤 | 内容 | 依赖 |
|:----:|:-----|:------|
| 2.1 | "添加组件" 弹出菜单 | 1.1 |
| 2.2 | 组件移除按钮 | 1.2 |
| 2.3 | Undo/Redo 集成（操作前快照） | 1.3, 1.4 |
| 2.4 | 组件可拖拽排序 | 1.2 |
| 2.5 | 预览面板独立为 Dock 区域 | — |

### 阶段 3：优化（P3）

| 步骤 | 内容 | 依赖 |
|:----:|:-----|:------|
| 3.1 | 多选实体编辑 | 1.2 |
| 3.2 | 组件引用拖拽（场景节点拾取） | 1.1 |
| 3.3 | 模板化自动字段 UI（可选，反射替代方案） | 1.1 |

---

## 八、文件结构变更

```
Editor/EditorLib/
  ├─ Core/
  │   └── EditorLayout.h/.cpp         ← 修改：DrawPropertiesPanel 改为遍历注册表
  ├─ Properties/                       ← 新增目录
  │   ├── ComponentEditorRegistry.h    ← 注册器
  │   ├── ComponentEditorRegistry.cpp
  │   ├── Editors/                     ← 各组件编辑实现
  │   │   ├── TransformEditor.cpp      ← ImGuizmo 集成
  │   │   ├── LightEditor.cpp
  │   │   ├── CameraEditor.cpp
  │   │   └── ...
  │   └── ImGuizmo/                    ← ImGuizmo 第三方库
  │       └── ImGuizmo.h/.cpp
  └── Viewport/
      └── EditorViewport.cpp           ← 修改：Gizmo 绘制叠加
```

---

## 九、相关文档

- `Docs/architecture/EditorPanelSystem.md` — 编辑器面板体系
- `Docs/architecture/Editor.md` — 编辑器架构总览
- `Docs/todos/remaining_issues.md` — 待办清单