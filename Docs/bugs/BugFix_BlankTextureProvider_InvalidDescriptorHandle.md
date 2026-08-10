# BugFix: 管理器空白纹理缺失导致 LightingRenderer envMapSrv 无效描述符崩溃

## 日期

2026-08-07

## 症状

编辑器启动/运行后崩溃，栈指向 `LightingRenderer::BeginFrame`（`SetGraphicsRootDescriptorTable(8, envMapSrv)`）：

```
D3D12 ERROR: SetGraphicsRootDescriptorTable: Specified GPU descriptor handle ptr=0x800002b4a22b9fbd
  does not refer to a location in a descriptor heap. [ EXECUTION ERROR #646: INVALID_DESCRIPTOR_HANDLE]
```

- **开 GBV**：`#646 INVALID_DESCRIPTOR_HANDLE` + `d3d12SDKLayers.dll` 崩溃
- **不开 GBV**：`_com_error` + `0xC0000005`（写地址 0x0）——**真实绑定无效描述符，非 GBV 误报**

## 根因

多个管理器（`AmbientOcclusionManager` / `SkyboxManager` / `WaterManager`）在对应资源**尚未加载**时，其 `GetXXXSRV()` 返回**空句柄**（`ptr==0`）：

- `LightingRenderer` 对空句柄条件跳过绑定（`if (srv.ptr != 0)`）→ 根参数未绑定 → shader 采样时 GBV #935（shader-if 方案已缓解）
- 管理器**内部自建 fallback** 的 `Allocate(EditorViewport, PartitionType::Texture)` 若在分区未注册时执行，`DescriptorHeapCollection::Allocate` 走 **Fallback 分支**（物理堆分配）返回**裸 index**，随后 `GetPartitionGpuHandle` 用 `baseOffset + index` 计算 → **无效 GPU 句柄**（`0x8000...`）→ `#646 INVALID_DESCRIPTOR_HANDLE` 崩溃

**关键缺口**：`AmbientOcclusionManager::SetFallbackWhiteSRV` 在 Game 端（`GameWorld.cpp:80`）已调用，**Editor 端从未调用**。

## 修复方案（引擎 CORE 空白纹理提供者）

新增引擎 CORE 单例 **`BlankTextureProvider`**（Game/Editor 共用）：

| 文件 | 内容 |
|:--|:--|
| `Engine/Renderer/Core/BlankTextureProvider.h/.cpp` | 同步创建空白纹理：White2D（1x1 R8G8B8A8 0xFFFFFFFF）+ BlackCube（1x1x6 R8G8B8A8 全 0） |
| `CMakeLists.txt` | RENDERER_SOURCES 显式 APPEND |

**同步语义**：`Initialize` 内 `UpdateSubresources` 上传 + `cmdMgr->Flush(DIRECT)` **阻塞至 GPU 完成**，返回的 SRV 在后续任意管理器 Initialize 时即可用。

**接入时机**（`Editor::Initialize`）：`AddPartition(RTV/DSV, EditorViewport)` **之后**、`aoMgr.Initialize` / `SkyboxManager.Initialize` **之前**——从时序上根治 #646（分区注册后再 Allocate）。

**注入**：

| 空白纹理 | 注入目标 |
|:--|:--|
| White2D SRV | `AmbientOcclusionManager::SetFallbackWhiteSRV`（补齐 Editor 端缺口） |
| BlackCube SRV | `SkyboxManager::SetFallbackCubeSRV`（新增接口，`GetCubeSRV` 无天空盒时兜底） |
| BlackCube SRV | `WaterManager::SetEnvironmentMap`（水环境贴图兜底） |

**双保险**：此前 shader-if 方案的 `gHasSsao/gHasEnvMap/gHasShadow` 标志保留（有资源采样、无资源跳过），与新注入不冲突。

## 涉及文件

- `Engine/Renderer/Core/BlankTextureProvider.h/.cpp`（新增）
- `CMakeLists.txt`（APPEND 新文件）
- `Engine/Renderer/Scene/SkyboxManager.h/.cpp`（`SetFallbackCubeSRV` + `GetCubeSRV` 兜底）
- `Editor/EditorLib/Core/Editor.cpp`（接入 BlankTextureProvider + 注入）

## 验证状态

- 代码静态审查通过（API 签名、include 路径、注入参数类型一致）
- 编译/运行验证：**待人工执行**（项目规则：AI 不编译）——重点看 `LightingRenderer.cpp:198` 崩溃是否消失、`[InstanceCulling][Verify]` 是否出现
