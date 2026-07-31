# Docs 文档索引

引擎设计文档按类别组织，避免重复设计已在计划中的模块。

---

## 📐 架构设计 (`architecture/`)

| 文档 | 内容 | 状态 |
|------|------|------|
| **EngineConstraints** | 引擎开发约束：系统调度、多线程安全、GPU 屏障规则、对齐要求 | ✅ 活跃 |
| **DirectoryStructure** | 目录结构划分原则 | ✅ 活跃 |
| **InitializationOrder** | 基础设施初始化顺序 | ✅ 活跃 |
| **Frame** | 帧循环与 BufferSegment 分配器 | ✅ 活跃 |
| **RenderDataAccess** | 渲染管线数据访问规则 | ✅ 活跃 |
| **FrameResourceManager** | 帧资源分配器职责与配置化方向 | ✅ 活跃 |

### 渲染管线

| 文档 | 内容 | 状态 |
|------|------|------|
| **pass** | 渲染 Pass 组织方式 | ✅ 活跃 |
| **shadow** | 阴影贴图演进方向 | ✅ 活跃 |
| **cull** | 剔除系统 | ✅ 活跃 |
| **GPU-Drive** | GPU Driven 管线方向 | ✅ 活跃 |
| **LOD** | LOD 系统 | ✅ 活跃 |
| **Raycaster** | 可见集射线检测 | ✅ 活跃 |
| **AmbientOcclusion** | 环境光遮蔽 | ✅ 活跃 |
| **Reflection** | 反射探针系统 | ✅ 活跃 |

### 配置系统

| 文档 | 内容 | 状态 |
|------|------|------|
| **config** | JSON/INI 格式策略讨论 | ✅ 活跃 |
| **configSystem** | 配置系统完整架构（Docs/notes/） | ✅ 活跃 |

> 配置系统相关模块：**ConfigManager**（配置加载器）、**ErrorReporter**（错误报告器）

### 场景与资产加载

| 文档 | 内容 | 状态 |
|------|------|------|
| **World** | **ECS 绝对源头：单一 World + 逻辑分区（Editor 端），SceneManager 降级为场景序列化器** | 📋 新设计 |
| **SceneManager** | **场景管理器架构：核心 SceneManager 设计（场景序列化器 + 环境状态容器）、Game/Editor 双端特化、子场景模块体系、场景生命周期、加载器体系、World 提取与 SceneManager 降级** | 📋 新设计 |
| **SceneFileAndLoading** | 场景文件格式 + AssetManager 加载管线 + 场景构造 | 📋 新设计 |
| **EventSystemAndDataLayer** | 事件系统四层架构 + 数据层（ECS / SharedDataStore）关系 | ✅ |
| **ResourceManager** | 资源管理器架构、AssetManager 设计 | ✅ 活跃 |
| **RelationshipModel** | **实体关系模型：扁平 JSON + ID 引用，关系类型（parent/socket/group/follow），引擎 CORE 只存不处理，submesh 与实体边界** | 📋 新设计 |
| **AssetSpecification** | **资产规范：原子资产（Mesh/Material/Texture/Skeleton/Animation 五元组）、复合资产（Character/Scene/Terrain/ParticleSystem）、加载器注册表、文件格式规范** | 📋 新设计 |
| **CharacterAsset** | **角色复合资产（`.character`）：骨架 `.bone` + 网格 + 材质槽 + 动画剪辑 `.anim` 打包，可跨场景复用；场景只放实例引用。含动画播放/调帧能力设计** | 📋 新设计 |
| **AssetArchitecture** | 资产体系架构设计思路、与几何系统子类型的关系、迁移步骤 | 📋 新设计 |
| **AssetTypeDefinition** | **资产类型定义（AssetType）——新增类型时需同步修改的部分清单** | 📋 新设计 |

> 相关模块：**BackgroundExecutor**（TaskGraph 后台执行器）、**AssetLoader**（文件读取）、**AssetDataManager**（CPU 数据中转）

### 资源与动画

| 文档 | 内容 | 状态 |
|------|------|------|
| **SkinnedAnimation** | 蒙皮骨骼动画系统（AnimationAdvancer + AnimationStateMachine） | ✅ 活跃 |
| **AdaptiveFarPlane** | 自适应远平面 | ✅ 活跃 |
| **FormatConsistency** | DXGI 格式一致性 | ✅ 活跃 |

### 编辑器

| 文档 | 内容 | 状态 |
|------|------|------|
| **Editor** | 编辑器/发布双模式架构 | ✅ 活跃 |
| **SnapshotSystem** | **操作快照 + 文件变更检测 + 热重载** | ✅ 活跃 |
| **AssetPreviewSystem** | 离屏渲染预览系统（文件图标 / 资产缩略图 / 实时编辑预览） | 📋 新设计 |

> - **重要**：Snapshot System L1 `FileSnapshot` 已提供文件级变更监听能力，任何需要热重载的模块（如 ConfigManager）应订阅其事件，不要重新设计文件监听器。
> - **AssetPreviewSystem** 的 L1 缓存失效检测可复用 `FileSnapshot` 的文件变更事件。

> ~~IconResourceManager~~ → 已合并到 **AssetPreviewSystem**（见 `Docs/architecture/AssetPreviewSystem.md`）

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
2. **跨文档引用**——在文档头部注明相关内容定位，如 `参见 Docs/architecture/SnapshotSystem.md L1`
3. **状态标记**——`✅ 活跃` / `📋 计划` / `❌ 废弃`
