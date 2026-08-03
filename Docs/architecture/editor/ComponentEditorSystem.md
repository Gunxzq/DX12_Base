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
  │     │                                └─ DragFloat3（位置/旋转/缩放数值输入）
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

                  ════════════════════════════════
                  EditorGizmoSystem（独立 System）
                    ├─ 在视口上方叠加 ImGuizmo::Manipulate()
                    ├─ 从 EditorViewportToolbar 获取操作模式（Translate/Rotate）
                    ├─ 从 CameraManager 获取 View/Proj 矩阵
                    └─ 操作结果写回 TransformComponent
                  ════════════════════════════════
```

### 分层职责

```
EditorLayout::DrawProperties()            ← 调度层：遍历组件，调用注册器
  │
  ├─ ComponentEditorRegistry              ← 注册层：存储组件类型 → 编辑方法的映射
  │     ├─ Register<T>(name, drawFn)      ← 各模块在初始化时调用
  │     └─ Get<T>() → drawFn             ← 属性卡遍历时查询
  │
  └─ IComponentEditor<T>                  ← 实现层：每个组件独立实现自己的 UI
        └─ Draw(component, entityId)      ← 纯 ImGui 控件（不含 ImGuizmo）

EditorGizmoSystem                         ← 独立 System：视口 3D 操纵器
  ├─ DrawGizmo()                          ← 注册为 EditorLayout 视口叠加回调
  │     └─ ImGuizmo::Manipulate()
  └─ 写回 TransformComponent              ← 在 ImGui 渲染阶段执行（Render Phase 之后）
```

> **设计决策**：ImGuizmo 作为独立 System 而非嵌入 TransformEditor，原因：
> 1. ImGuizmo 需要在**视口**中叠加绘制，而 TransformEditor 的回调在 Properties Panel 中执行
> 2. ImGuizmo 可能被其他用途复用（非变换操作），独立 System 更易扩展
> 3. 符合 EditorLayout 缩减原则——Layout 不持有任何具体功能模块的代码

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

## 四、ImGuizmo 集成方案（EditorGizmoSystem）

ImGuizmo 作为独立系统 `EditorGizmoSystem` 运行，不嵌入 EditorLayout 或 TransformEditor。

### 交互流程

```
Viewport 鼠标点击
  │
  ├─ 选中实体（Raycast → 拾取）
  │
  ├─ Gizmo 操作模式切换（快捷键 / Toolbar）
  │     ├─ W → TRANSLATE
  │     ├─ E → ROTATE
  │     └─ R → SCALE（后续）
  │
  └─ EditorGizmoSystem::DrawGizmo() 在视口叠加回调中执行
        ├─ 获取选中实体的 TransformComponent
        ├─ 获取 Camera View/Proj 矩阵
        ├─ 从 EditorViewportToolbar 获取操作模式
        ├─ ImGuizmo::SetRect() / SetDrawlist()
        ├─ ImGuizmo::Manipulate()
        └─ ImGuizmo::IsUsing() → 矩阵分解 → 写回 TransformComponent
```

### EditorGizmoSystem 接口

```cpp
// Editor/EditorLib/Viewport/Systems/EditorGizmoSystem.h

class EditorGizmoSystem {
public:
    void Initialize(DX12Engine::Boot::GameContext *context);
    void Shutdown();

    /// 设置 Gizmo 操作类型回调（从 EditorViewportToolbar 获取）
    void SetGetGizmoOpCallback(std::function<int()> cb);

    /// 设置选中实体查询回调
    void SetGetSelectedEntityCallback(std::function<DX12Engine::ECS::Entity()> cb);

    /// 在视口图像上叠加绘制 Gizmo（注册为 EditorLayout 的视口叠加回调）
    void DrawGizmo(ImVec2 viewportMin, ImVec2 viewportMax);

private:
    DX12Engine::Boot::GameContext *m_context = nullptr;
    std::function<int()> m_getGizmoOp;
    std::function<DX12Engine::ECS::Entity()> m_getSelectedEntity;
};
```

### 关键集成点

| 集成点 | 说明 |
|:-------|:------|
| 绘制位置 | 注册为 EditorLayout 的视口叠加回调，在 DrawViewport() 内的图像渲染之后调用 |
| 矩阵输入 | ViewMatrix（CameraManager）+ ProjMatrix（视口参数） |
| 操作模式 | 从 EditorViewportToolbar::GetCurrentGizmoOp() 获取 |
| 选中实体 | 通过回调从 EditorLayout/OutlinerPanel 获取 |
| 回写时机 | ImGuizmo::IsUsing() 为 true 时每帧更新 TransformComponent |
| 撤销支持 | 操作开始前快照，操作结束后 Push Undo（后续） |

### Gizmo 渲染位置

在视口图像渲染完成后，通过 EditorLayout 的视口叠加回调机制绘制：
- EditorLayout::DrawViewport() 渲染视口图像
- 保存图像位置到 m_viewportMin/Max
- 调用注册的视口叠加回调（包含 EditorGizmoSystem::DrawGizmo 和工具栏）
- ImGuizmo 不写入场景深度缓冲，始终在最上层

### 与 EditorLayout 的协作

```
EditorLayout::DrawViewport()
  ├─ 渲染视口图像（TabBar 回调 → ImGui::Image）
  ├─ 保存视口位置到 m_viewportMin/Max
  ├─ 调用视口叠加回调列表（通知各子系统视口位置）  ← 新增通用机制
  │     ├─ EditorViewportToolbar::DrawToolbar()    ← 工具栏（已有）
  │     └─ EditorGizmoSystem::DrawGizmo()          ← Gizmo（新增）
  └─ ImGui::End()
```

> EditorLayout 不感知 ImGuizmo，只提供通用的视口叠加回调注册机制。

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
| 1.2 | 重构 `EditorLayout::DrawProperties()` → 遍历注册表 | 1.1 |
| 1.3 | 创建 `EditorGizmoSystem`（独立 System，视口叠加 Gizmo + 变换回写） | 无 |
| 1.4 | 注册 `TransformComponent` 编辑方法（DragFloat3 数值输入） | 1.1 |
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
  │   └── EditorLayout.h/.cpp         ← 修改：DrawProperties 改为遍历注册表，移除 ImGuizmo 代码
  ├─ Properties/                       ← 新增目录
  │   ├── ComponentEditorRegistry.h    ← 注册器
  │   ├── ComponentEditorRegistrations.h ← 注册函数声明
  │   └── Editors/                     ← 各组件编辑实现
  │       ├── TransformEditor.cpp      ← DragFloat3 数值输入（不含 ImGuizmo）
  │       ├── LightEditor.cpp
  │       ├── CameraEditor.cpp
  │       └── ...
  └── Viewport/
      ├── EditorViewport.cpp           ← 视口渲染管理
      ├── EditorViewportToolbar.h/.cpp ← 工具栏 + 工具模式状态
      └── Systems/
          ├── EditorCameraSystem.h/.cpp ← 相机控制（已有）
          └── EditorGizmoSystem.h/.cpp  ← 新增：Gizmo 视口叠加 + 变换回写

Engine/ThirdParty/
  └── imguizmo/                        ← ImGuizmo 源码（仅编辑器 exe 编译）
```

---

## 九、相关文档

- `Docs/architecture/editor/EditorPanelSystem.md` — 编辑器面板体系
- `Docs/architecture/editor/Editor.md` — 编辑器架构总览
- `Docs/todos/remaining_issues.md` — 待办清单

---

## 十、已知缺陷与局限（2026-08-01 记录，暂不重构）

> 基于对已实现代码（`Properties/` 四个 Editor + `EditorLayout::DrawProperties` 消费端）的核查记录。
> 现状结论：**横向扩展性（新增组件类型）良好，纵向能力（字段级）受限**。以下按影响排序。

| # | 缺陷 | 影响 | 备注 |
|:-:|:-----|:-----|:-----|
| 1 | **声明式只到组件级，字段级仍是命令式**：每个 Editor 手写 ImGui 控件并直接写组件内存，无字段级拦截点 | Undo/Redo、多选编辑、复制粘贴、重置默认值等编辑器能力无法低成本接入（设计文档 P2 2.3 的"操作前快照"至今未实现） | 纵向优化的核心瓶颈；需字段描述符方案（member pointer + widget 声明）才能解锁 |
| 2 | **新增组件注册链三处手工环节**：新 Editor 文件 + `ComponentEditorRegistrations.h` 声明 + `Editor::Initialize()` 调用，三处缺一即组件在属性卡中静默缺失 | 漏注册不易察觉，靠人工保证 | 可收敛为宏/静态自注册，但显式调用更可调试，当前可接受 |
| 3 | **组件绘制顺序不可控**：`DrawProperties()` 直接遍历 `unordered_map<type_index, info>`，展示顺序由哈希决定、无显式排序 | 组件数量增多后展示顺序不可预测 | `category` 字段已声明但消费端**完全未使用**（无分组/排序），属预留元数据 |
| 4 | **每个 Editor 重复样板代码**：`CollapsingHeader` / `PushStyleVar` / `Indent` / `PushItemWidth` / `Unindent` 等约 10 行在各 Editor 间复制 | 样板噪音，新增 Editor 时复制粘贴易漏配对（Push/Pop 不平衡） | 可用 RAII 面板作用域对象收敛 |
| 5 | **自定义逻辑无"自定义段"钩子**：`TransformEditor` 的四元数↔欧拉角转换纯手写 | 若未来引入字段描述符方案，此类自定义逻辑无法用纯描述符表达，需保留 escape hatch（如 `CustomSection` 回调） | 设计描述符层时必须预留 |
| 6 | **组件编辑与序列化四端一致性无联动**：字段直接写组件，若组件结构变更，需手工同步 Editor + `LightDesc` to_json/from_json + `SceneLoader::Parse*` + `SceneConstructor`（规则 23） | 存在"属性卡可编辑但保存后丢失"的隐患（`BugFix_LightDesc_MissingFalloffFields` 同类问题） | 字段描述符方案的红利之一：元数据可与 Desc 序列化双向校验/自动生成 |

### 记录结论

- 当前"组件级声明式 + 字段级命令式"在组件数量少时够用，新增组件类型这条路是通的、干净的，**无需近期改动**。
- 优化空间的优先级建议（若未来重构）：**字段描述符方案（含自定义段钩子）> 样板代码收敛 > 绘制顺序确定性**。
- 重构触发条件参考：组件数量跨过 ~10 个，或需要多选编辑/Undo-Redo 时，再考虑上描述符层。

---

## 十一、Gizmo 绘制/计算分离重构（2026-08-01 已执行）

> 背景：`EditorGizmoSystem.cpp` 中四个辅助函数（`WorldToScreen` / `DrawAABBWireframe` / `ComputeFrustumCorners` / `DrawFrustumWireframe`）存在通用数学与引擎侧重复，且"绘制"与"计算"混杂。原则定为**绘制与计算分离**。

### 改动清单

| # | 文件 | 内容 |
|:-:|:-----|:-----|
| 1 | `Engine/Renderer/Scene/Struct/Frustum.h/.cpp` | `BuildFromCamera` 增加 `isOrtho` / `orthoSize` 参数（默认 false/0.0，保持现有调用兼容），正交投影下近/远平面尺寸恒定 = orthoSize |
| 2 | `Engine/Math/ScreenProjection.h`（新增） | `Math::ProjectToScreen(worldPos, viewProj, vpMinX, vpMinY, vpW, vpH)`：世界坐标 → 屏幕像素，无 ImGui 依赖，Editor/Engine 通用 |
| 3 | `Editor/EditorLib/Viewport/Systems/EditorGizmoSystem.cpp` | 删除 4 个旧辅助函数；新增统一 `DrawBoxWireframe(worldCorners[8], viewProj, ..., dl, connectColor, nearColor, farColor, thickness)`；AABB 分支与视锥体分支均改为"构造世界角点 → DrawBoxWireframe" |

### 关键设计决策

1. **计算层唯一化**：视锥体角点计算统一收敛到 `Frustum::BuildFromCamera`（补正交分支后覆盖透视+正交），Gizmo 不再持有自己的 `ComputeFrustumCorners`。
2. **绘制层保留 ImGui 依赖**：`DrawBoxWireframe` 仍留在 Gizmo（`ImDrawList` / `ImVec2` / `ImU32`），但其内部投影调用 `Math::ProjectToScreen`——计算归 Math，绘制归 Editor。
3. **角点布局约定统一**：`DrawBoxWireframe` 约定 8 角点布局"0-3 面 A / 4-7 面 B / i↔i+4 连接"，与 `Frustum::GetCorners()`（BL/BR/TL/TR 环序）天然一致，AABB 分支按同布局构造。
4. **行为保持**：视锥体三色（近亮/远暗/连接中间色）通过 `nearColor`/`farColor`/`connectColor` 参数保留，AABB 黄色线框传 0/0 复用 connectColor。
5. **射线检测链路未动**：`VisibleRaycaster`（ScreenToRay / RaycastOnSet / RaycastAll）不在本次改动范围，点击拾取 + 包围盒反馈行为不变。

### 后续可复用面（本次未做）

- `Math::ProjectToScreen` 的逆方向（屏幕→世界）仍由 `VisibleRaycaster::ScreenToRay` 内部持有 NDC 数学，未来可抽到 Math 层统一（`UnprojectScreen`），使世界↔屏幕投影成为单一归属。
- `DrawBoxWireframe` 目前仅 Gizmo 使用；若未来多实体线框（骨骼/路径/多选）出现，可提为独立 Overlay 模块。

---

## 十二、视锥体绘制修复（2026-08-01 已执行）

> 反馈：重构后相机可见区域（视锥体）绘制异常、蓝色过浅。定位到两个根因并修复。

### 根因 1：`Frustum::BuildFromCamera` up 重正交化方向错误（几何异常）

`Frustum.cpp` 中重正交化 up 的叉积方向写反：

```cpp
// 修复前：X × Z = -Y，up 翻转 → 角点上下颠倒、滚转丢失
upVec = XMVector3Normalize(XMVector3Cross(right, fwd));
// 修复后：Z × X = +Y，up 方向正确
upVec = XMVector3Normalize(XMVector3Cross(fwd, right));
```

- 影响：视锥体 8 角点 Y 分量翻转（上下颠倒），相机实体带滚转时视锥体与实体朝向不一致，视觉上"绘制异常"。
- 由于 `ComputePlanesFromCorners` 从角点重算平面并自动校正法线方向，**剔除/射线检测不受影响**（用户确认射线检测与包围盒正常）——这也是该 bug 未被早期发现的原因。
- 现有调用方（`CullingSystem`、`LightManager`）传入的 up 均已正交，重正交化仅为数值稳定，修复后输出不变或更正确。

### 根因 2：视锥体颜色过浅（视觉问题）

- 连接边 `connectColor` alpha 仅 120（半透明），叠加在视口图像上显得"蓝色太浅"。
- 修复：`nearColor (0,200,255,220) → (0,220,255,255)`、`farColor (0,150,200,160) → (0,150,210,190)`、`connectColor (0,180,220,120) → (0,190,235,200)`，线宽 1.5f → 2.0f（恢复重构前的近平面加粗观感）。

### 改动清单

| 文件 | 内容 |
|:-----|:-----|
| `Engine/Renderer/Scene/Struct/Frustum.cpp` | `upVec = XMVector3Cross(fwd, right)` 修正叉积方向 + 注释说明 |
| `Editor/EditorLib/Viewport/Systems/EditorGizmoSystem.cpp` | 视锥体三色饱和度/alpha 提高，线宽 2.0f |

### 经验教训

1. **标准叉积在左手系的约定**：`up × forward = +X` 依赖向量定义，但重正交化 up 时用 `forward × right`（Z × X = +Y）而非 `right × forward`（X × Z = -Y），两者差一个负号，肉眼不可见的角点翻转会直接体现为渲染方向错误。
2. **调试叠加层颜色 alpha 不宜过低**：视口图像之上叠加的线框，alpha < 150 会被背景淹没，建议连接边至少 180+。

---

## 十三、视锥体色相区分（2026-08-01 已执行）

> 反馈：视锥体远近裁剪面与锥角连接边应使用不同颜色，便于快速观察投影面。

### 方案

近/远裁剪面与锥角连接边改用**不同色相**（此前三者均为蓝色系，区分度不足）：

| 元素 | 颜色 | 说明 |
|:-----|:-----|:-----|
| 近裁剪面 | `(0, 230, 255, 255)` 亮青 | 投影面入口，最醒目 |
| 远裁剪面 | `(255, 170, 50, 220)` 橙黄 | 投影面出口，与近平面色相对立 |
| 锥角连接边 | `(190, 215, 255, 170)` 中性白 | 不干扰投影面识别 |

线宽保持 2.0f。改动位于 `EditorGizmoSystem.cpp` 视锥体分支的颜色常量，`DrawBoxWireframe` 的三色参数机制无需改动。

---

## 十四、相机 FOV 语义决策：单垂直 FOV + aspect 推导（2026-08-02 记录）

> 讨论：相机从垂直和水平看应存在不同夹角，但属性卡只显示一个 FOV——这是行业标准做法，非遗漏。

### 决策

**相机只有一个 FOV（垂直视野角），水平 FOV 由宽高比推导，不设独立参数。**

- 属性卡 `CameraEditor.cpp` 编辑的 FOV = 垂直视野角（度），与 `CameraManager::CalculateMatrices` 中 `XMMatrixPerspectiveFovLH(camera.FOV, camera.AspectRatio, ...)` 的 `fovAngleY` 完全一致。
- 垂直/水平 FOV 数学关联（非独立变量）：
  ```
  tan(hFov/2) = tan(vFov/2) × aspect
  ```
  给定垂直角 + aspect，水平角唯一确定；独立可设两者仅适用于变形镜头（anamorphic），游戏引擎基本不使用。

### 与大型引擎对齐

| 引擎 | FOV 语义 | 另一方向 |
|:-----|:---------|:---------|
| **Unity** | 垂直 FOV（`fieldOfView`） | aspect 推导 |
| **Unreal** | 水平 FOV（默认 90°） | aspect 推导 |
| **本项目** | 垂直 FOV（同 Unity） | aspect 推导 |

### 关键边界

- **aspect 来自运行时视口宽高比**（`CameraManager::OnResize` 更新），不是相机组件字段——因此相机属性卡中没有"宽高比"一项。
- 若未来对齐 Unreal 的"水平 FOV"操作习惯：仅需属性卡存水平角、`CalculateMatrices` 由 aspect 反推垂直角喂给 `PerspectiveFovLH`，不涉及结构变更。