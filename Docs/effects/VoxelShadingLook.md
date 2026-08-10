# 体素型光照（Voxel-Shading Look）

> 日期：2026-08-05
> 状态：📋 调研记录（视觉缺陷根因 + 风格化利用思路）
> 关联：`Docs/effects/WaterRenderingTechniques.md`（水渲染）、`Docs/todos/archived/remaining_issues.md`（待办）
> 背景：city 场景地面出现"体素/马赛克"型光照——每块地面明暗不一致，形似体素游戏。

---

## 1. 现象

地面（mapChip）光照明暗呈**块状**——每个 30 单位的地面块独立明暗，块间边界生硬，视觉上像体素/马赛克。水面反射/光照正常（水面是程序化网格，法线平滑）。

---

## 2. 根因（已闭环）

### 2.1 数据层面

```
582 个 mapChip 地面实体，全部 scale=[-1,1,1]（X 镜像）
但 precomputed 烘焙：
  world[0] = 1.00（应为 -1，镜像丢失）
  worldInvTranspose[0] = 1.00（应为 -1）
```

### 2.2 渲染链路

```
MapSceneConverter 生成 mapChip 实体 scale=[-1,1,1]
  → 静态烘焙 precomputed 时 world/worldInvTranspose 未纳入 scale 负号
  → OpaqueRenderItemBuilder: cachedWorldInvTranspose 直接进 InstanceData
  → PBR 着色器: 法线经 worldInvTranspose 变换 → X 方向不翻转
  → 镜像块的表面法线方向错误 → 每块光照明暗不一致 → 体素型外观
```

**结论**：不是渲染器 bug，是**烘焙端（precomputed 生成）没吃 scale 负号**。`world[0]=1` 与 `worldInvTranspose[0]=1` 自洽，但都不反映镜像。

### 2.3 修复方向

| 方案 | 改动 | 说明 |
|:--|:--|:--|
| A（根治） | 烘焙端：world/worldInvTranspose 旋转部分纳入 scale 负号 | 需定位烘焙代码（AssetTool 或编辑器 SaveSnapshot） |
| B（兜底） | `ConstructEntity` 静态分支：precomputed 与 scale 不一致时按 transform.scale 重建 | 改 SceneConstructor.cpp |
| C（合并地面） | AssetTool flood fill 合并邻接同材质平面 mapChip → 程序化四边形 | **连带解决**：合并后法线统一，无独立镜像块（推荐主路径，见 §3） |

---

## 3. 与"合并地面"的关系

方案 C（合并邻接地面为程序化四边形）**连带解决体素型光照**：
- 合并后地面是少数程序化大四边形，法线统一向上，无 582 个独立镜像块
- 镜像烘焙错误只影响"独立块之间的明暗差异"——合并后没有块，问题消失
- 完全复用已验证的水管线（`procedural://grid` + 世界 UV 平铺 + 桶模式）

**合并条件**（待转换工具实现）：
- 邻接的 mapChip 格为**纯平面四边形**（无起伏，仅 4 顶点 2 三角）
- **材质和贴图一致**（同一 mapChip 子类型 + 同一材质槽）
- 合并后可用单个矩形程序化网格表达

---

## 4. 风格化利用（体素型游戏）

体素型光照可作为**风格化能力保留**，而非仅视为缺陷：

| 用途 | 实现方式 |
|:--|:--|
| 体素/像素风游戏 | 材质级 `FlatShading` 开关：PS 用 `ddx/ddy` 算面法线（或 face-normal），低分辨率光照 |
| Low-poly 风格 | 低细分网格 + 面法线 + 硬边光照 |
| 风格化水面 | 水面同样 flat shading → 像素风海洋 |

**架构**：PBR 着色器加 `FlatShading` 材质参数（0/1），`flatNormal = normalize(cross(ddx(WorldPos), ddy(WorldPos)))` 替代插值法线——写实场景关闭、体素游戏开启，一行开关。

---

## 5. 状态

- 根因已定位（烘焙丢失 scale 负号）
- 修复优先走"合并地面"路径（连带解决 + 性能收益）
- 风格化开关为长期可选项，待合并落地后评估

## 6. 参考资料

- 场景数据：`Content/City/City.scene.json`（mapChip 实体 + precomputed）
- 渲染链路：`Engine/Renderer/RenderItemBuilder/OpaqueRenderItemBuilder.cpp:174-175`（cachedWorldInvTranspose）
- 待办：`Docs/todos/archived/remaining_issues.md` N9（地面合并程序化化）
