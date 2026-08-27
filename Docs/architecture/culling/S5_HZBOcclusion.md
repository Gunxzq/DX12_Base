# 两趟 HZB 遮挡剔除设计（Two-Pass HZB Occlusion Culling）

> **状态（2026-08-27 更新）**：✅ **剔除能力完全正常化**（用户确认验证：运行时无异常；
> **City 稳定 60 FPS**、**City4（未做块分割处理）稳定 50 FPS**）——§七 人工编译验证（剔除部分）
> 与帧率复测两项就此关闭。⏸ **SSR 当前不可用**——HZB 消费端 SSR 暂不活跃（§4.3）；§七收尾项与
> `SSR_Todo.md` P3（水面倒影视角稳定性重验证）挂起待 SSR 恢复。
>
> 创建：2026-08-12。本设计为 GPU 驱动剔除的 L3 遮挡层方案——**确认当前架构即标准两趟
> （早期/晚期 HZB）形态，无需 PrePassDepth**。早期 HZB = 上一帧 HZB（Opaque 后构建），
> 本帧 PrePass 剔除直接消费；新进入视野物体天然安全（HZB 无记录区域采样 1.0 远值 → 不剔）。
> **阶段定位**：S5 GPU 剔除（L3 HZB 遮挡层）——CS 逐实例视锥 + 消费上一帧 HZB 遮挡测试；
> 阶段表见本目录 `README.md`
> 关联：`GPU-Drive.md` §6（HZB 双时域消费）、`InstanceCullingSystem.md`（剔除管道）、
> `AdaptiveFarPlane.md`（俯仰误剔处理）、`Docs/todos/SSR_Todo.md`（SSR 消费本帧 HZB）、
> `ShadowCullingGPUDriven.md`（GPU 驱动阴影剔除方案——阴影剔除复用同一 CullData/候选集）。
> 参考：Bevy PR #12899/#17413（两趟剔除 6 步流程）、VTK WebGPU Compute Occlusion Culler、
> Vovan675/RenderingEngine（两趟 HiZ + Meshlet）。

---

## 一、背景与问题

### 1.1 现状（2026-08-12 实测确认）

```
当前帧序（FrameDriver.cpp）：
  PrePass      ClearSystem + EditorInstanceCullingSystem（CS 剔除，消费【上一帧】HZB）
  Opaque       EditorOpaqueRenderSystem（ExecuteIndirect 渲染可见实例 → G-buffer + 当前帧深度）
  HZB_Build    EditorHzbBuildSystem（从本帧 Opaque 深度构建 HZB mip 链）
  → DynamicAOcclusion → Lighting（消费本帧 HZB）→ SSR（消费本帧 HZB）→ ...
```

**早期缺陷认知**：曾认为"CS 剔除消费上一帧 HZB → 相机快速旋转/俯仰/高度跃升时误剔"
需要 PrePassDepth（Bevy 步骤 1）重建当前视角深度。**2026-08-12 用户纠正：此认知错误**。

### 1.2 正确认知（两趟本质）

**两趟方案不需要 PrePassDepth**：

| 项 | 内容 |
|:--|:--|
| **早期 HZB** | = **上一帧 HZB**（上一帧 Opaque 后构建），本帧 PrePass 剔除直接消费 |
| **新进入视野物体** | 其屏幕区域在早期 HZB 中**无深度记录** → HZB 采样到默认值 **1.0（无限远）** → `ObjNear(如 0.5) < 1.0` → **不剔除**（仅做视锥剔除）→ 天然安全 |
| **晚期 HZB** | = 本帧 Opaque 后构建的 HZB（含本帧全部可见物体），供下一帧 PrePass 消费（即下一帧的早期 HZB） |

**结论**：当前架构（`PrePass 剔除消费上一帧 HZB → Opaque → HZB_Build`）本身就是标准
早期/晚期两趟方案。Bevy 步骤 1 的 PrePassDepth（用当前视角重建上帧可见物体深度）是
可选优化，本引擎**直接消费上一帧 HZB 即可**——新物体区域深度=1.0 远值，`ObjNear < 1.0`
不剔，正确性有保证。

### 1.3 防御措施状态（2026-08-13 终版：仅朴素两趟 HZB）

| 措施 | 作用 | 状态 |
|:--|:--|:--|
| P0 近平面相交跳过（`gNearPlane`） | 俯仰时跨近平面 AABB 保守保留 | ❌ **2026-08-13 已移除**（`gNearPlane` 字段删除；近裁剪面防御改由 HLSL 写死 `kHZBNearPlane = 0.5f` 承担） |
| P1 俯仰膨胀（`gPitchExpand`） | 按俯仰角放大采样足迹 | ❌ **2026-08-13 已移除**（`gPitchExpand` 字段与参数删除） |
| §7.3 相机联动三档（`gHzbMode`） | ΔAngle/ΔPos/ΔHeight → 正常/保守/禁用 | ❌ **2026-08-13 已移除**（`gHzbMode`/`gHzbBias` 字段与参数删除——两趟天然处理新物体，无需退让） |
| fovea 屏幕位置感知（`gFoveaInner/Outer`） | 中心精确、边缘保守/跳过 | ❌ **2026-08-13 已移除**（`gFoveaInner/Outer` 字段与参数删除） |

**结论**：仅保留朴素遮挡测试（AABB 投影 → mip → 4 采样比较）+ HLSL 写死常量 `kHZBNearPlane`
（近裁剪面防御）。增强字段已从 `CullParams`（176B ↔ HLSL 同步）与 `DispatchCulling` 签名
删除，文档保留供恢复参考。

---

## 二、开源参考（Bevy / VTK / Vovan675）

### 2.1 Bevy PR #12899 + #17413（两趟剔除标准 6 步，权威参考）

| 步骤 | 内容 | 本引擎对应 |
|:--|:--|:--|
| 1 | 渲染**上一帧可见**物体到深度缓冲（depth-only） | **不需要**（本引擎直接消费上一帧 HZB） |
| 2 | 降采样深度 → **早期 HZB（early）** | = 上一帧 HZB（Opaque 后构建） |
| 3 | 用早期 HZB 对所有 mesh 做遮挡测试（屏幕空间 AABB） | PrePass CS 剔除（消费上一帧 HZB）✅ |
| 4 | 可选 PrePass（用 (3) 结果） | 本引擎无独立 PrePass（G-buffer 即主渲染） |
| 5 | 主渲染（只画 (3) 判定可见物体） | Opaque（ExecuteIndirect）✅ |
| 6 | **再次降采样 → 晚期 HZB（late）** | HZB_Build（Opaque 后构建）✅ |

**关键**：步骤 6 的晚期 HZB 包含本帧新进入物体 → 下一帧早期 HZB 更完整——本引擎
`HZB_Build` 即此语义，已正确。

### 2.2 VTK WebGPU Compute Occlusion Culler（算法细节参考）

| 环节 | 做法 |
|:--|:--|
| 深度来源 | 上一帧可见对象 PrePass 渲染 → 当前帧 z-buffer（两趟；本引擎简化为直接消费上一帧 HZB） |
| mip 链 | `MipmapWidths` 逐级降采样（2×2 → 1 级） |
| 遮挡测试 | 包围盒投影 → 屏幕矩形 → 选 mip（矩形覆盖 2×2 区域）→ **4 次采样**取最大深度比较 |
| 首帧 | **渲染全部对象**填充 z-buffer（无上一帧可见列表时）——本引擎等价：HZB 初始化为远值 1.0（`InitializeContentToFar`，2026-08-13）→ `objNear < 1.0` 不剔 → 全画 |

### 2.3 Vovan675/RenderingEngine + orbit/HellTech/Unity VirtualMesh（GPU-Driven 共性）

| 共性 | 说明 |
|:--|:--|
| **两趟 HiZ** | 第一趟：上一帧 HZB 剔除幸存者 → 绘制 → 重建 HZB；第二趟：新 HZB 剔除"新来者"→ 合并绘制 |
| **meshlet 级剔除** | 视锥 + 背面（法线锥）+ 遮挡逐 meshlet 剔除（本设计**不做**，后续演进） |
| **SPD 降采样** | Granite hiz.comp 风格 SPD 替代手写 2×2（本引擎保持手写 CS，成本可忽略） |

---

## 三、架构定案（2026-08-12 终版：无需 PrePassDepth）

### 3.1 帧序（保持现状，无阶段重排）

```
PrePass（CS 剔除，消费【上一帧】HZB = 早期 HZB）
  → Opaque（ExecuteIndirect 渲染可见实例 → G-buffer + 当前帧深度）
  → HZB_Build（当前帧深度 → mip 链 = 晚期 HZB，供下一帧 PrePass + 本帧 SSR/接触阴影）
  → DynamicAOcclusion → Lighting → SSR → ...
```

### 3.2 早/晚两版 HZB 语义（单纹理，跨帧复用）

| HZB | 何时构建 | 深度来源 | 消费者 | 目的 |
|:--|:--|:--|:--|:--|
| **早期（early）** | 上一帧 HZB_Build | 上一帧 Opaque 深度 | 本帧 PrePass CS_Culling | 本帧遮挡测试（新物体区域=1.0 不剔） |
| **晚期（late）** | 本帧 HZB_Build | 本帧 Opaque 深度（含全部可见） | 下一帧 PrePass + 本帧 SSR/接触阴影 | 下一帧早期 HZB + 本帧效果 |

### 3.3 数据流（全 GPU 显存闭环，CPU 只发命令）

```
上一帧 HZB（HzbManager 纹理，mip 链）
   │
   ▼ PrePass CS_Culling（InstanceCulling.cs.hlsl：视锥 + HZB 遮挡测试）
可见实例列表（gAppend/gIndirectArgs）
   │
   ▼ Opaque（ExecuteIndirect → G-buffer + 深度图）
当前帧深度图
   │
   ▼ HZB_Build（手写 CS 2×2 降采样，现 HzbRenderer 复用）
本帧 HZB（下一帧早期 HZB + 本帧 SSR/接触阴影）
```

**关键正确性**：本帧新进入视野物体（上帧不可见）→ 早期 HZB 对应屏幕区域无深度记录
（=1.0 远值）→ `ObjNear(0.5) < HZBDepth(1.0)` → **不剔**，仅视锥剔除 → 不会误剔。

---

## 四、资源与数据流

### 4.1 单缓冲结论（2026-08-12 评估）

**无需 gAppend/gIndirectArgs 双缓冲**：FrameDriver 严格顺序提交（PrePass → Opaque 同队列
FIFO），帧 N 的 PrePass 读 gAppend 时，内容天然是帧 N-1 CS_Culling 写入的"上帧可见结果"；
帧 N CS_Culling 在其后覆盖为帧 N 结果，Opaque 再读——无竞争、无 in-flight 冲突。
（文档初稿的"双缓冲"是过度设计，已废弃。）

### 4.2 HZB 纹理

| 项 | 方案 |
|:--|:--|
| 资源 | `HzbManager` 单一纹理（R32_FLOAT mip 链），每帧构建一次（Opaque 后） |
| 屏障 | 规则 10 对称：COMMON→SRV/UAV→COMMON（现 HzbRenderer 已实现） |
| 首帧 | ✅ 初始化纹理兜底（2026-08-13）：HZB 创建/重建时清为远值 1.0 → `objNear < 1.0` 不剔（全画，仅视锥）；非"gHzbMode=2"（三档已注释停用） |

### 4.3 与现有 HZB 消费者的关系（不变）

| 消费者 | 版本 | 说明 |
|:--|:--|:--|
| CS_Culling（PrePass） | 上一帧 HZB（早期） | 核心消费方 ✅ |
| SSR（Lighting 后） | 本帧 HZB（晚期，Opaque 后构建） | 含完整深度 ✅（2026-08-27：SSR 当前不可用，此消费者暂不活跃） |
| 接触阴影（未来） | 本帧 HZB | 同 SSR 语义 |

---

## 五、与现有策略的关系（2026-08-13 更新）

| 策略 | 状态 | 说明 |
|:--|:--|:--|
| §7.3 相机联动三档（`gHzbMode`） | ❌ **已移除** | 两趟天然处理新物体（HZB 区域=1.0 不剔），无需相机运动退让；字段/参数从 CullParams 与 DispatchCulling 删除 |
| P1 俯仰膨胀（`gPitchExpand`） | ❌ **已移除** | 实测误判率高；字段/参数删除 |
| fovea 屏幕位置感知（`gFoveaInner/Outer`） | ❌ **已移除** | 实测误判率高；字段/参数删除 |
| P0 近平面跳过（`gNearPlane`） | ❌ **已移除** | 实测误判率高；近裁剪面防御改由 HLSL 写死 `kHZBNearPlane = 0.5f` |

**预期效果**：仅朴素两趟 HZB——AABB 投影 → mip → 4 采样比较 + 写死近裁剪面常量，
剔除逻辑简化，新物体天然不误剔。

---

## 六、边界情况

| 场景 | 处理 |
|:--|:--|
| **首帧** | **✅ 2026-08-13 初始化纹理兜底**：HzbManager 创建/重建时同步初始化 HZB 内容为远值 1.0（`InitializeContentToFar`——**UpdateSubresources 上传全 mip 链填 1.0**，Flush 同步；2026-08-13 弃用 ClearUnorderedAccessViewFloat：其 CPU handle 必须指向 CPU-only 堆，shader-visible 堆 CPU 视图=CPU-write-only 驱动读取无效 #646）——HZB 未被构建时内容 = 1.0（远）→ `objNear < 1.0` 不剔 → 天然不误剔（对齐 BlankTextureProvider 无效默认值纹理模式） |
| **场景切换** | ✅ 同首帧：HzbManager 重建（Initialize/OnResize）自动再次初始化为远值 1.0——新场景首帧 HZB 无效内容不会误剔 |
| **非 2 次幂窗口** | mip 降采样尺寸 `max(1, size>>mip)` 防越界（HzbRenderer 已处理，对齐 Bevy #14062 教训） |
| **相机静止** | 早期 HZB = 晚期 HZB（无新物体）→ 剔除结果稳定，无闪烁 |

**实现**：`HzbManager::Initialize` 新增 `CommandManager*` 参数（Editor 传入），`BuildResources` 末尾调用
`InitializeContentToFar()`——每级 mip `ClearUnorderedAccessViewFloat(1.0)` + `Flush` 同步阻塞
（对齐 BlankTextureProvider 同步语义）。Game 端未传 cmdMgr 时跳过（调用方保证不消费 HZB）。

---

## 七、实施记录（2026-08-12）

- [x] **调研**：Bevy PR #12899/#17413 6 步流程 + VTK 算法 + Vovan675 共性（§二）；
- [x] **定案**：无需 PrePassDepth——早期 HZB = 上一帧 HZB，新物体天然不误剔（§1.2/§三）；
- [x] **阶段保持**：RenderPhase.h / FrameDriver.cpp 保持 `PrePass → Opaque → HZB_Build` 原序
      （曾误加 PrePassDepth/HZB_Build_Early/CS_Culling/HZB_Build_Late 拆分，已回退）；
- [x] **三档联动注释停用**：Editor.cpp `EditorInstanceCullingSystem` 三档判定注释保留，
      `hzbMode` 恒 0（正常档）；
- [x] **增强项全部注释停用**（实测误判率高，代码保留供恢复）：P0 近平面跳过 / P1 俯仰膨胀 /
      fovea 屏幕位置感知——HLSL `InstanceCulling.cs.hlsl` 遮挡测试块仅保留朴素
      AABB 投影 → mip → 4 采样比较；Editor.cpp 传参全部置 0（`0.0f, 0.0f, 0.0f`）；
- [x] **首帧/场景切换兜底（2026-08-13）**：HzbManager 创建/重建时同步初始化 HZB 内容为远值 1.0
      （`InitializeContentToFar`——**UpdateSubresources 上传全 mip 链填 1.0** + Flush 同步，对齐
      BlankTextureProvider 无效默认值纹理模式；弃用 ClearUnorderedAccessViewFloat——其 CPU handle
      必须指向 CPU-only 堆，shader-visible 堆 CPU 视图=CPU-write-only 驱动读取无效 #646）——
      HZB 未被构建时内容 = 1.0（远）→ `objNear < 1.0` 不剔 → 全画；场景切换重建自动再次初始化（§六）；
- [x] **运行时修复一：NDC→UV Y 轴翻转（2026-08-13）**：D3D NDC Y 向上（+1=屏幕顶）、纹理 V 向下
      （0=顶 1=底），原实现 `uv = ndc * 0.5 + 0.5` 未翻转 → 上屏物体投影到下屏 HZB 深度区域比较 →
      **上半/下半恒定误剔**（远处散落块同源）。修复：`uvMin = (ndcMin.x*0.5+0.5, 0.5-ndcMax.y*0.5)`、
      `uvMax = (ndcMax.x*0.5+0.5, 0.5-ndcMin.y*0.5)`（对齐 VTK/Bevy 屏幕空间 AABB 测试语义）；
- [x] **运行时修复二：相机背后角点守卫（2026-08-13）**：8 角点投影循环加 `cp.w <= 0` 跳过
      （坏角点污染 ndcMin/ndcMax/objNear → 远处物体跨近平面足迹错位 → 散落误剔）；全部角点
      在相机背后时跳过遮挡测试保守保留（对齐 UE/Bevy 保守 bounds，非 P0 的"整体跳过"）；
- [x] **空数据/极端值防御（2026-08-13）**：HZB 采样极端值（`hzbOccluder ≥ 0.999` 全白 /
      `≤ 0.001` 全黑）→ 跳过遮挡测试保守可见（不 return，落到存活逻辑）——打破"全白↔全黑"
      2 帧振荡（全白：objNear<1.0 恒成立→全可见；全黑：objNear>0.0 恒成立→全剔除）。
      对齐 UE/Unity 空数据防御；注意**必须双端防御**（只防全白时全黑仍会振荡）；
- [x] **近裁剪面防御常量（2026-08-13）**：写死 `kHZBNearPlane = 0.5f`（对齐 Camera.h NearPlane
      上下限）；AABB 任一角点视空间 z（clip.w）≤ 阈值 → `nearClip=true` → 跳过遮挡测试保守保留——
      消除近处物体（贴近/穿过近裁剪面，投影不稳定）频闪；进入物体内部由空数据防御兜底，
      贴近未进入由本防御兜底（详见 BugFix_HZB_NDC_UV_YFlip.md）；
- [x] **SSR.hlsl Y 翻转（2026-08-13）**：`TraceReflection` 内 uv0/uvEnd 两处世界投影 clip→UV 加
      NDC→UV Y 翻转（uvM/uvHit 由 uv0 派生自动继承）——与 HZB 剔除同根因，修复"全屏 SSR 异常/
      颠倒"；`kSsrCompositeEnabled=true` 重新打开验证（SSR 合成，见 SSR_Todo.md / SSR 快照）；
- [x] **增强字段移除（2026-08-13）**：朴素两趟 HZB 定案后，`CullParams` 增强字段
      （`gHzbMode`/`gPitchExpand`/`gHzbBias`/`gFoveaInner`/`gFoveaOuter`/`gNearPlane`）从 HLSL
      cbuffer 与 C++ 结构体删除（布局 208B → **176B**），`DispatchCulling` 签名移除对应参数
      （CullingRenderer.h/.cpp + CullingLayer.h/.cpp + Editor.cpp 调用点），HLSL 遮挡测试块
      清理旧字段注释逻辑——仅保留朴素 AABB 投影 → mip → 4 采样比较 + 写死 `kHZBNearPlane`。
      帧率回归（60→40-47 FPS）确认为多余字段填充 CPU 端开销，移除后恢复（§1.3/§五）；
- [x] **人工编译验证**（项目规则 AI 不编译；**2026-08-27 用户确认**：运行时无异常、剔除能力完全正常化——
      旋转/俯仰/高度跃升新物体不误剔、俯视远处物体不闪烁 ✅；⏸ SSR 合成（Y 翻转后）与 RenderDoc 确认
      HZB 内容待办——SSR 当前不可用）；
- [x] **帧率复测**（2026-08-13 诊断；**2026-08-27 关闭**：City 稳定 60 FPS、City4（未做块分割处理）
      稳定 50 FPS，帧率下降未复现）。原记录：City 场景帧率下降 60→40-47 FPS，疑为候选实例数激增
      （Verify total 354~467→1170~1404）——剔除率正常（41.7~64.4% vs 基线 28.8~65.1%），
      非 HZB/SSR 回归（详见 SSR_Todo.md 帧率诊断记录）；
- [ ] **验证后收尾**：若水面倒影视角稳定性恢复，关闭 SSR_Todo.md P3。
      （2026-08-27：⏸ 挂起——剔除能力已完全正常化，但 SSR 当前不可用，水面倒影视角稳定性
      （依赖 SSR）重验证挂起，SSR_Todo.md P3 保持开放）

---

## 八、待决策项（远期记录）

1. **meshlet 级两趟剔除**（Vovan675/orbit 形态）：当前实体级剔除结构对齐，meshlet 拆分留待
   GPU-Driven 深度演进（`GPU-Drive.md` §L2b 后续）；
2. **SPD 替换手写降采样**：Bevy 待办（Granite hiz.comp）；当前手写 CS 足够，成本已证可忽略；
3. **阴影视图遮挡**：Bevy 明确"planned but not in this patch"——本设计同样不包含，记录；
4. **PrePassDepth（Bevy 步骤 1）——后续优化，非渲染步骤**：其本质是**基于上一帧 HZB +
   上一帧相机矩阵**，用上一帧可见列表 depth-only 重绘，生成"当前视角"的早期 HZB——
   目的是**优化早期 HZB 质量**（让遮挡深度更贴近当前视角，而非依赖上帧深度直接采样）。
   本引擎当前直接消费上一帧 HZB（新物体区域采样 1.0 远值 → 不剔，天然安全）已满足正确性；
   PrePassDepth 是**质量优化项**（缩小上帧 HZB 与当前视角的差距），**后续需要时再引入**，
   不在当前实施范围。

---

## 参考

- Bevy PR #12899：`Add an optional pass that generates a hierarchical Z-buffer`
  （6 步流程原文：prev-visible render → early HZB → cull → prepass → main → late HZB）
- Bevy PR #17413：`Implement experimental GPU two-phase occlusion culling for the standard 3D mesh pipeline`
  （early/late preprocess phase、work_item_buffer、Bistro 1591→585）
- VTK：`WebGPU Occlusion Culling in VTK`（Kitware 2024-08-29，Tom Clabault）——两趟 + 4 采样比较 + 首帧全量
- Vovan675/RenderingEngine / Thefefe/orbit / Eichenherz/HellTech-Engine / Unity VirtualMesh：
  两趟 HiZ + meshlet + SPD 降采样（Granite hiz.comp）共性参考
- 本项目：`GPU-Drive.md` §6/§7、`InstanceCullingSystem.md`、`AdaptiveFarPlane.md`、`BugFix_L2c_InstanceCulling_ContentCorruption.md`
