# 资产预览系统 (AssetPreviewSystem) 设计

## 概述

统一管理编辑器中的离屏渲染预览需求，覆盖从**文件图标**到**资产缩略图**再到**实时编辑预览**（材质、动画、Shader 连线等）的全部场景。

核心能力只有一条：**离屏渲染一个纹理，并且可以缓存和在指定位置渲染该纹理**。在此之上，不同场景有不同的缓存策略和特化逻辑。

## 架构分层

```
┌──────────────────────────────────────────────────────┐
│                    特化层 (Provider)                    │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐  │
│  │ FileIcon     │ │ AssetPreview │ │ LivePreview  │  │
│  │ Provider     │ │ Provider     │ │ Provider     │  │
│  │ (系统图标/    │ │ (网格/材质/   │ │ (动画/材质/   │  │
│  │  程序化图标)  │ │  纹理快照)   │ │  Shader 实时) │  │
│  └──────┬───────┘ └──────┬───────┘ └──────┬───────┘  │
│         │                │                │           │
│         └────────┬───────┴────────────────┘           │
│                  ▼                                    │
│  ┌──────────────────────────────────────────────┐     │
│  │            PreviewCacheManager                │     │
│  │  L1 磁盘缓存 / L2 内存缓存 / 失效检测          │     │
│  │  (纯数据管理，不涉及渲染)                       │     │
│  └──────────────────────┬───────────────────────┘     │
│                         ▼                             │
│  ┌──────────────────────────────────────────────┐     │
│  │            PreviewContext                     │     │
│  │  预览场景上下文（相机/灯光/实体/RT）+ System   │     │
│  │  (接入 ECS 管线，复用 RenderTargetPool)       │     │
│  └──────────────────────────────────────────────┘     │
└──────────────────────────────────────────────────────┘
```

### 设计原则

- **ECS 管线是调度框架，不是场景绑定**：预览渲染走现有的 System 调度机制，只是写到不同的 RT 上，不另起一套渲染管线。SSAO、阴影贴图已经是现成的离屏例子。
- **离屏渲染能力已存在，不重复造**：`RenderTargetPool`、`GpuResourceManager`、描述符管理都已具备。需要新增的是"预览场景上下文"这个数据切片，和把它接入 ECS 管线的 System。
- **缓存是分层策略，不是固定行为**：不同场景有不同的缓存需求，由特化层决定。

## 缓存三层模型

### L1 磁盘缓存

| 属性 | 说明 |
|------|------|
| 介质 | 硬盘（DDS 文件） |
| 策略 | 跨会话持久化，按资产文件时间戳失效 |
| 适用 | 资产缩略图（网格、材质预览），文件类型图标 |
| 写出 | 离屏渲染完成 → `ReadbackHeap` 读回 CPU → 编码 DDS → 写入磁盘 |
| 读入 | 启动时扫描缓存目录 → 读 DDS → 上传 GPU → 分配描述符 → 放入 L2 |
| 失效 | 用资产文件 `last_write_time` 与缓存文件时间戳比对 |

### L2 内存缓存

| 属性 | 说明 |
|------|------|
| 介质 | GPU 纹理（`ID3D12Resource`）+ SRV 描述符 |
| 策略 | 运行时 LRU，ImGui 直接引用 |
| 适用 | 当前可视目录的图标、预览图 |
| 来源 | 从 L1 加载，或从 L3 提升（用户主动快照） |

### L3 实时生成

| 属性 | 说明 |
|------|------|
| 介质 | 离屏 RT → 即时采样 |
| 策略 | 逐帧/按需重新渲染，不缓存 |
| 适用 | 材质参数调整实时反馈、动画播放预览、Shader 连线实时预览 |
| 交互 | 用户编辑 → 离屏渲染 → `ImGui::Image` 直接采样离屏 RT |

> L3 的离屏 RT 本身就是一个"单帧缓存"。当用户停止编辑时，可以选择将当前结果快照（Snapshot）到 L2/L1，但这由用户主动触发，不是自动行为。

## 核心模块

### 1. PreviewContext — 预览场景上下文

预览渲染的核心数据切片，**不是**一个完整的 World/Scene 实例。

```cpp
struct PreviewContext {
    // 独立相机（围绕预览对象旋转/缩放）
    Camera camera;
    
    // 独立灯光（固定三点布光或环境球光）
    LightSetup lights;
    
    // 要预览的资产实体（单个网格/模型）
    EntityHandle previewEntity;
    
    // 离屏渲染目标（从 RenderTargetPool 分配）
    RenderTargetHandle renderTarget;
    
    // 输出 SRV（供 ImGui::Image 使用）
    D3D12_GPU_DESCRIPTOR_HANDLE outputSRV;
    
    // 预览类型标识
    PreviewType type;
};
```

**关键约束**：
- 相机和灯光与主场景完全隔离，不影响主视口
- 渲染目标从 `RenderTargetPool` 池化分配，避免每帧创建
- 输出 SRV 描述符在 `DebugUIManager` 的 ImGui 描述符堆中分配
- CB 数据通过 `FrameScratchAllocator` 临时上传，不依赖 `LightManager`/`CameraManager` 等主场景管理器

### 2. FrameScratchAllocator — 帧临时上传分配器

#### 定位：引擎 CORE 模块

`FrameScratchAllocator` 属于引擎 CORE（Editor 和 Game 共用），不是 Editor 特有模块。它的使用场景不限于资产预览，还包括 Game 端和 Editor 端共用的临时上传需求。

预览渲染需要上传相机、灯光、变换矩阵等常量数据到 GPU，但这些数据每帧各不相同，且预览场景的灯光和相机与主场景完全隔离，不应复用主场景管理器的 upload buffer。

#### 使用场景

| 场景 | 端 | 数据量 | 频率 |
|------|----|--------|------|
| 资产预览 CB（相机/灯光/变换） | Editor | ~768B/上下文 | 每帧/按需 |
| Debug 绘制（线框、碰撞体、路径点） | Editor + Game | ~几 KB | 每帧 |
| 粒子系统临时上传 | Game | 不定 | 每帧 |
| 动态 UI 纹理（小地图、雷达图） | Game | 不定 | 按需 |
| ImGui vertex/index buffer | Editor | 不定 | 每帧 |

#### 与 FrameResourceManager 的关系

两者是**互补**关系，不是替代：

| 维度 | FrameResourceManager | FrameScratchAllocator |
|------|---------------------|----------------------|
| 定位 | 重型、持久帧数据 | 轻型、临时 ad-hoc 上传 |
| 缓冲策略 | 3 帧轮换 + fence reclaim | 单 ring-buffer，每帧重置 |
| 配置 | JSON 声明名称和大小（16MB/条） | 无配置，构造时指定大小（64KB~256KB） |
| 同步 | 复杂（fence 追踪每帧 GPU 进度） | 无（假设 GPU 在帧结束时已完成） |
| 数据持久性 | 跨帧有效（直到 reclaim） | 仅当前帧有效 |
| 典型用途 | 实例数据、蒙皮矩阵、对象 CB | 临时 CB、debug 顶点、小纹理上传 |

`FrameScratchAllocator` 不依赖 `FrameResourceManager` 的 fence 同步机制，也不需要 JSON 配置声明。它只是一个带 ring-buffer 的 upload heap，每帧重置写指针。

> **注意**：帧驱动器不提供单帧生命周期的资源，`FrameScratchAllocator` 是唯一的例外（因为它的数据是临时性的，假设 GPU 在帧结束时已完成消费）。除此以外，所有 GPU 资源分配至少 3 帧缓冲。

#### 现有管理器为什么不复用

| 管理器 | 为什么不复用 |
|--------|-------------|
| `LightManager` | 持有单场景的灯光缓冲区，`UpdateAndUpload` 写死到自己的 upload heap |
| `CameraManager` | 数据通过 `PassConstants` 写入 `FrameResource`，不对外暴露临时写入接口 |
| `GridManager` | 同理，单例单场景设计 |

扩展现有管理器支持多上下文（`PushPreviewContext`/`PopPreviewContext`）会显著增加接口复杂度，且预览场景的灯光需求远小于主场景（固定三点光 vs 几百盏灯），用重型管线是杀鸡用牛刀。

#### 设计：线性分配器，不是池

这不是一个内容池——`RenderTargetPool` 是真正的池，复用的是**内容**（RT 在多个帧之间保持有效）。而临时 CB 上传每次写入的都是**新数据**，不存在内容复用。复用的只是底层 upload heap 的**地址空间**。

```cpp
class FrameScratchAllocator {
    // 从 ring-buffer 中分配一块 CPU 可写、GPU 可见的空间
    // size: 所需字节数（自动对齐到 256B CB 对齐要求）
    // 返回 CPU 映射指针 + GPU 虚拟地址
    ScratchAllocation Allocate(uint32_t size);

    // 每帧开始调用，推进 ring-buffer 写位置
    // 上一帧分配的所有空间可安全回收（GPU 已完成该帧）
    void ResetFrame();
};

struct ScratchAllocation {
    void        *cpuPtr;   // CPU 映射写入地址
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddr; // GPU 可见地址，直接用于 SetGraphicsRootConstantBufferView
};
```

#### 数据量估算

一个预览上下文每帧需要上传的 CB 数据：

| 数据 | 大小（对齐后） | 说明 |
|------|---------------|------|
| PassConstants（View/Proj/ViewProj） | 256B | 预览相机矩阵 |
| 灯光数据（2-3 盏固定灯） | 256B | 位置、颜色、强度 |
| 对象 Transform（世界矩阵） | 256B | 预览实体的变换 |
| **合计（单预览上下文）** | **~768B** | |

一个 64KB 的 upload heap ring-buffer 可以容纳约 **80 个**预览上下文。即使同时打开多个预览面板（文件浏览器 + 材质编辑器 + 动画编辑器），也完全够用。

#### 封装为 PreviewRenderContext

在 `FrameScratchAllocator` 之上，封装一个面向预览场景的 typed wrapper：

```cpp
class PreviewRenderContext {
    // 构造时从 FrameScratchAllocator 分配 CB 空间
    void UploadCamera(const Camera &cam);
    void UploadLights(const LightSetup &lights);
    void UploadTransform(const XMMATRIX &world);
    
    // 绑定到命令列表
    void BindToCommandList(ID3D12GraphicsCommandList *cmdList);
    
    // 数据由 FrameScratchAllocator::ResetFrame() 统一回收，无需单独释放
};
```

#### 生命周期策略

| 使用场景 | 分配模式 | 回收时机 |
|----------|----------|----------|
| **静态预览**（生成缩略图） | 一次性 allocate → fill → render | 帧结束时 `ResetFrame()` |
| **LivePreview**（材质/动画实时） | 每帧 allocate → fill → render | 帧结束时 `ResetFrame()` |
| **L2 缓存纹理** | 持久分配，不走临时分配器 | 由 PreviewCacheManager 管理 |

两种场景在 API 层面没有区别——`FrameScratchAllocator` 不关心数据来源，只负责分配和回收地址空间。

#### 可复用性

`FrameScratchAllocator` 不限于预览系统，其他需要临时上传小量数据的场景也可复用：

- ImGui vertex/index buffer 上传
- 调试绘制（线框、调试球体）的 CB
- 拾取/射线检测的临时数据

### 3. PreviewSystem — 预览渲染 System

在 ECS 渲染管线中注册一个独立的 Pass，负责将所有待处理的 `PreviewContext` 渲染到对应的离屏 RT 上。

```
帧管线中的位置：
ShadowPass → OpaquePass → SSAOPass → TransparentPass → PreviewPass → UIPass
```

PreviewPass 的职责：
- 遍历所有活跃的 `PreviewContext`
- 为每个上下文设置对应的视口、RT、相机、灯光
- 渲染预览实体
- 执行对称屏障（入口转换 → 渲染 → 出口转换回到初始状态）

### 3. PreviewCacheManager — 缓存管理

纯数据管理模块，不涉及渲染。

```cpp
class PreviewCacheManager {
    // L1 磁盘缓存
    void SetCacheDirectory(const std::string &path);
    bool LoadFromDisk(const std::string &assetKey, /* out */ TextureData &data);
    void SaveToDisk(const std::string &assetKey, const TextureData &data);
    bool IsCacheValid(const std::string &assetKey, const std::filesystem::file_time_type &sourceTime);
    
    // L2 内存缓存
    ImTextureID GetCachedTexture(const std::string &assetKey);
    void CacheTexture(const std::string &assetKey, ID3D12Resource *texture, 
                      D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);
    void EvictLRU(uint32_t maxCount);
    
    // 缓存失效
    void Invalidate(const std::string &assetKey);
    void InvalidateAll();
};
```

## 特化层 (Provider)

### FileIconProvider

为文件浏览器提供文件类型图标，来源包括：

| 来源 | 说明 | 缓存策略 |
|------|------|----------|
| **Icon Font** | FontAwesome 合并到 ImGui 字体，`ImGui::Text(ICON_FA_FILE)` | 零 GPU 开销，始终可用 |
| **系统图标** | `SHGetFileInfo` 获取 Windows 图标，转为纹理缓存 | L1 + L2 |
| **程序化生成** | 运行时生成简单图标（文件夹、文件类型标记） | L2 仅内存 |

> Icon Font 优先使用，它是零开销的。系统图标和程序化图标在 Icon Font 覆盖不足时作为补充。

### AssetPreviewProvider

为资产选择器/文件浏览器提供缩略图预览，按资产类型分类：

| 资产类型 | 渲染策略 | 缓存策略 |
|----------|----------|----------|
| 纹理 | 直接采样纹理，带 checkerboard 背景 | L1 缓存（初次生成后永久有效） |
| 材质 | 在球体/立方体上应用材质，固定光照渲染 | L1 缓存，材质变更时失效 |
| 网格 | 在摄影棚中旋转展示模型 | L1 缓存，网格变更时失效 |
| 场景 | 场景缩略图（暂不实现） | — |

### LivePreviewProvider

为需要实时交互的编辑器（材质编辑器、动画编辑器、Shader Graph 等）提供实时的、可交互的预览。

```cpp
class LivePreviewProvider {
    // 设置要预览的资产
    void SetPreviewTarget(AssetHandle asset);
    
    // 每帧更新场景状态（由外部传入当前状态）
    void UpdateScene(const LivePreviewState &state);
    
    // 获取当前帧的渲染结果（供 ImGui::Image 使用）
    ImTextureID GetOutput();
    
    // 可选：保存当前帧为静态快照到 L1 缓存
    void SnapshotToCache(const std::string &key);
};

// 不同编辑器的 LivePreviewState 不同
struct MaterialPreviewState {
    MaterialParams params;
    LightingPreset lighting;
};

struct AnimationPreviewState {
    std::vector<XMMATRIX> boneTransforms;
    float timePosition;
    float playSpeed;
    bool isPlaying;
};
```

**LivePreview 的渲染路径**：
- 不经过 L1/L2 缓存
- 每帧直接渲染到离屏 RT，RT 的 SRV 直接作为 `ImTextureID`
- 用户停止编辑时，可选择 `SnapshotToCache()` 将当前帧写入 L1

## 集成路径

### 第一阶段：基础能力

1. 实现 `PreviewContext` 数据结构
2. 实现 `PreviewSystem`（在 ECS 管线中注册预览 Pass）
3. 接入 `RenderTargetPool` 分配临时 RT/DS
4. 验证：一个简单的 System 可以将网格渲染到离屏 RT，并在 ImGui 中显示

### 第二阶段：FileIconProvider

1. 加载 FontAwesome 字体，合并到 ImGui 字体
2. 实现 `FileIconProvider`，用 Icon Font 字符替换 `EditorAssetManager::GetIconTexture()` 的文本回退
3. 实现 `SHGetFileInfo` → 纹理的转换路径（可选，看 Icon Font 覆盖是否足够）

### 第三阶段：预览缓存

1. 实现 `PreviewCacheManager`（L1 磁盘读写 + L2 内存 LRU）
2. 实现 DDS 编码/解码（GPU 纹理 ↔ 磁盘文件）
3. 实现缓存失效检测（基于 `last_write_time`）

### 第四阶段：AssetPreviewProvider

1. 为每种资产类型实现"摄影棚"渲染逻辑
2. 接入 `PreviewCacheManager`，按需生成 + 缓存

### 第五阶段：LivePreviewProvider

1. 实现 `LivePreviewProvider` 框架
2. 接入材质编辑器（参数变化 → 实时重新渲染）
3. 接入动画编辑器（时间轴播放 → 每帧更新骨骼 → 实时渲染）

## 注意事项

- **对称屏障规则**：`PreviewSystem` 必须遵循入口转换 → 渲染 → 出口转换的对称屏障规则（项目约束第 10 条）
- **描述符生命周期**：预览 RT 的 SRV 描述符在 `DebugUIManager` 的描述符堆中分配，`Shutdown` 时必须在 `DebugUIManager::Shutdown()` 之前释放
- **RT 池化**：预览 RT 从 `RenderTargetPool` 分配，`OnResize` 时需检查尺寸是否真实变化（项目约束第 9 条）
- **异步安全**：L1 磁盘缓存的读写通过 `BackgroundExecutor` 进行，避免阻塞主线程
- **LivePreview 的线程安全**：`LivePreviewState` 由编辑器主线程更新，`PreviewSystem` 在渲染线程读取，需要 double-buffer 或 fence 同步

## 与现有架构的关系

| 现有模块 | 关系 |
|----------|------|
| `RenderTargetPool` | 预览 RT 从池中分配，使用完毕后归还 |
| `GpuResourceManager` | 预览纹理作为 GPU 资源，通过 `GpuResourceManager` 管理上传/释放 |
| `DebugUIManager` | 预览输出的 SRV 描述符在 ImGui 描述符堆中分配 |
| `EditorAssetManager::m_iconCache` | 第二阶段的 `FileIconProvider` 替换此处的图标缓存逻辑 |
| `BackgroundExecutor` | L1 磁盘缓存的异步读写通过此模块调度 |
| `SnapshotSystem` | L1 缓存失效检测可复用 `FileSnapshot` 的文件变更事件 |
| `ECS 渲染管线` | `PreviewSystem` 作为渲染管线中的一个 Pass 注册 |

## 相关文档

- 参见 `Docs/architecture/Editor.md` 编辑器架构
- 参见 `Docs/architecture/SnapshotSystem.md` 文件变更检测
- 参见 `Docs/notes/asyncResource.md` 异步资源加载