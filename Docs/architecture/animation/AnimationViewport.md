# 动画视口设计（AnimationViewport）

> 状态：📋 设计定案（2026-08-01，大型引擎模式）
> 关联：`Editor.md` §2.5（多堆隔离）、`AssetPreviewSystem.md`（LivePreviewProvider / AnimationPreviewState）、`EditorPanelSystem.md`（IEditorPanel 面板体系）、`AnimationAsset.md`（.anim 资产 + AnimationManager）、`SkinnedAnimation.md`（动画 System，Game 端）
> 定位：**独立于主场景的资产级动画查看器**——仿大型引擎（Unreal Persona / Unity Animation 窗口）的"编辑器 ≠ 游戏"分层：动画视口是编辑器工具，不经主场景 ECS；动画系统（ECS 驱动）留给 Game 端，长期不着急。

---

## 〇、设计定案（2026-08-01 用户确认）

### 0.1 大型引擎的参考模式

| | Unreal（Persona） | Unity | Godot |
|:--|:--|:--|:--|
| 动画运行时 | SkeletalMeshComponent + AnimInstance（组件驱动） | Animator + AnimatorController（组件） | AnimationPlayer + Skeleton3D（节点） |
| 动画编辑器 | 独立预览世界（Preview Scene），**不跑游戏逻辑** | Animation/Animator 窗口，Inspector 底部预览 | 独立 Animation 编辑器 |
| 编辑器的"大纲" | **资产级大纲**：骨骼树 / 插槽 / 动画资源列表 | Project 面板资源 | 场景树中的动画节点 |
| 编辑器的"属性" | **资产属性**：骨骼 Transform、动画曲线、Notify | 资产 Inspector | 属性面板 |

**核心洞察**：大型引擎的动画编辑器是**独立于游戏场景的资产工具**——有自己的预览世界、资产级大纲（非场景实体）、资产属性（非场景组件）。游戏运行时的动画系统是 ECS/组件驱动的另一条链路。两者共享资产数据结构，但**编辑器不依赖游戏运行**。

### 0.2 本项目分层

```
┌─ 动画视口（编辑器，现在做）────────────────┐
│  独立预览上下文（自包含面板，不经主场景 ECS） │
│  ├─ 加载 .dxmesh/.bone/.anim（资产）        │
│  ├─ 面板内"大纲"：骨骼树 + 剪辑列表          │
│  ├─ 面板内"属性"：选中骨骼/剪辑的参数         │
│  ├─ 轨道相机 + 播放控制 + 骨骼线框           │
│  └─ EditorViewport 堆隔离，不污染主场景      │
└─────────────────────────────────────────┘

┌─ 动画系统（Game 端，长期不着急）───────────┐
│  ECS 组件驱动（Unreal AnimInstance 对应物）  │
│  ├─ SkinnedComponent（已有）                │
│  ├─ AnimationComponent（播放状态机，待定）    │
│  └─ AnimationAdvancer（AlwaysRun 推进）      │
└─────────────────────────────────────────┘
```

**共享桥梁**：`SkinnedComponent` 数据结构两份用——视口"值持有"做预览，运行时"组件持有"做驱动。`AnimLoader`/`AnimationManager`/`ClipHandle` 已实现，两者消费同一套资产链路。

### 0.3 决策理由

1. **编辑器解耦 Game**：Game 端长期不着急（Unreal Persona 在编辑器进程内独立渲染，Game 未修好不影响动画工具开发）
2. **不违背既有定案**：角色不进主 viewport（独立预览 RT）、不改 `SceneConstructor`（不往场景塞实体）
3. **复用价值**：`ComponentEditorRegistry` 风格的属性控件（Combo/Slider/Checkbox）在视口内直接画；将来 NPC 场景化（P2）时注册为正式组件编辑器，一次写好两处复用
4. **骨骼调试是核心价值**：骨骼树大纲 + 骨骼线框是 Persona 的精髓，本质是"资产视角"而非"场景视角"

---

## 一、目标与边界

| 项 | 说明 |
|:---|:-----|
| 目标 | 在编辑器中查看角色网格 + 骨架 + 动画剪辑播放效果（播放/调帧/循环/速度）+ 骨骼调试 |
| 不进主视口 | 角色是资产预览对象，不是场景实体；主 viewport 只显示场景内容 |
| 不经主场景 ECS | 视口自包含：值持有 `SkinnedComponent` 数据，不挂实体、不污染 Outliner |
| 不改生成器 | `SceneConstructor`/场景生成器不因角色预览而修改（NPC 场景化走 .character，P2） |
| 引擎角色 | 引擎提供：蒙皮渲染 + 动画采样 + 骨骼矩阵输出，不做动画编辑/关键帧操作 |
| 数据源 | 外部资产：`.dxmesh` + `.bone` + `.anim`（Blender 生产 → AssetTool 转换） |

---

## 二、面板体系（遵循 EditorPanelSystem.md）

实现为 `IEditorPanel` 子类 `AnimationViewportPanel`：

| 接口 | 实现 |
|:-----|:-----|
| `GetWindowName()` | `"AnimationViewport###AnimationViewport"`（含 `###ID`，与 Dock 布局一致） |
| `Draw(float dt)` | 见 §四 布局 |
| `IsVisible()/SetVisible()` | 面板自行管理显隐标志 |
| `GetTargetDockId()` | `EditorLayout::InitializeDockLayout()` 中定义的 Dock ID |

**注册**：`Editor::Initialize()` 持有面板对象 → `m_layout->RegisterPanel(&panel)`。

**语言包**：窗口标题用 `EditorStrings::Get("animation_viewport.title", "Animation Viewport")`，同步更新 `editor_strings_*.json`（en-US / zh-CN）。

---

## 三、渲染上下文（EditorViewport 堆隔离）

复用 `Editor.md` §2.5 的隔离模式——动画预览是**自包含渲染上下文**：

```
AnimationViewportPanel
  ├─ HeapTag::EditorViewport 堆（CBV_SRV_UAV / Texture / RTV / DSV 分区）
  ├─ 独立离屏 RT + DSV（深度格式 = 主交换链深度格式，见项目约束 16）
  ├─ 独立命令列表（每帧录制，含对称 ResourceBarrier，见项目约束 10）
  └─ 渲染结果 RT 的 SRV → ImGui::Image 显示
```

**隔离收益**：骨骼缓冲/蒙皮渲染计算异常不波及主场景 `Default` 堆。

### 渲染依赖（复用现有渲染器，不新建 Pass）

| 渲染项 | 复用 |
|:-----|:-----|
| 蒙皮网格 | `SkinnedRenderer`（GBuffer PSO + 骨骼 SRV）或专用预览 PSO（简单光照 + 骨骼线框叠加） |
| 骨骼矩阵 | 面板内联采样 `SkeletonData`/`AnimationClip` → 骨骼缓冲（`SkinnedRenderItem.boneBufferAddress`） |
| 骨骼线框 | 新增轻量绘制：`BoneDebugRenderer`（Debug line list，骨骼原点 + 父子连线） |

---

## 四、面板布局（Draw）——资产级大纲模式

```
Draw(float deltaTime):
  早期返回（不可见 / 资产未就绪）
  ├─ 消费外部数据（当前预览目标：mesh/bone/anim 句柄）
  ├─ ImGui::Begin(GetWindowName(), &m_visible)
  │    ├─ 工具栏：
  │    │    ├─ 资产选择：Combo（已加载 .dxmesh+.bone 候选）→ 自动带出剪辑列表
  │    │    └─ 剪辑选择：Combo（.anim 剪辑列表，来自 AnimationManager / 目录扫描）
  │    ├─ 分隔线
  │    ├─ 左侧栏（大纲，宽度 ~180px）：
  │    │    ├─ 骨骼树：按 BoneHierarchy 层级展开（BoneNames），选中高亮
  │    │    └─ 剪辑列表：所有 .anim（ClipHandle 表），选中即切换播放
  │    ├─ 中央：ImGui::Image(预览 RT 的 ImTextureID)（轨道相机：拖拽旋转/滚轮缩放）
  │    ├─ 右侧栏（属性，宽度 ~220px）：
  │    │    ├─ 选中骨骼：TRS 数值（只读调试）/ 层级信息（父/子索引）
  │    │    └─ 当前剪辑：duration / fps / loop / 键帧数
  │    ├─ 底部（播放控制条）：
  │    │    ├─ Play / Pause / Stop 按钮 + Seek 滑条 + Speed 滑块 + Loop 勾选
  │    │    └─ 调试开关：Show Bones（骨骼线框）、Show Grid、Show UV
  │    └─ 信息栏：当前帧 / 总时长 / 骨骼数 / 顶点数
  └─ ImGui::End()
```

**控件 ID 约束**（项目约束 22）：所有 `Combo`/`SliderFloat`/`Checkbox` 等控件 ID 用硬编码 `##` 前缀（如 `"##AnimClip"`、`"##PlaybackSpeed"`、`"##BoneTree"`），不用语言包。

---

## 五、播放控制与数据流

### 5.1 预览状态（值持有，非 ECS 组件）

```cpp
struct AnimationViewportState {
    // 预览目标（资产句柄）
    Resource::GeometryHandle mesh;       // .dxmesh
    Resource::SkeletonHandle skeleton;   // .bone
    Resource::ClipHandle clip;           // .anim（AnimationManager）

    // 播放控制（对应 AssetPreviewSystem.md 的 AnimationPreviewState）
    float timePosition = 0.0f;
    float playSpeed = 1.0f;
    bool isPlaying = true;
    bool loop = true;

    // 大纲选中（资产级，非实体）
    int selectedBoneIndex = -1;
};
```

### 5.2 每帧流程

```
面板 Draw
  ├─ 若 isPlaying: timePosition += dt × playSpeed（loop 时回绕到 duration）
  ├─ Seek 滑条: 写 timePosition（暂停推进，实现"调帧"）
  ├─ 面板内联采样（不经 ECS，直接读 SkeletonData + AnimationClip）:
  │     SkeletonManager::GetSkeleton(skeleton) → SkeletonData
  │     AnimationManager::GetClip(clip) → AnimationClip
  │     采样 AnimationClip → 与骨架层级/偏移矩阵合成最终矩阵（复用 GetFinalTransforms 逻辑）
  │       → 上传预览骨骼缓冲 → boneBufferAddress
  ├─ SkinnedRenderer 绘制蒙皮网格（骨骼缓冲 SRV）
  ├─ BoneDebugRenderer 绘制骨骼线框（复用同一矩阵数组）
  └─ RT SRV → ImGui::Image
```

> **采样实现注意**：现有 `SkeletonData::GetFinalTransforms(clipName, t)` 从骨架内置 `Animations` 取剪辑（M3D 遗留）。.anim 原子化后剪辑独立于骨架（AnimationManager 持有），视口需按 `AnimationAsset.md` §5.3 的改造方向：从 ClipHandle 采样 → 复用骨架层级/偏移合成。可在视口内先做一个小工具函数，将来 AnimationSystem 复用同逻辑。

### 5.3 视角控制

- 独立轨道相机（OrbitCamera）：拖拽旋转、滚轮缩放、中键平移；
- 视角变换仅作用于该面板的渲染（不共享主相机）。

---

## 六、优先级与实施（并入 AnimationAsset.md §七待办）

| # | 任务 | 优先级 | 依赖 |
|:-:|:-----|:-------|:-----|
| 1 | `AnimationViewportPanel` 骨架（窗口 + 轨道相机 + 空 RT 显示） | P1 | 面板体系就绪 |
| 2 | 资产加载接入（选择 .dxmesh/.bone/.anim 并显示网格静态帧） | P1 | AnimationManager（✅ 已实现）+ GeometryManager |
| 3 | 播放控制（Play/Pause/Seek/Speed/Loop） | P1 | #2 |
| 4 | 骨骼树大纲 + 选中骨骼属性（资产级） | P1 | #2（BoneNames 已就绪） |
| 5 | 骨骼线框调试渲染（BoneDebugRenderer） | P2 | #3 |
| 6 | `.character` 资产快捷加载（选一个角色资产即载入全套） | P2 | .character 资产就绪（P2） |

> **范围约束**：视口只负责**查看**，不提供动画编辑（关键帧操作、曲线编辑）——那是 Blender 的职责。

---

## 七、与 Game 端动画系统的关系（长期）

| | 动画视口（现在） | 动画系统（Game 端，长期不着急） |
|:--|:--|:--|
| 数据持有 | 值持有（AnimationViewportState） | ECS 组件（SkinnedComponent + AnimationComponent） |
| 采样驱动 | 面板内联（视口 Draw） | AnimationAdvancer（AlwaysRun System） |
| 渲染 | SkinnedRenderer（复用） | SkinnedRenderer（复用） |
| 共享 | AnimLoader / AnimationManager / ClipHandle / SkeletonData | 同左 |
| 接合点 | 视口可先在属性面板注册 SkinnedComponent 编辑器（复用） | NPC 场景化（P2）时注册正式组件编辑器 |

---

## 八、相关文档

- `Editor.md` §2.5 — 多堆隔离（EditorViewport 堆）
- `EditorPanelSystem.md` — IEditorPanel 面板体系
- `ComponentEditorSystem.md` — 属性卡注册制（视口内控件风格参考）
- `AssetPreviewSystem.md` — LivePreviewProvider / AnimationPreviewState
- `AnimationAsset.md` — .anim 资产格式与 AnimationManager（✅ 已实现）
- `SkinnedAnimation.md` — AnimationAdvancer / AnimationStateMachine（Game 端）
