# BugFix: HZB 遮挡剔除 NDC→UV Y 轴翻转缺失（上半/下半恒定被剔 + 远处散落块）

> 日期：2026-08-13
> 涉及文件：`Shaders/InstanceCulling.cs.hlsl`
> 关联：`Docs/architecture/culling/S5_HZBOcclusion.md`（两趟 HZB 定案）、
> `Docs/architecture/rendering/AdaptiveFarPlane.md`（俯仰误剔处理）、
> `Docs/architecture/culling/CullingBlueprint.md` §6a（HZB 双时域消费）

---

## 现象

运行期（2026-08-13 实测）两个视觉异常：

1. **视野上半部分与下半部分恒定被剔除**——上下区域物体持续消失（画面上下缺失）；
2. **远处物体呈散落块状剔除**——远处内容不连续，块状缺失。

无 GPU 报错（纯逻辑错误，非资源/屏障问题）。

## 根因

**NDC→UV 转换缺少 Y 轴翻转**，违反 D3D 坐标语义：

| 坐标系 | Y 方向 | 说明 |
|:--|:--|:--|
| D3D NDC | **向上**（+1 = 屏幕顶部） | `cp.xy / cp.w ∈ [-1,1]` |
| 纹理坐标 V | **向下**（0 = 顶部，1 = 底部） | HZB 采样 `gHzb.Load` |

原实现 `uv = ndc * 0.5f + 0.5f` **未翻转 Y** → 上屏物体（NDC y>0）投影到 UV v>0.5（下屏）的 HZB 深度区域比较：

- **上半/下半恒定误剔**：上屏物体与下屏深度（如地面近深度）比较 → `objNear(远) >= hzbOccluder(近地面)` → 误剔；
- **远处散落块**：远处物体 AABB 投影足迹错位到错误 HZB 区域 → 部分剔除部分保留。

## 修复（`Shaders/InstanceCulling.cs.hlsl`）

```hlsl
// uvMin = 屏幕左上（x 取 ndcMin.x，y 取翻转后的 ndcMax.y=顶部）
// uvMax = 屏幕右下（x 取 ndcMax.x，y 取翻转后的 ndcMin.y=底部）
float2 uvMin = float2(ndcMin.x * 0.5f + 0.5f, 0.5f - ndcMax.y * 0.5f);
float2 uvMax = float2(ndcMax.x * 0.5f + 0.5f, 0.5f - ndcMin.y * 0.5f);
```

对齐 VTK（WebGPU Compute Occlusion Culler）与 Bevy（PR #17413）屏幕空间 AABB 测试的坐标语义。

## 验证

- ✅ 上半/下半正常渲染（不再恒定被剔）；
- ✅ 远处无散落块（连续剔除）；
- ✅ **HZB 剔除整体稳定**（用户确认"绝对的稳定性"）——Y 翻转是 HZB 剔除稳定性的关键前提。

## 同批次附带修复（同为 HZB 稳定性防御）

| 修复 | 内容 | 作用 |
|:--|:--|:--|
| **相机背后角点守卫** | 8 角点投影循环加 `cp.w <= 0` 跳过；全部角点在相机背后时跳过遮挡测试保守保留 | 消除坏角点污染 ndcMin/ndcMax/objNear → 远处跨近平面足迹错位 |
| **空数据/极端值防御** | 采样 `hzbOccluder ≥ 0.999`（全白）/ `≤ 0.001`（全黑）→ 跳过遮挡测试保守可见（不 return） | 打破"全白↔全黑"2 帧振荡（极端视角频闪） |
| **近裁剪面防御常量（2026-08-13）** | 写死 `kHZBNearPlane = 0.5f`（对齐 Camera.h NearPlane 上下限）；AABB 任一角点视空间 z（clip.w）≤ 阈值 → `nearClip=true` → 跳过遮挡测试保守保留 | 消除近处物体（贴近/穿过近裁剪面，投影不稳定）频闪——**进入物体内部由空数据防御兜底，贴近未进入由本防御兜底** |
| **SSR.hlsl Y 翻转（2026-08-13）** | `TraceReflection` 内 uv0/uvEnd 两处世界投影 clip→UV 加 Y 翻转（uvM/uvHit 由 uv0 派生自动继承） | 与 InstanceCulling 同根因——未翻转则反射步进采样 HZB/深度错位 → **全屏 SSR 异常**（用户提示确认） |

**SSR/water 坐标语义核查（2026-08-13）**：
- SSR.hlsl `TraceReflection` 世界投影 → UV：**已修复翻转**；
- SSR.hlsl 全屏 quad VS 的 `pin.UV`（`PosH.xy * 0.5 + 0.5`）：自洽约定（G-buffer 写入/合成采样一致），**不翻转**；
- water.hlsl 内嵌 `HZBScreenSpaceReflection`：已废弃（水改用 gSsrReflection 采样），无需改；
- water.hlsl 反射图采样 `screenUV = pin.PosH.xy / 尺寸`（SV_Position 派生，y 向下与纹理 V 一致）：**正确**。

**增强字段移除（2026-08-13）**：朴素两趟 HZB 定案后，`CullParams` 增强字段
（`gHzbMode`/`gPitchExpand`/`gHzbBias`/`gFoveaInner`/`gFoveaOuter`/`gNearPlane`）已从
HLSL cbuffer 与 C++ 结构体删除（布局 208B → **176B**），`DispatchCulling` 签名移除对应参数
（CullingRenderer.h/.cpp + CullingLayer.h/.cpp + Editor.cpp 调用点），HLSL 遮挡测试块
同时清理旧字段注释逻辑——仅保留朴素 AABB 投影 → mip → 4 采样比较 + 写死 `kHZBNearPlane`
近裁剪面防御。帧率回归（60→40-47 FPS）确认由多余字段填充的 CPU 端开销导致，移除后恢复。

---

## 经验总结

- **NDC→UV 转换必须显式翻转 Y**（D3D NDC Y 向上 vs 纹理 V 向下）——HZB 遮挡测试的坐标语义是稳定性的基础；
- 屏幕空间测试（包围盒投影 → mip → 采样比较）的**每个坐标环节**都需对照 VTK/Bevy 参考实现核对；
- 空数据（全白/全黑）必须**双端防御**（只防全白时全黑仍会振荡）。
