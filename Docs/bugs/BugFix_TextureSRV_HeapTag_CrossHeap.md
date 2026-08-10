# BugFix: 纹理 SRV 跨堆错配（Default vs EditorViewport）导致 gTextureMaps 越界

> 日期：2026-08-03
> 涉及文件：`TextureLoadTask.h`、`AssetManager.h`、`Loaders/TextureLoader.cpp`、`SceneConstructor.cpp`
> 关联规则：`.atomcode.md` #17（编辑器多堆模式下 `GetPartitionCpuHandle` 必须显式传入 `HeapTag`）

---

## 现象

编辑器加载场景（mini_city 验证场景，3 实体 4 材质 5 纹理）后，GPU-Based Validation 报纹理数组越界：

```
space2, 0: gTextureMaps[16381] WindowFrameResources_GBufferWorldPos Texture 2D 980 546 1 1 R16G16B16A16_FLOAT
```

- `gTextureMaps[]` 是 `register(t0, space2)` 的无界纹理数组（`Common_PBR.hlsl:37`），PS 按 `matData.BaseColorTexIndex` 等字段索引采样
- 索引 **16381 = 16384 − 3**：纹理分区（`base=0, size=16384`）**末尾倒数第 3 槽**，指向的却是 Editor 视口的 G-buffer WorldPos 纹理（`WindowFrameResources_GBufferWorldPos`，980×546 视口尺寸）

## 根因

**纹理 SRV 创建与渲染绑定的描述符堆不一致（跨堆错配）**，违反项目规则 #17：

| 端 | 调用 | 堆域 |
|:---|:-----|:-----|
| 渲染绑定 | `Editor.cpp:979` `GetPartitionGpuHandle(Texture, 0, HeapTag::EditorViewport)` | **EditorViewport** |
| 纹理加载 | `TextureLoadTask.h` `Allocate(PartitionType::Texture)`（旧接口） | **Default**（默认参数） |
| 纹理 SRV 写入 | `TextureLoadTask.h` `GetPartitionCpuHandle(Texture, srvIndex)`（旧接口） | **Default**（默认参数） |

链路：材质 buffer 中 `baseColorTextureId` 存的是 **Default 堆**的 srvIndex（来自 `TextureManager::GetSRVIndex`）→ shader 用该索引在**EditorViewport 堆**上寻址 → 索引错位，落到 EditorViewport 堆 Texture 分区末尾附近未初始化槽位（恰为 GBufferWorldPos 的 SRV）→ PS 采样越界。

Game 端单堆模式（`HeapMode::Single`，所有 tag 映射 Default）不暴露此问题；编辑器多堆模式（`HeapMode::Multi`）Default 堆与 EditorViewport 堆物理隔离，因此复现。

## 修复

**方案定案：heapTag 作为 task 参数传递，由场景构建器（SceneConstructor）分发任务时同步。**

| 修复 | 文件 | 改动 |
|:-----|:------|:------|
| task 增加 heapTag 参数 | `Engine/Background/TextureLoadTask.h` | `Create()` 新增 `HeapTag heapTag = Default` 参数；`Allocate`/`GetPartitionCpuHandle` 改用显式 tag；`onComplete` lambda 捕获 heapTag |
| AssetManager 持有 heapTag | `Engine/Resource/AssetManager/AssetManager.h` | 新增 `SetHeapTag/GetHeapTag` + `m_heapTag` 成员（默认 Default）；补 include `DescriptorHeapCollection.h`（HeapTag 完整枚举定义，前向声明不含枚举） |
| loader 传递 | `Engine/Resource/AssetManager/Loaders/TextureLoader.cpp` | `TextureLoadTask::Create(..., m_heapTag)` 传入 |
| 场景构建器分发 | `Engine/Scene/SceneConstructor.cpp` | `LoadScene` 提交 LoadBatch 前 `AssetManager::GetInstance().SetHeapTag(m_heapTag)` |

传递链：`SceneConstructor::LoadScene(heapTag)` → `AssetManager::SetHeapTag` → `LoadTextureImpl` → `TextureLoadTask::Create(heapTag)` → SRV 与渲染端同堆（编辑器传 `EditorViewport`）。

**兼容性**：Game 端单堆模式所有 tag 路由 Default，`m_heapTag` 默认值即 Default，行为不变。

## 验证结果

- 重新编译 AssetTool 侧不受影响（纯引擎改动）
- 预期：`gTextureMaps[16381]` 越界报错消失；mini_city 场景中 asfa2/sea/build03/build085_2 纹理正常显示
- 验证方式：Editor → AssetBrowser 打开 `Content/Scenes/mini_city.scene.json` → 观察视口纹理 + Console 无 GPU-BV 越界

## 待排查（同类风险，本次未改）

以下路径直接调 `AssetManager::Load`（不走 `LoadScene`，不会触发 `SetHeapTag`），若加载纹理且最终渲染绑定 EditorViewport 堆，存在同类跨堆风险：

| 调用点 | 用途 |
|:-------|:-----|
| `Editor/EditorLib/Panels/AssetBrowser.cpp:753/797/837` | 预览/图标加载 |
| `Editor/EditorLib/Panels/AnimationViewportPanel.cpp:619` | 动画视口资源加载 |

---

## 相关代码位置

- `Engine/Background/TextureLoadTask.h` — `Create()` 签名（L40）、`onComplete` SRV 分配（L150-176）
- `Engine/Resource/AssetManager/AssetManager.h` — `SetHeapTag/GetHeapTag`、`m_heapTag` 成员
- `Engine/Resource/AssetManager/Loaders/TextureLoader.cpp` — `LoadTextureImpl`（L52-53）
- `Engine/Scene/SceneConstructor.cpp` — `LoadScene` 分发（L117-118）
- `Engine/Resource/Core/DescriptorHeapCollection.h` — `HeapTag` 枚举（L32）、`Allocate(tag, partition)`（L97）、`GetPartitionCpuHandle(partition, index, tag)`（L92）
- `Shaders/Common_PBR.hlsl` — `gTextureMaps[] : register(t0, space2)`（L37）
