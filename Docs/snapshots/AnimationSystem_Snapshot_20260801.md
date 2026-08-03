# 动画系统现状快照 + 重构规划（2026-08-01）

> 状态：📋 现状记录（2026-08-01 会话末，用户指示"先记录现状，显然需要重构和规划一些内容才可行"）
> 关联：`Docs/architecture/animation/AnimationAsset.md`（.anim 资产规范）、`AnimationViewport.md`（动画视口）、`CharacterAsset.md`（.character 复合资产）、`SkinnedAnimation.md`（Game 端动画 System 设计）、`07_EngineAssetPipeline.md`（管线优先级）
> 结论先行：**功能代码已大量落地，但存在明显的结构性问题（单文件膨胀、渲染链路重复、复合资产聚合脆弱），需要一次重构规划后再继续推进**。

---

## 一、现状盘点（已落地内容）

### 1.1 资产转换侧（AssetTool）

| 文件 | 内容 | 状态 |
|:-----|:-----|:-----|
| `AssetTool/Core/AnimClipConverter.h/.cpp` | `anim2clip` 命令：FBX AnimStack → `.anim` 剪辑 | ✅ 编译通过、实测 64 组导出 |
| `Schemas/animation.schema.json` | .anim JSON Schema | ✅ |

### 1.2 引擎动画资产链路

| 文件 | 内容 | 状态 |
|:-----|:-----|:-----|
| `Engine/Resource/Struct/ClipHandle.h` | 剪辑句柄（index+generation） | ✅ |
| `Engine/Asset/IO/Loader/AnimLoader.h/.cpp` | `.anim` JSON → AnimationClip（按骨骼名匹配） | ✅ |
| `Engine/Resource/Manager/AnimationManager.h/.cpp` | 剪辑注册/查询/引用计数（仿 SkeletonManager） | ✅ |
| `Engine/Background/AnimationLoadTask.h` | 异步加载 .anim | ✅ |
| `AssetManager::LoadAnimation(path, boneNames, cb)` | .anim 专用加载入口 | ✅ |

### 1.3 角色复合资产（线 B）

| 文件 | 内容 | 状态 |
|:-----|:-----|:-----|
| `Engine/Resource/Struct/CharacterHandle.h` | 角色句柄 | ✅ |
| `Engine/Resource/Character/CharacterData.h` | mesh/skeleton/materials[]/clips 查表/defaultClip | ✅ |
| `Engine/Asset/IO/Loader/CharacterLoader.h/.cpp` | 解析 .character → 依赖清单（含材质槽位） | ✅ |
| `AssetManager.cpp` `AssetType::Character` 分支 | 骨架同步 + mesh/materials/clips 异步聚合 → CharacterData | ✅ |
| `Content/Models/KD-03/KD-03.character` | 示例：8 材质槽 + 8 剪辑 | ✅ |

### 1.4 动画视口（线 A）

| 功能 | 实现 | 状态 |
|:-----|:-----|:-----|
| 骨架/资产加载 | 扫描 Content/Models + Animations → Combo 选择 → LoadMesh/Skeleton/Clip | ✅ |
| 播放控制 | Play/Pause/Stop/Seek/Speed/Loop + `AdvancePlayback` + `SampleBoneTransforms` | ✅ |
| 骨骼树大纲 + 属性 | 左侧骨骼树（层级展开/选中）+ 右侧骨骼属性（父/矩阵） | ✅ |
| 骨骼缓冲 | `EnsureBoneBuffer`/`UploadBoneTransforms`（UPLOAD 堆每帧上传） | ✅ |
| 蒙皮预览 PSO | `Shaders/preview_skinned.hlsl` + `CreateSkinnedPreviewPSO`（单 RTV） | ✅ |
| 骨骼线框 | `Shaders/bone_lines.hlsl` + `DrawBoneLines`（父→子连线） | ✅ |
| RenderFrame | 蒙皮/静态双分支（骨架就绪→蒙皮，否则静态帧） | ✅ |

### 1.5 编译状态

- `DX12_Editor.exe` 21:25、`DX12Resource.lib` 21:24 已生成；`AssetManager.cpp` 最后 mutable lambda 修复于 21:21
- **最后一次编译是否通过待用户确认**（会话在此处被"先记录现状"打断）

---

## 二、结构性问题（为什么需要重构）

### 2.1 动画视口单文件膨胀（最突出）

```
AnimationViewportPanel.cpp  = 1196 行（+ .h 166 行）
```

职责混杂在一个类里：
- UI 层：工具栏/播放条/骨骼树/属性/状态栏
- 渲染层：RT 创建、骨骼缓冲、蒙皮 PSO、骨骼线框 PSO、RenderFrame 命令录制
- 资产层：扫描/加载/释放
- 播放逻辑：推进/采样

**问题**：任何一处改动都波及 1200 行文件；渲染上下文与 UI 状态耦合，难以单测/复用；`SkinnedRenderer`/`PreviewPBRRenderer` 已存在，但视口自建了一套 preview_skinned PSO，**渲染链路重复**。

### 2.2 蒙皮渲染链路重复

| 组件 | 现状 |
|:-----|:-----|
| `SkinnedRenderer`（引擎） | GBuffer PSO（4 RTV）+ 根签名（cbPass b1 / InstanceData t12 / BoneTransforms t13） |
| `preview_skinned.hlsl`（视口） | 单 RTV + 简化 CB（viewProj/光照）+ 相同蒙皮 VS 逻辑 |

两套蒙皮 VS 逻辑重复（skinned.hlsl vs preview_skinned.hlsl）；根签名约定不同（引擎 6 参数 vs 视口 2 参数）。**应统一：引擎蒙皮 VS 抽公共，视口复用**。

### 2.3 复合资产聚合脆弱

`AssetManager.cpp` Character 分支用 **lambda + 原子计数** 聚合依赖：

```cpp
auto onDep = [this, ...](...) mutable { ... pending->fetch_sub(1) == 1 ... };
```

- 依赖顺序硬编码（Skeleton 必须先于 Animation 同步加载）
- 错误处理缺失（某依赖失败 → 计数仍递减 → 半组装 CharacterData 回调 success=false）
- 逻辑堆在 `Load()` 的 switch 分支里，不可测

**应独立成 CharacterLoader 驱动的加载流程**（解析 → 依赖图 → 依赖回调 → 组装），AssetManager 仅分发。

### 2.4 动画数据源未统一（长期缺口）

| 层 | 现状 |
|:-----|:-----|
| 视口 | `SampleBoneTransforms` 面板内联：读 SkeletonData + AnimationClip → 合成最终矩阵（**自实现，未复用 SkeletonData::GetFinalTransforms**） |
| Game 端 | `SkeletonData::GetFinalTransforms(clipName, t)` 仍从骨架内置 `Animations` 取剪辑（M3D 遗留）；`.anim` 原子化后剪辑在 AnimationManager，**接口未改造（clipName → ClipHandle，见 AnimationAsset.md §5.3）** |
| 需求 | 视口/Game 共用同一套"ClipHandle 采样 → 骨骼矩阵"逻辑 |

### 2.5 资产侧遗留

- 第 45 组动画（`robo_BoostDash01`）在 Blender 导出环节丢失（原始 FBX 65 组 vs Blender 版 64 组）——待定以哪边为源
- 材质槽加载链路已实现（CharacterLoader → materials[slot]），但**消费端（渲染按 SubMesh 槽绑定材质）未验证**
- `.character` 无 schema（Schemas/character.schema.json 待补）

---

## 三、重构规划（建议顺序）

### 阶段 1：动画视口拆分（P1，解耦 1200 行单文件）

```
Editor/EditorLib/
  ├─ Panels/AnimationViewportPanel.h/.cpp     ← 只留 UI 层（工具栏/播放条/骨骼树/属性/状态栏）
  ├─ Animation/AnimationViewportRenderer.h/.cpp ← 渲染上下文（RT/骨骼缓冲/蒙皮 PSO/骨骼线框/RenderFrame）
  ├─ Animation/AnimationViewportState.h        ← 预览状态（资产句柄/播放参数/骨骼矩阵）
  └─ Animation/BoneLineRenderer.h/.cpp         ← 骨骼线框（从面板剥离）
```

- 面板持有 Renderer + State，Draw 只编排 UI；RenderFrame 委托给 Renderer
- 渲染器可被其他预览消费（后续 AssetPreviewSystem 复用）

### 阶段 2：统一蒙皮渲染（P1）

- 引擎 `skinned.hlsl` VS 抽公共蒙皮头（`SkinnedCommon.hlsl`），preview_skinned 复用
- 评估：视口直接复用 `SkinnedRenderer`（增单 RTV 预览 PSO）而非自建第二套

### 阶段 3：Character 加载流程独立（P1）

```
CharacterLoader（持有依赖图 + 回调）
  ├─ ParseCharacterFile → CharacterParseResult（已实现）
  ├─ 依赖图：Skeleton(同步) → [Mesh, Material[], Clip[]](异步)
  ├─ 依赖完成 → 组装 CharacterData → 回调
AssetManager::Load(Character) 仅分发，聚合逻辑移出 switch
```

- 补错误处理（依赖失败 → 立即失败回调 + 释放已加载 handle）
- 补 `Schemas/character.schema.json`

### 阶段 4：动画采样统一（P2，Game 端恢复前）

- `SkeletonData::GetFinalTransforms` 改造：新增 `GetFinalTransforms(ClipHandle, t, out)`（AnimationAsset.md §5.3）
- 视口 `SampleBoneTransforms` 改为调用统一接口（删除自实现）
- Game 端 AnimationAdvancer 复用同逻辑

### 阶段 5：资产侧收尾（P2）

- 第 45 组动画源定案（原始 65 组 vs Blender 64 组）
- 材质槽消费验证（视口/场景按 SubMesh 槽绑定 materialHandle）
- `.character` schema + 动画视口 `.character` 快捷加载

---

## 四、立即待办（下次会话）

1. **确认最后一次编译结果**（mutable lambda 修复后 DX12_Editor 是否通过）
2. 按阶段 1 拆分动画视口（当前最高优先——1200 行单文件不可持续）
3. 阶段 2 统一蒙皮渲染
4. 阶段 3 Character 加载独立化

---

## 五、相关文档

- `Docs/architecture/animation/AnimationAsset.md` — .anim 资产规范（已实现 §五 AnimationManager/AnimLoader）
- `Docs/architecture/animation/AnimationViewport.md` — 动画视口设计（大型引擎模式定案）
- `Docs/architecture/assets/CharacterAsset.md` — .character 复合资产（§六 结构/时序）
- `Docs/architecture/animation/SkinnedAnimation.md` — Game 端 AnimationAdvancer/StateMachine（长期）
- `Docs/snapshots/UKW_AssetPipeline_Snapshot_20260801.md` — 资产管线快照

---

## 六、会话结论（2026-08-01 会话末）

**下一阶段方向：基于重构，动画功能优先级降低。**

- 本会话动画/角色资产功能代码已大量落地（见 §一），但**结构性问题优先**（§二），需先重构再继续动画功能
- 重构优先级：① 动画视口拆分（1200 行单文件）→ ② 蒙皮渲染统一 → ③ Character 加载独立化 → ④ 动画采样统一（P2）→ ⑤ 资产侧收尾（P2）
- 已确认无大问题项：八叉树剔除 + 可见集检测链路完整；静态（Opaque）子网格渲染 + 材质槽消费正常；蒙皮侧材质槽（SkinnedRenderItemBuilder 只用 materialSlots[0]）为已知缺口，纳入重构
- 编译状态：最后一轮 mutable lambda 修复（AssetManager.cpp 21:21）是否通过待下次会话首查
