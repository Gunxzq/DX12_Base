# DX12_Base — 3D 渲染引擎框架

> 基于 DirectX 12 的轻量级 3D 渲染引擎框架，采用现代 C++ (C++17) 开发。  
> 不是框架的框架——架构设计优先于功能堆砌，约束驱动演化，文档与代码同步演进。

---

## 架构总览

引擎采用**分层 + 数据驱动**架构，从底向上依次为基础设施层、资源管理层、渲染管线层、场景与 ECS 层、应用层。每一层有明确的职责边界和依赖方向，禁止跨层依赖。

```
┌──────────────────────────────────────────────────────────────────┐
│  Game / Editor (应用层)                                           │
│  入口、配置、窗口、输入、编辑器面板、游戏逻辑                       │
│  GameWorld / Editor 各自持有 SceneManager 特化                    │
├──────────────────────────────────────────────────────────────────┤
│  Scene & ECS (场景与实体层)                                       │
│  SceneManager 统一管理实体生命周期 (CreateEntity → RegisterEntity) │
│  ECS Registry 内部私有，外部通过 EntityHandle 操作                 │
│  子场景模块 (RenderScene / PhysicsScene / AudioScene) 按领域拆分  │
├──────────────────────────────────────────────────────────────────┤
│  Renderer (渲染管线层)                                            │
│  ECS 组件 → Builder (PreRender 并行) → Renderer (PostProcess)     │
│  RHI 抽象层、Pipeline Pass 管理器、RenderItem 队列                 │
│  每个 Renderer 独立管理 ResourceBarrier，对称转换                  │
├──────────────────────────────────────────────────────────────────┤
│  Resource (资源管理层)                                            │
│  GPU 资源生命周期 (GpuResourceManager)                             │
│  纹理/几何体/材质管理器 (协作模式，不直接调用 Release)              │
│  描述符堆 (HeapTag 多堆/单堆策略)                                 │
├──────────────────────────────────────────────────────────────────┤
│  Infrastructure (基础设施层)                                      │
│  FrameDriver (帧循环调度)   BackgroundExecutor (异步执行)          │
│  Event System (四层架构)   SharedDataStore (跨线程中转)            │
│  ConfigManager (配置驱动)   Logger (日志)   Input (输入抽象)       │
└──────────────────────────────────────────────────────────────────┘
```

---

## 核心设计模式

### ECS → Builder → Renderer 管线

渲染数据从 ECS 组件到最终 DrawCall 经过三层转换，每层职责严格分离：

```
ECS Components (数据层)
  MeshComponent / LightComponent / WaterComponent / SkinnedComponent
        │ 只读
        ▼
Builders (PreRender 阶段，Worker 线程并行)
  OpaqueRenderItemBuilder / WaterRenderItemBuilder / SkinnedRenderItemBuilder
        │ 产出 RenderItem 队列
        ▼
Renderers (PostProcess 阶段，主线程录制命令)
  OpaqueRenderer / WaterRenderer / SkyRenderer / TerrainRenderer / BillboardRenderer
        │ 绑定 PSO → 设置描述符 → ResourceBarrier → DrawCall
        ▼
GPU 执行
```

| 层 | 职责 | 约束 |
|:---|:-----|:------|
| **ECS 组件** | 原子化数据，无行为，无生命周期 | 纯数据结构体，不包含逻辑 |
| **System** | 状态机节点，可读写 ECS 组件，可发消息 | 常驻 (AlwaysRun) 或消息触发 (WithMessage)，禁止用消息模拟每帧驱动 |
| **Builder** | 只读 ECS 组件，产出 RenderItem 队列 | Worker 线程并行执行，不分配内存 |
| **Renderer** | 只读 RenderItem，绑定 PSO/缓冲区，提交 DrawCall | 只录制命令列表，不分配内存 |

### 帧生命周期

帧循环由 `FrameDriver::Tick()` 驱动，严格按 Phase 顺序执行：

```
FrameDriver::Tick()
  │
  ├─ Immediate 回调
  │   ├─ CameraManager::UpdateMainCamera()
  │   ├─ LightManager::UpdateAndUpload()     ← 光源数据上传
  │   └─ WaterManager::UpdateAndUpload()     ← 波浪数据上传
  │
  ├─ PreRender (Worker 线程并行)
  │   ├─ BuilderUpload (串行，设 Frustum/Camera)
  │   ├─ BuildOpaque / BuildTransparent / BuildWater / ...  ← Builder 并行
  │   └─ FrameSync (串行，分配 RingBuffer)
  │
  ├─ Render
  │   ├─ 场景构造 (事件驱动，响应 GeneratorTaskCompleteEvent)
  │   └─ 阴影/SSAO 等预处理 Pass
  │
  └─ PostProcess
      ├─ 不透明渲染
      ├─ 水渲染
      ├─ 天空盒渲染
      └─ 透明渲染
```

**RingBuffer 生命周期策略**：每帧分配，3 帧后回收，自动扩容（翻倍增长），调用方无需关心容量。

### 异步加载管线

场景资产加载走三段式异步管线，不阻塞主线程渲染：

```
Scene JSON 文件
  ↓ SceneLoader::LoadFromFile
SceneDescription
  ↓ SceneConstructor::LoadScene
AssetManager::LoadBatch(meshes + textures)
  ↓ 后台线程
LoadTask (cpuWork → gpuWork → onComplete)
  ↓ 全部完成
OnDependenciesLoaded
  ├─ 纹理 SRV 映射
  ├─ 材质注册
  ├─ SkyboxManager::SetSkybox()
  ├─ 材质 buffer GPU 上传 (同步 submit + flush)
  └─ SceneConstructData → SharedDataStore
       ↓ PostEvent(GeneratorTaskCompleteEvent, payload = genType << 32 | sceneId)
SceneConstructSystem → CreateEntity + RegisterEntity → ECS 实体
```

跨线程数据传递通过 **SharedDataStore**（临时 LRU 缓存）中转，配合 **Event System** 的 64-bit Payload 完成通知，不直接共享指针。

---

## 架构约束

这些约束不是偶然的代码风格，而是经过多次 Bug 修复验证后提炼的**不可违反的设计规则**。违反任一约束会导致 GPU 崩溃、资源泄漏或渲染错误。

### GPU 资源状态管理

- **资源管理器/池只负责生命周期，不追踪也不重置 GPU resource state**
- 使用资源的 System 必须在自己管理的命令列表中通过 `ResourceBarrier` 将资源转到所需状态
- **不能假设资源初始状态为 `COMMON`**：池化资源被释放后可能在任意 GPU 状态，复用时不会重置
- 每个 System 独立管理其使用的所有资源的屏障，**不能假定上一个 System 留下了什么状态**
- 推荐模式：`COMMON → 目标状态 → 工作 → COMMON`，保证帧间状态一致
- 如果有多条 code path（如 PSO 未就绪时提前返回），**所有路径都必须做屏障回退**

### 渲染管线数据访问

- **主深度缓冲 (Main DSV) 和主颜色缓冲 (Main RTV) 在对应的主渲染阶段之外，只允许读取，不允许写入**
- 非主 Pass 使用私有 RT/DSV，通过 SRV 只读主渲染资源
- 这一约束与 ECS 多线程安全原则一致：System 只读 ECS 组件，修改通过临时结构进行

### 全屏 Quad 渲染

- 全屏 Quad / 后处理 Pass 必须在 Draw 前显式设置视口 (RSSetViewports) 和裁剪矩形 (RSSetScissorRects)
- 全屏 Quad 的 PSO 必须设置 `CullMode = NONE`（SV_VertexID 生成的三角形绕序为逆时针）
- 所有渲染器的根签名须完整声明着色器使用的静态采样器，否则 PSO 创建失败且无编译错误提示
- 从 C++ 端传入的 `XMMATRIX` 矩阵常量，在 HLSL 中必须声明为 `row_major float4x4`

### 初始化顺序

基础设施初始化有严格的顺序依赖，后初始化的系统可以安全引用前序系统的资源，反之则不可：

| 阶段 | 顺序 | 系统 | 关键约束 |
|:----:|:----:|------|:---------|
| **Bootstrap** (18 步) | 1–4 | ConfigManager → Logger → Input → Window | 无 |
| | 5–9 | D3D12Device → DescriptorHeaps → 分区 → DepthStencilPool → RTPool | 此时不可提交 GPU 命令 |
| | 10–16 | MaterialManager → TextureManager → FrameResource → GeometryResource → DebugUI → Dispatcher → ECS Registry | 仅初始化数据结构 |
| | 17–18 | FrameDriver → GNS | 此后可注册帧同步回调 |
| **Game** (11 步) | 1–4 | GpuResourceManager → OpaqueRenderer → CameraManager → LightManager | 命令管理器尚未就绪 |
| | 5–7 | AOManager → ReflectionProbeManager → 引擎级 System 注册 | AOManager 不可使用命令管理器 |
| | 8–11 | GameWorld::Initialize → BuildRandomVec → InitResourceStates → 输入处理器 | 命令管理器完全就绪 |

### 编辑器约束

- 编辑器新增面板时必须同步更新语言包 (`editor_strings_*.json`)
- 非 Default 堆域的堆分区必须在异步加载和主循环开始前注册完毕
- 编辑器主循环必须在 `FrameDriver::Tick()` 之前调用 `BackgroundExecutor::Tick()`
- 编辑器离屏 RT 的深度格式必须与主交换链一致，禁止硬编码深度格式
- 多堆模式下 `GetPartitionCpuHandle` 必须显式传入 `HeapTag`，禁止依赖 `HeapTag::Default` 默认参数

### 资源管理器协作模式

- 资源管理器 (`GeometryResourceManager` / `TextureManager` / `MaterialManager`) 与 `GpuResourceManager` 是**协作模式**，而非包含/依赖关系
- 资源管理器只管理自己的槽位/索引/引用计数，**禁止直接调用 `GpuResourceManager::Release`**
- GPU 资源的实际释放由 `GpuResourceManager` 统一管理，通过 `GpuWorkItem::uploadBufferHandles` 机制或 `GpuResourceManager::Update` 的 fence 回调完成

### 资产体系

资产分为两层：**原子资产**和**复合资产**，复合资产的依赖（原子资产）必须先于复合资产自身加载完成。

| 层级 | 类型 | 文件格式 | 运行时系统 |
|:-----|:-----|:---------|:-----------|
| **原子资产** | Mesh | `.dxmesh` | `GeometryResourceManager` |
| | Material | `.material` | `MaterialManager` |
| | Texture | `.dds` / `.png` / `.jpg` | `TextureManager` + `GpuResourceManager` |
| **复合资产** | Scene | `.scene.json` | `SceneConstructor` + ECS |
| | Terrain | `.terrain` | `TerrainManager` |
| | ParticleSystem | `.particle` | `ParticleManager` |
| | Prefab | `.prefab` | `SceneConstructor` |

---

## 长期稳定性保障

架构的长期稳定性不是靠"不修改代码"实现的，而是靠**设计文档记录决策、Bug 修复积累约束、迁移路径指引演化**。

### 设计文档体系

40+ 篇架构设计文档位于 `Docs/architecture/`，按子系统组织：

| 类别 | 文档 | 说明 |
|:-----|:------|:------|
| **核心架构** | EngineConstraints, DirectoryStructure, InitializationOrder, Frame, EngineOverview | 分层、约束、生命周期、初始化顺序 |
| **渲染管线** | pass, shadow, cull, GPU-Drive, LOD, RenderDataAccess, Reflection, AmbientOcclusion | Pass 组织、剔除、数据访问规则 |
| **场景与资产** | SceneManager, SceneFileAndLoading, AssetSpecification, AssetArchitecture, ResourceManager | 场景生命周期、资产规范、加载管线 |
| **编辑器** | Editor, EditorPanelSystem, SnapshotSystem, AssetPreviewSystem | 双模式架构、面板体系、热重载 |
| **事件系统** | EventSystemAndDataLayer | 四层架构 (L1-L4)、SharedDataStore |
| **资源管理** | FrameResourceManager, FormatConsistency, AdaptiveFarPlane | 帧资源分配器、格式一致性 |

每篇文档标注状态：`✅ 活跃` / `📋 计划` / `❌ 废弃`，确保文档与代码同步。

### Bug 修复记录体系

每个 Bug 修复记录在 `Docs/bugs/` 下，包含：

- 根因分析（Root Cause）
- 修复方案（Fix）
- 从中提炼的约束原则（Prevention）

已有记录涵盖：描述符槽双分配、根签名采样器缺失、编辑器视口格式不匹配、SSAO 资源状态不一致、Grid 着色器行主序矩阵、RenderDoc 无界表捕获失败、反射探针 Resize TDR 等。

### 迁移路径与路线图

重大架构变更不靠"一次性重写"，而是分阶段推进：

| 阶段 | 内容 | 文档 |
|:----:|:------|:------|
| **P0** | 文档定稿 + 接口定义 + 核心实现 | SceneManager 基类、EditorSceneManager、GameSceneManager |
| **P1** | 子场景模块实现 (RenderScene)、场景加载职责分离、系统执行恢复 | SceneManager 路线图 |
| **P2** | 场景切换资源释放、Undo/Redo 系统 | SceneManager 路线图 |
| **P3** | 流式加载、多场景 Tab 编辑 | SceneManager 路线图 |
| **P4** | PhysicsScene / AudioScene / NavMeshScene 子模块 | SceneManager 路线图 |

### 配置驱动设计

核心参数不硬编码，通过 JSON 配置驱动：

```json
{
  "ringBuffers": [
    { "name": "ObjectCB",   "initialSize": "16MB", "alignment": 256, "usage": "CBV" },
    { "name": "Skinning",   "initialSize": "16MB", "alignment": 16,  "usage": "SRV" },
    { "name": "Instance",   "initialSize": "16MB", "alignment": 16,  "usage": "SRV" },
    { "name": "WaterCB",    "initialSize": "16MB", "alignment": 256, "usage": "CBV" }
  ]
}
```

新增渲染功能（粒子、贴花、程序化植被）只需要在配置中加条目，**不修改引擎核心代码**。

---

## 项目结构

```
DX12_Base/
├── Engine/                        # 核心引擎代码
│   ├── Async/                     # 后台异步任务 (BackgroundExecutor, LoadTask)
│   ├── Boot/                      # 启动引导、配置、GameContext (依赖注入容器)
│   ├── Common/                    # 公共工具 (d3dUtil, MathHelper, d3dx12)
│   ├── Core/                      # 路径系统、项目配置、SharedDataStore
│   ├── DebugUI/                   # ImGui 调试面板
│   ├── ECS/                       # EnTT 实体组件系统 (Components, Registry)
│   ├── Event/                     # 事件系统 (四层架构: L1 Arena → L2 Data → L3 Scheduler → L4 System)
│   ├── Framework/                 # 系统注册与构建 (TaskGraphBuilder)
│   ├── Logger/                    # 日志系统 (多端输出: 调试输出 / 日志窗口 / 空)
│   ├── Math/                      # 数学库 (包围体、射线相交、哈希)
│   ├── Network/                   # 网络传输层抽象 (GNS / P2P)
│   ├── Platform/                  # 平台抽象 (输入系统、窗口管理)
│   ├── Renderer/                  # 渲染管线
│   │   ├── Pipeline/              #   渲染 Pass (Opaque, Shadow, Sky, Water, Terrain, Billboard)
│   │   ├── RHI/                   #   D3D12 设备抽象层
│   │   ├── Scene/                 #   场景内容 (Camera, Light, Terrain)
│   │   ├── Material/              #   材质系统 (句柄 + 数据 + 管理器)
│   │   ├── RenderItemBuilder/     #   ECS 组件 → 渲染项 (Builder 并行)
│   │   ├── Effects/               #   系统内置效果 (SSAO)
│   │   ├── Core/                  #   剔除、LOD
│   │   ├── FrameResources/        #   帧资源 (RingBuffer)
│   │   └── Utils/                 #   工具
│   └── Resource/                  # GPU 资源管理
│       ├── Manager/               #   几何体/骨骼管理器
│       ├── Texture/               #   纹理系统 (磁盘资产 + 加载)
│       ├── Pool/                  #   池化资源 (DepthStencilPool, RenderTargetPool)
│       ├── GpuResourceManager/    #   底层 GPU 资源生命周期管理
│       ├── AssetLoader/           #   资产加载器
│       ├── Geometry/              #   几何体类型定义
│       ├── Struct/                #   句柄/描述符定义
│       └── Utils/                 #   通用工具 (PathUtils, HashUtils, FileUtils)
│
├── Runtime/                       # 业务逻辑与入口
│   ├── Application/               # 应用程序入口 (WinMain)
│   └── Game/                      # 游戏逻辑 (Game, GameWorld, Input)
│
├── Editor/                        # 编辑器 (WITH_EDITOR 编译宏隔离)
│   ├── EditorLib/                 # 编辑器核心 (Editor, EditorLayout, Panel 体系)
│   └── Config/                    # 编辑器配置 (语言包、布局)
│
├── Shaders/                       # HLSL 着色器
├── Config/                        # JSON 配置文件 (renderer.json, ringbuffers.json)
├── Content/                       # 运行时资源 (模型、纹理、场景、地形)
├── Docs/                          # 设计文档
│   ├── architecture/              #   架构设计 (40+ 篇)
│   ├── bugs/                      #   Bug 修复记录
│   ├── notes/                     #   开发笔记
│   ├── todos/                     #   待办清单
│   └── targets/                   #   目标分析
├── Tests/                         # Google Test 单元测试
│
├── CMakeLists.txt                 # CMake 构建脚本
└── README.md                      # 本文件
```

---

## 环境要求与构建

### 环境

- **系统**: Windows 10/11 (64-bit)
- **编译器**: MSVC (Visual Studio 2022)
- **构建工具**: CMake (≥ 3.14)
- **包管理器**: vcpkg

### 第三方库

| 库 | 用途 |
|:---|:------|
| **Dear ImGui** | 即时模式 GUI (调试界面与编辑器工具) |
| **EnTT** | ECS 框架 (游戏对象管理) |
| **assimp** | 3D 模型加载 (.obj, .gltf, .fbx) |
| **spdlog** | 高性能日志系统 |
| **nlohmann/json** | JSON 配置解析 |
| **concurrentqueue** | 无锁并发队列 (多线程任务调度) |
| **mimalloc** | 高性能内存分配器 (全局替换) |
| **Jolt Physics** | 物理引擎 (刚体碰撞与关节模拟) |
| **Google Test** | 单元测试框架 (可选) |

### 快速开始

```powershell
# 1. 安装依赖 (vcpkg)
cd D:\Dev\vcpkg
.\vcpkg install nlohmann-json:x64-windows concurrentqueue:x64-windows ^
    entt:x64-windows spdlog:x64-windows assimp:x64-windows ^
    mimalloc:x64-windows joltphysics:x64-windows ^
    imgui[dx12-binding]:x64-windows gtest:x64-windows

# 2. 配置 CMake
cmake -B build -DCMAKE_TOOLCHAIN_FILE=D:/Dev/vcpkg/scripts/buildsystems/vcpkg.cmake

# 3. 构建
cmake --build build

# 4. 运行
./build/Bin/x64/Debug/DX12_Base.exe
```

---


- **约束即特性**：每个约束（对称屏障、只读主资源、三步加载）都对应一个真实 Bug，不是过度设计
- **显式优于隐式**：ResourceBarrier 显式声明、HeapTag 显式传入、初始化顺序显式列表——不依赖巧合和假设
- **文档与代码同步**：架构决策记录在 Docs/ 下，Bug 修复提炼为约束写入文档，新增功能先写设计文档再实现
- **分阶段演进**：重大架构变更走 P0-P4 路线图，不搞一次性重写，每条迁移路径都有明确的前置条件

---
