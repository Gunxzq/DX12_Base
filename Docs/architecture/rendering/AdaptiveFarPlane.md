# 自适应远平面 (Adaptive Far Plane)

> 状态（2026-08-26 确认）：✅ 第一层双视锥体已落地（早期实现，当前运行中）；
> ❌ 第二层空间哈希自适应未启动——当前无动态 FarPlane 调整（FarPlane 维持固定 `2500` 上限）。
> 术语更正（2026-08-26）：本文"八叉树"均为早期设计命名——空间索引实现已更名为
> `SpatialHashGrid`（均匀格子哈希，非八叉树，见 `S1_SpatialCulling.md` 改名注记）。

## 问题背景

投影矩阵的远平面固定为一个较大的值（当前上限 `2500.0`）时，场景中物体的深度值会被压缩到映射区间的极小范围内，导致：
- 深度纹理精度不足（影响 SSAO、Shadow Map 等依赖深度采样的特性）
- 深度缓冲的 Z-fighting 风险增加
- 深度可视化调试困难

## 解决方案

整体方案分为两层：**双视锥体**（基础架构）和**空间哈希自适应**（进阶优化，早期设计称"八叉树自适应"）。

---

### 第一层：双视锥体 (Dual Frustum)

深度精度与剔除范围存在冲突：
- 深度缓冲远平面需紧贴场景有效物体以保证精度
- 但 CPU 剔除需要更大范围，避免远平面外物体进出时突现/突隐

维护**两个不同的远平面**：

| 视锥 | 用途 | 远平面 | 说明 |
|------|------|:------:|------|
| **渲染视锥** (Render Frustum) | 投影矩阵、深度缓冲、SSAO、Shadow Map | 上限 `2500`（推荐默认） | 实际画图边界，紧贴场景保证深度精度 |
| **剔除视锥** (Cull Frustum) | CPU/L2 GPU 剔除、可见性判断 | 上限 `4500`（推荐默认） | 大范围，确保物体进出平滑（> 渲染，边界安全防 pop-in） |

> **定案（2026-08-12）**：`2500 / 4500` 为**上限 + 推荐默认值**，具体场景（室内/开放世界）可按需调小——见 `Camera.h` / `CameraManager.cpp` / `EditorSceneManager::InitCameraConfig`。

#### 核心逻辑

```
每一帧:
  1. 用 Cull Frustum 做 CPU 视锥剔除 (宽范围)
  2. 用 Render Frustum 设置投影矩阵 (紧范围)
  3. 被 Cull 剔除的物体不提交绘制命令
  4. 物体进入 Render Frustum 范围后才实际参与渲染
```

#### 优势

- 深度缓冲精度不受剔除范围影响
- 物体进出画面边缘不会突现/突隐
- SSAO、Shadow Map 等深度相关特性受益于紧贴的远平面
- 剔除逻辑与投影矩阵解耦

---

### 第二层：基于空间哈希的自适应远平面

> 术语：早期设计为"基于八叉树"；现行空间索引为 `SpatialHashGrid`（均匀格子哈希，
> 非八叉树——2026-08-10 自 `OctreeSystem` 改名，见 `S1_SpatialCulling.md` 改名注记）。
> 空间分区本身已就绪（剔除层粗筛在用），本层只需新增"视锥内最远物体"查询。

在第一层双视锥体的基础上，进一步将 Render Frustum 的远平面从固定值改为**动态计算**。

#### 流程

1. **空间哈希分区** — 场景中所有物体按包围盒插入空间哈希网格（均匀格子哈希，已在剔除层粗筛中使用）
2. **帧级查询** — 从相机位置出发，沿视锥方向查询空间哈希网格，找到距离相机**最远**的可见物体
3. **动态远平面** — 以该距离为基础（加上少量余量）作为 Render Frustum 的 FarPlane

#### 边界情况

| 场景 | 处理方式 |
|------|----------|
| 视野内无物体 | 使用兜底远平面（如 `1000.0` 或场景最大范围） |
| 帧间远平面跳变 | 做指数平滑 `FarPlane = lerp(FarPlane, targetFarPlane, 0.1f)` |
| 空间哈希查询性能 | 开销与视锥交叠格子数相关，量级可控（均匀网格哈希查询） |
| 近平面配合 | 当前固定 `0.5`，后续可考虑同理自适应 |

---

## 影响范围

| 模块 | 影响 |
|------|------|
| `CameraManager` | 需维护 `renderFarPlane`、`cullFarPlane` 两个远平面 |
| `Frustum` | 支持以不同的远平面构建两个视锥体 |
| `SpatialHashGrid`（空间哈希网格，现名） | 提供"视锥内最远物体距离"查询接口 |
| `FrameDriver` | 需要在场景剔除阶段后插入更新回调 |
| `Renderer` (投影矩阵) | 使用 Render Frustum 构建投影矩阵 |
| SSAO / Shadow Map / 深度剔除 | 更新后的深度纹理自动受益，无需修改 |

## 优先级（⚠️ 过时——以文末「优先级（2026-08-26 更新）」为准）

1. **双视锥体** — 基础架构，建议优先实现
2. **八叉树自适应** — 进阶优化，在 SSAO 功能稳定后推进

## 状态

### 已实现

- **双视锥体 (Dual Frustum)** — 基础架构已就绪

| 组件 | 实现状态 | 说明 |
|------|:--------:|------|
| `Camera::CullFarPlane` | ✅ | 剔除远平面，默认/上限 `4500.0`（2026-08-12 定案） |
| `Camera::FarPlane` | ✅ | 渲染远平面，默认/上限 `2500.0`（原字段，语义变为渲染远平面） |
| `PredictedCameraData::CullFarPlane` | ✅ | 预测数据携带双远平面 |
| `CullingSystem::m_cullFrustum` | ✅ | 剔除视锥（宽范围，Builder 用） |
| `CullingSystem::m_renderFrustum` | ✅ | 渲染视锥（紧范围，SSAO/深度计算用） |
| `CullingSystem::GetRenderFrustum()` | ✅ | 新增接口 |
| `Camera::CalculateMatrices()` | ✅ | 投影矩阵自动用 `FarPlane=2500`（紧） |

#### 当前数据流

```
每一帧:
  1. CullingCameraUpdate → GetPredictedCameraData() → FarPlane=2500, CullFarPlane=4500
  2. CullingSystem::SetCamera() → 构建两个视锥体
  3. BuilderUpload → GetFrustum() → 剔除视锥（宽）做 CPU/L2 GPU 剔除
  4. PassConstants → Camera::ProjMatrix → 渲染视锥（紧）设投影矩阵
  5. SSAO/Shadow Map → 使用紧远平面的深度缓冲，精度受益
```

#### 后续待实现

- **动态渲染远平面** — 当前 `FarPlane=2500` 仍为固定上限值
  - 需引入空间哈希查询最远可见物体距离
  - 动态计算 `targetFarPlane`（≤ 2500 上限），替代固定值
- **帧间平滑插值** — `FarPlane = lerp(FarPlane, targetFarPlane, 0.1f)` 防止跳变
- **空间哈希"最远物体"查询接口** — 空间分区已就绪（`SpatialHashGrid`，均匀格子哈希，剔除层粗筛在用）；待新增的是沿视锥方向求"最远可见物体距离"的查询接口

---

## 归档：HZB 俯仰角误剔处理（2026-08-12，P0/P1 均已移除）

> 归档说明（2026-08-26）：P0/P1 方案均已移除，本节保留为历史记录。
> **当前方案**：朴素两趟 HZB（`S5_HZBOcclusion.md`）——极其简单且精准地完成遮挡剔除
> （新进入视野物体其屏幕区域 HZB 无深度记录 → 采样远值不剔，天然安全）。
> 近裁剪面防御由 HLSL 写死常量 `kHZBNearPlane = 0.5f` 承担（对齐 Camera.h NearPlane）。

**背景**：HZB 遮挡剔除在**俯仰角大**时误剔率偏高——俯视时屏幕大部分是地面（近深度），远处物体的 AABB 投影落在近地面区域 → `objNear(远) >= hzbOccluder(近地面)` → 误剔可见物体。

**P0（❌ 2026-08-13 已移除，文档保留供恢复参考）——近平面相交跳过（对齐 UE）**：
- `InstanceCulling.cs.hlsl`：AABB 8 角点投影循环中，任一角点 `cp.w <= gNearPlane`（视空间 z ≤ 近平面）→ `aabbValid=false` 跳过测试（保守保留）；
- 依据 UE 官方实践："occlusion query is not being run for bounds that intersect near clipping plane"——俯仰角大时远处物体 AABB 易跨近平面，正是俯仰误剔的根因解药；
- 配套：`CullParams` 曾加 `gNearPlane` 字段（C++/HLSL 同步 192B），`EditorInstanceCullingSystem` 传 `cam.NearPlane`；
- **2026-08-12 注释停用（实测误判率高）→ 2026-08-13 已移除**：`gNearPlane` 字段从 CullParams 删除（近裁剪面防御改由 HLSL 写死常量 `kHZBNearPlane = 0.5f` 承担，对齐 Camera.h NearPlane）；DispatchCulling 签名不再传 `cam.NearPlane`。

**P1（❌ 2026-08-13 已移除，文档保留供恢复参考）——俯仰角相关屏幕膨胀**：
- 俯仰角大时按角度放大 HZB 采样足迹的屏幕膨胀量，吸收一帧俯仰位移（对齐 UE "fast camera turns" 场景的 bounds scale 增大思路）；
- 实现：`InstanceCulling.cs.hlsl` 按 CPU 传入的 `gPitchExpand`（像素量，Editor.cpp 每帧从 `asin(Forward.y)` 俯仰角线性计算，0.3px/度、上限 16px）膨胀 `minPx/maxPx` 包围盒；
- 配套：`CullParams` 曾加 `gHzbMode`（0 正常/1 保守/2 禁用）+ `gPitchExpand` + `gHzbBias`（保守偏置 0.01）字段（C++/HLSL 同步 192B）；
- **2026-08-12 注释停用（实测误判率高）→ 2026-08-13 已移除**：`gHzbMode`/`gPitchExpand`/`gHzbBias` 字段从 CullParams 删除、DispatchCulling 签名移除对应参数——仅保留朴素两趟 HZB
  （`S5_HZBOcclusion.md`：早期 HZB = 上一帧 HZB，新进入视野物体其屏幕区域 HZB 无深度记录 →
  采样 1.0 远值 → `ObjNear < 1.0` 不剔，天然安全）。

---

## 优先级（2026-08-26 更新）

1. ✅ **双视锥体** — 已落地（早期实现，当前运行中）
2. ❌ **空间哈希自适应** — 未启动（2026-08-26 确认：当前无动态 FarPlane 调整，FarPlane 维持固定 `2500` 上限；SSAO 功能稳定后推进）
