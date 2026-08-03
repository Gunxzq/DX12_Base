# Docs 文档索引

引擎设计文档按类别组织，避免重复设计已在计划中的模块。

---

## 📐 架构设计 (`architecture/`)

> 目录按主题划分 6 簇：`core/`（基础设施）、`rendering/`（渲染管线）、`assets/`（资产体系）、`scene/`（场景与实体）、`editor/`（编辑器）、`animation/`（动画）。

### 🏗️ core/ — 基础设施与总览

| 文档 | 内容 | 状态 |
|------|------|------|
| **EngineConstraints** | 引擎开发约束：系统调度、多线程安全、GPU 屏障规则、对齐要求 | ✅ 活跃 |
| **EngineOverview** | **ECS 统一数据源 + Manager 打包器设计（ECS Registry 唯一数据源，Manager 不持私有数据）** | ✅ 活跃 |
| **DirectoryStructure** | 目录结构划分原则 | ✅ 活跃 |
| **InitializationOrder** | 基础设施初始化顺序 | ✅ 活跃 |
| **Frame** | 帧循环与 BufferSegment 分配器 | ✅ 活跃 |
| **EventSystemAndDataLayer** | 事件系统四层架构 + 数据层（ECS / SharedDataStore）关系 | ✅ |
| **SnapshotSystem** | **操作快照 + 文件变更检测 + 热重载** | ✅ 活跃 |
| **AsyncPipelineResponsibilities** | 异步管线职责划分 | ✅ 活跃 |
| **ProjectConfigSystem** | 项目配置系统 | ✅ 活跃 |
| **config** | JSON/INI 格式策略讨论 | ✅ 活跃 |
| **InputSystem** | 输入系统（声明式推送模式） | ✅ 活跃 |

> - **重要**：Snapshot System L1 `FileSnapshot` 已提供文件级变更监听能力，任何需要热重载的模块（如 ConfigManager）应订阅其事件，不要重新设计文件监听器。
> - 配置系统相关模块：**ConfigManager**（配置加载器）、**ErrorReporter**（错误报告器）。完整架构见 `Docs/notes/configSystem.md`。

### 🎨 rendering/ — 渲染管线

| 文档 | 内容 | 状态 |
|------|------|------|
| **RendererDataDriven** | **渲染器数据驱动与绑定架构：PSO 集合描述（几何条件共享+变体各一）、静态描述/动态绑定分离、BeginFrame(pass)/Draw(item) 拆分、渲染项统一（全可能大渲染项 + bindings 模式 §4.2a1）、槽位全声明（§2.2，含 Constants32/CBV 数据源判别）、RenderContext 外观（get 透传+占位兜底）、几何/材质条件校验、特殊渲染器保留管理器、大型引擎（UE FMeshDrawCommand/Unity SRP）比对** | 📋 新设计 |
| **pass** | 渲染 Pass 组织方式 | ✅ 活跃 |
| **shadow** | 阴影贴图演进方向 | ✅ 活跃 |
| **OctreeCullingAndRaycaster** | **八叉树空间划分 + 剔除管线 + Raycaster 统一架构（原 cull.md/Raycaster.md 已合并入本文并删除，要点见 §8）** | 📋 设计方案 |
| **GPU-Drive** | GPU Driven 管线方向 | ✅ 活跃 |
| **LOD** | LOD 系统 | ✅ 活跃 |
| **AmbientOcclusion** | 环境光遮蔽 | ✅ 活跃 |
| **Reflection** | 反射探针系统 | ✅ 活跃 |
| **AdaptiveFarPlane** | 自适应远平面 | ✅ 活跃 |
| **FormatConsistency** | DXGI 格式一致性 | ✅ 活跃 |
| **FrameResourceManager** | 帧资源分配器职责与配置化方向 | ✅ 活跃 |
| **RenderDataAccess** | 渲染管线数据访问规则 | ✅ 活跃 |
| **SubMeshMaterialSlots** | 子网格材质槽模式（#22/#23/#24） | ✅ 活跃 |
| **BillboardSystemArchitecture** | 公告牌系统架构 | ✅ 活跃 |
| **WaterSystemArchitecture** | 水体系统架构 | ✅ 活跃 |
| **WindowFrameResources** | 窗口帧资源（G-buffer/SceneColor/DepthStencil） | ✅ 活跃 |

### 📦 assets/ — 资产体系

| 文档 | 内容 | 状态 |
|------|------|------|
| **AssetSpecification** | **资产规范：原子资产（Mesh/Material/Texture/Skeleton/Animation 五元组）、复合资产（Character/Scene/Terrain/ParticleSystem）、加载器注册表、文件格式规范** | 📋 新设计 |
| **AssetTypeDefinition** | **资产类型定义（AssetType）——新增类型时需同步修改的部分清单** | 📋 新设计 |
| **AssetArchitecture** | 资产体系架构设计思路、与几何系统子类型的关系、迁移步骤 | 📋 新设计 |
| **AssetFormatStrategy** | 资产文件格式策略 | 📋 新设计 |
| **AssetLoaderImprovement** | 资产加载器改进（Godot ResourceFormatLoader 注册表模式） | 📋 设计定案 |
| **CharacterAsset** | **角色复合资产（`.character`）：骨架 `.bone` + 网格 + 材质槽 + 动画剪辑 `.anim` 打包，可跨场景复用** | 📋 新设计 |
| **SkeletalMeshAssetPipeline** | 骨骼网格资产管线 | 📋 新设计 |
| **AssetPreviewSystem** | 离屏渲染预览系统（文件图标 / 资产缩略图 / 实时编辑预览） | 📋 新设计 |
| **ResourceManager** | 资源管理器架构、AssetManager 设计 | ✅ 活跃 |

> 相关模块：**BackgroundExecutor**（TaskGraph 后台执行器）、**AssetLoader**（文件读取）、**AssetDataManager**（CPU 数据中转）

### 🌍 scene/ — 场景与实体

| 文档 | 内容 | 状态 |
|------|------|------|
| **World** | **ECS 绝对源头：单一 World + 逻辑分区（Editor 端），SceneManager 降级为场景序列化器** | 📋 新设计 |
| **SceneManager** | **场景管理器架构：核心 SceneManager 设计（场景序列化器 + 环境状态容器）、Game/Editor 双端特化、子场景模块体系、场景生命周期、加载器体系** | 📋 新设计 |
| **SceneFileAndLoading** | 场景文件格式 + AssetManager 加载管线 + 场景构造 | 📋 新设计 |
| **SceneStateMachine** | **场景状态机与实体生命周期：场景=扁平纯容器、独立全局状态机文件、输入上下文栈、角色跨场景持久实体层** | 📋 新设计 |
| **SceneSnapshot** | 场景快照 | 📋 新设计 |
| **RelationshipModel** | **实体关系模型：扁平 JSON + ID 引用，关系类型（parent/socket/group/follow），引擎 CORE 只存不处理** | 📋 新设计 |
| **PrefabAndGenerator** | Prefab 与生成器 | 📋 新设计 |
| **EntityTemplates** | 实体模板（Outliner 创建菜单，JSON 驱动） | 📋 新设计 |

### 🛠️ editor/ — 编辑器

| 文档 | 内容 | 状态 |
|------|------|------|
| **Editor** | 编辑器/发布双模式架构 | ✅ 活跃 |
| **EditorPanelSystem** | **编辑器 Panel 体系（IEditorPanel 接口、EditorLayout 职责、Panel Draw 规范）** | ✅ 活跃 |
| **ComponentEditorSystem** | 组件属性卡编辑系统（ECS 组件驱动注册制） | ✅ 活跃 |
| **ViewportToolbar** | 视口工具栏 | ✅ 活跃 |
| **WireframeDebugDraw** | **引擎级调试线框渲染（WireframeManager，Game/Editor 共用，管理器模式 + GS 展开 + 深度）** | ✅ 活跃 |
| **IconResourceManager** | ❌ 已合并到 **AssetPreviewSystem**（见 `Docs/architecture/assets/AssetPreviewSystem.md`） | ❌ 废弃 |

### 🎬 animation/ — 动画

| 文档 | 内容 | 状态 |
|------|------|------|
| **SkinnedAnimation** | 蒙皮骨骼动画系统（AnimationAdvancer + AnimationStateMachine） | ✅ 活跃 |
| **AnimationAsset** | 动画资产格式规范（`.anim`，格式规范见 AssetTool 注释引用） | 📋 新设计 |
| **AnimationViewport** | 动画视口（预览/调帧） | 📋 新设计 |

---

## 🐛 Bug 修复记录 (`bugs/`)

| 文档 | 内容 |
|------|------|
| **BUGS** | 已知问题汇总 |
| **BugFix_DescriptorSlotAllocator_DoubleAlloc** | 描述符槽双分配修复 |
| **BugFix_LightingPass_RootSigSampler** | 光照 Pass 根签名采样器修复 |
| **BugFix_ReflectionProbe_ResizeTDR** | 反射探针 Resize TDR 修复 |
| **BugFix_RenderDoc_UnboundedTable_CaptureFail** | 无界表 RenderDoc 捕获失败修复 |
| **BugFix_SSAO_AmbientResourceStateMismatch** | SSAO 资源状态不匹配修复 |
| **terrain_debug** | 地形调试记录 + CBV 256 对齐说明 |

---

## 📝 开发笔记 (`notes/`)

| 文档 | 内容 |
|------|------|
| **configSystem** | 配置系统完整架构（格式、加载器、热重载现状） |
| **MemoryAllocStrategy** | 内存分配策略 |
| **TexGeoMat** | 纹理·几何·材质关系 |
| **texArray** | 纹理数组 vs 材质数组 |
| **instanceData** | 实例数据处理 |
| **asyncResource** | 异步资源加载 |
| **performance** | 性能优化记录 |
| **picking** | 拾取系统 |
| **book** | 参考书目 |
| **P2P** | P2P 网络同步 |
| **TTimerManager** | 计时器管理器 |

---

## 📋 待办清单 (`todos/`)

| 文档 | 内容 |
|------|------|
| **remaining_issues** | 全局待办清单（当前所有活跃 TODO 集中于此，含 ANI 解析器——下一会话第一任务） |
| **todo** | 历史待办（遗留条目） |
| **todo6** | 历史待办（遗留条目） |
| **todo-10** | 渲染构建器并行化与多缓冲 |

---

## 使用原则

1. **新增文档前，先扫索引**——确认要设计的模块是否已在 Snapshot System、ResourceManager 等现有设计中存在
2. **跨文档引用**——在文档头部注明相关内容定位，如 `参见 Docs/architecture/core/SnapshotSystem.md L1`
3. **状态标记**——`✅ 活跃` / `📋 计划` / `❌ 废弃`
