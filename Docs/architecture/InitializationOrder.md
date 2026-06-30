# 初始化顺序与约束

## 概述

系统的初始化分两个阶段：**Bootstrap 阶段**（引擎基础设施）和 **Game 阶段**（游戏模块）。  
阶段内部有严格的顺序依赖：后初始化的系统可以安全引用前序系统的资源，反之则不可。

---

## 第一阶段：Bootstrap（引擎基础设施）

`Bootstrap::InitializeModules()` → `Bootstrap::CreateContext()`

| 顺序 | 系统/组件 | 说明 | 依赖 |
|:---:|-----------|------|------|
| 1 | `ConfigManager` | 配置管理 | 无 |
| 2 | `EngineLogger` | 日志系统 | ConfigManager |
| 3 | `InputManager` | 输入管理 | ConfigManager |
| 4 | `Window` | 主窗口 | ConfigManager |
| 5 | `D3D12DeviceContext` | D3D12 设备上下文 | Window (窗口句柄) |
| 6 | `DescriptorHeapCollection` | 描述符堆集合 | D3D12DeviceContext |
| 7 | Texture/Buffer 分区 | 描述符分区注册 | DescriptorHeapCollection |
| 8 | `DepthStencilPool` | 深度模板池 | Device + DescriptorHeaps |
| 9 | `RenderTargetPool` | 渲染目标池 | Device + DescriptorHeaps |
| 10 | `MaterialManager` | 材质管理器 | 无（仅 CPU 数据结构） |
| 11 | **`TextureManager`** | **纹理管理器** | Device + DescriptorHeaps |
| 12 | `FrameResourceManager` | 帧资源管理器 | Device + DescriptorHeaps |
| 13 | `GeometryResourceManager` | 几何体资源管理器 | 无 |
| 14 | `DebugUI` | 调试 UI | 渲染管线和窗口 |
| 15 | `MessageDispatcher` | 事件分发器 | 无 |
| 16 | `ECS::Registry` | ECS 注册表 | 无 |
| 17 | `FrameDriver` | 帧驱动器 | Registry |
| 18 | `GameNetworkingSockets` | 网络层 | 无 |

### Bootstrap 阶段的关键约定

- **TextureManager** (顺序 11) — 此时仅初始化内部数据结构，**没有 GPU 纹理资源**
- **RenderTargetPool** (顺序 9) — 可以分配 RT，但 **命令管理器尚未就绪**，不能提交 GPU 命令
- **FrameDriver** (顺序 17) — 此后的系统可以注册帧同步回调

---

## 第二阶段：Game（游戏模块）

`Game::Initialize()` → `GameWorld::Initialize()`

| 顺序 | 系统/组件 | 说明 | 依赖 |
|:---:|-----------|------|------|
| 1 | `GpuResourceManager::Initialize()` | GPU 资源管理器 | D3D12DeviceContext |
| 2 | `OpaqueRenderer` | 不透明渲染器 | Device + GeometryMgr + MaterialMgr |
| 3 | `CameraManager` | 相机 | 无 |
| 4 | `LightManager` | 光源管理器 | Device + DescriptorHeaps |
| 5 | **`AmbientOcclusionManager`** | **SSAO 管理器** | Device + DescriptorHeaps |
| 6 | `ReflectionProbeManager` | 反射探针管理器 | 已在 Bootstrap CreateContext 初始化 |
| 7 | 引擎级系统注册 | ECS 系统 | Registry |
| **8** | **`GameWorld::Initialize()`** | **场景初始化** | Context + Renderer |
| 9 | `BuildRandomVectorTexture()` | SSAO 随机向量纹理上传 | **需要命令管理器就绪** |
| 10 | `InitializeResourceStates()` | AO RT 状态过渡 | BuildRandomVectorTexture 之后 |
| 11 | 输入处理器 | 拾取/交互 | GameWorld |

### Game 阶段的关键约束

> **`AmbientOcclusionManager::Initialize()`（顺序 5）中不可使用命令管理器。**
> 命令管理器在 `GameWorld::Initialize()`（顺序 8）之后才完全就绪。
> 需要提交 GPU 命令的操作（纹理上传、资源状态转换）必须推迟到顺序 9 之后。

---

## 关键依赖关系（约束矩阵）

| 后↓ \ 前→ | TextureMgr | GpuResourceMgr | 命令管理器 | GameWorld 场景纹理 |
|-----------|:----------:|:--------------:|:----------:|:-----------------:|
| **AOManager::Initialize** | ✅ 可以引用 | ✅ 可以引用 | ❌ **不可用** | ❌ **尚未创建** |
| **AOManager::BuildRandomVec** | ✅ | ✅ | ✅ 可用 | ✅ 已存在 |
| **OpaqueRenderer::BeginFrame** | ✅ | ✅ | ✅ | ✅ 已存在 |

### 本次改动引入的约束

**`AmbientOcclusionManager` 的白色回退纹理依赖 `GameWorld` 的 `m_whiteTextureHandle`。**

- `GameWorld::Initialize()`（顺序 8）创建 1×1 白色纹理并注册到 TextureManager
- `AmbientOcclusionManager::GetAmbientMapSRV()` 在渲染时调用（远晚于顺序 8）
- 因此：**设置 `SetFallbackWhiteSRV()` 必须在 GameWorld::Initialize 之后**

> **规则：如果 Manager A 需要依赖 Manager B 的纹理/资源作为回退，  
> Manager B 的资源创建必须在 Manager A 的资源访问之前完成。  
> 若无法在初始化时满足，应通过回调/Setter 在依赖就绪后注入。**

---

## 模式总结

### 允许的模式

| 模式 | 示例 | 安全 |
|------|------|:----:|
| 管理器 A 在 Bootstrap 阶段创建，B 在 Game 阶段引用 | TextureMgr → AOManager | ✅ |
| 场景纹理在 GameWorld 创建，渲染时引用 | WhiteTexture → AOManager 渲染时 | ✅ |
| 同一阶段，顺序靠前的系统被靠后的引用 | GpuResourceMgr → AOManager | ✅ |

### 禁止的模式

| 模式 | 示例 | 风险 |
|------|------|:----:|
| 在 `Initialize()` 中提交 GPU 命令 | AOManager 在 Init 中上传纹理 | ❌ 命令管理器未就绪 |
| 引用尚未创建的资源 | AOManager 在 Init 中引用 GameWorld 的白纹理 | ❌ 纹理不存在 |
| 跳阶段引用 | Game 阶段引用 Bootstrap 阶段尚未初始化的系统 | ❌ 空指针/未定义 |
