# 调试线框渲染（WireframeManager）设计定案

> 状态：📋 设计定案（2026-08-01）
> 关联：`EngineOverview.md` §9（ECS 唯一数据源与 Manager 分工）、`SceneSnapshot.md` §2.2（运行时唯一数据源）、`ComponentEditorSystem.md` §十一/十二（Gizmo 绘制/计算分离）、`AssetPreviewSystem.md`（FrameScratchAllocator 复用先例）

---

## 一、背景与目标

编辑器视锥体/AABB 线框目前由 `EditorGizmoSystem` 内联绘制（ImDrawList 屏幕空间投影），存在两个问题：

1. **投影翻转伪影**：屏幕空间投影（`XMVector3TransformCoord` 除 w）在角点位于观察相机后方（w<0）时交叉成"双 Z"，大型引擎均使用 3D 线框几何体而非屏幕空间投影。
2. **Editor 私有**：Game 端运行时调试（DrawDebugLine）无此能力。

目标：**引擎级调试线框渲染子系统**，Game 调试 + Editor 共用，对齐大型引擎（Unity `Debug.DrawLine` / Unreal `DrawDebugLine`）的"声明式收集 + 集中渲染"模式。

## 二、架构决策

### 2.1 管理器模式，不做 ECS 组件

依据 `EngineOverview.md` §9.5 判断准则：

| 准则 | 答案 | 结论 |
|:-----|:-----|:-----|
| 数据是否对应场景"物体"（灯/相机/水面）？ | 否——调试线是**瞬态命令**，非持久实体 | **不做 ECS 组件** |
| 多个实体的数据需聚合为连续 GPU buffer？ | 是——Gizmo + Game 调试的线聚合为线列表 | **需要 Manager** |
| 管理 GPU 资源（顶点/上传缓冲）？ | 是 | **需要 Manager** |

**Manager 职责边界**：`LightManager` 等管理器因属性卡（ECS 驱动）需求改为 ECS 化；调试线**无属性卡需求**（瞬态、每帧重建），保持纯管理器模式不冲突，且避免瞬态数据污染 ECS Registry（`SceneSnapshot.md` §2.2 唯一数据源纯度）。

### 2.2 收集与录制分离

```
数据声明（无 GPU、无顺序敏感性）            固定录制点（每帧一次，顺序确定）
┌────────────────────────────┐   ┌─────────────────────────────────┐
│ EditorGizmoSystem::DrawGizmo│   │ WireframeManager::UpdateAndUpload│
│   → AddLine/AddAABB(...)   │   │   ├─ 上传 CPU 线列表 → 动态 VB    │
│ Game 调试 system            │──→│   ├─ SetPSO（GS 展开 + 深度）     │
│   → AddLine(...)           │   │   └─ Draw（LINELIST）            │
└────────────────────────────┘   └─────────────────────────────────┘
```

- `Add*` 只写 CPU 侧临时列表，任何调用者安全、顺序无关。
- `UpdateAndUpload(fence, camera)` 在**固定时机**（主渲染后、UI 前）统一上传+录制，保证命令顺序确定。
- 资源管理收敛在 Manager 内部（动态 VB），调用者零资源责任（规避"谁调用谁管理资源"的协作模式冲突，规则 11）。

### 2.3 GS 展开 + 深度

- **GS 展开**：D3D12 原生 LINELIST 无可靠线宽（通常 1px），GS 按屏幕空间宽度把线段展开成四边形（2 三角形）。项目已有 GS 先例（`BillboardRenderer::m_gs`）。
- **深度**：PSO 具备深度测试（按需启用），编辑器线框与场景正确遮挡关系，Game 调试线同样受益。

## 三、设计

### 3.1 WireframeManager 接口

```cpp
// Engine/Renderer/Debug/WireframeManager.h
class WireframeManager {
public:
    static WireframeManager &GetInstance();

    void Initialize(ID3D12Device *device);      // 创建 PSO/根签名/缓冲
    void Shutdown();
    bool IsInitialized() const;

    // ── 声明式收集（CPU 侧，无 GPU、顺序无关） ──
    void AddLine(const DirectX::XMFLOAT3 &a, const DirectX::XMFLOAT3 &b, DirectX::XMFLOAT4 color);
    void AddAABB(const Math::BoundingAABB &aabb, const DirectX::XMMATRIX &world, DirectX::XMFLOAT4 color);
    // Blender 风格：相机位置 → 远平面 4 角点汇聚线 + 近/远裁剪面矩形（线宽默认 3.0f）
    void AddFrustum(const Frustum &frustum, const DirectX::XMFLOAT3 &cameraPosition, DirectX::XMFLOAT4 nearColor,
                    DirectX::XMFLOAT4 farColor, DirectX::XMFLOAT4 connectColor);
    void Clear();                                // 帧首清空

    // ── 固定录制点 ──
    void UpdateAndUpload(uint64_t fence, const Camera &camera);

    void SetVisible(bool visible);
    bool IsVisible() const;
private:
    struct LineVertex { DirectX::XMFLOAT3 pos; DirectX::XMFLOAT4 color; };
    std::vector<LineVertex> m_lines;             // CPU 线列表（帧内累积）
    // GPU: 动态 VB + PSO + 根签名
};
```

顶点格式：`Position(3f) + Color(4f)`，`LINELIST` 拓扑，VS 直传、GS 展开、PS 直出颜色。

### 3.2 缓冲策略

- CPU 侧：`std::vector<LineVertex>` 帧内累积（Editor 每帧 ~ 24-48 顶点，量极小）。
- GPU 侧：动态顶点缓冲，`UpdateAndUpload` 时上传一次；上传缓冲复用 `FrameScratchAllocator`（`AssetPreviewSystem.md` 已有先例，Editor 侧已初始化 `m_scratchAllocator`）。

### 3.3 挂载点（两端各一处，共享单例）

| 端 | 位置 | 说明 |
|:---|:-----|:-----|
| Editor | `Editor.cpp` 主循环固定位置（GridManager `UpdateAndUpload` 调用附近） | 复用现有主循环 |
| Game | `GameWorld::Update()` 渲染固定位置 | 后续接入 |

## 四、GeometryGenerator 复用评估（Engine/Renderer/Utils）

`GeometryGenerator`（Frank Luna）可生成 Box/Sphere/Geosphere/Cylinder/Grid/Quad 的三角网格（`MeshData`：顶点+索引，顶点含 Position/Normal/TangentU/UV）。

| 用途 | 可行性 | 说明 |
|:-----|:-------|:-----|
| 生成线框原语（AABB 盒 12 边 / 球体经纬线） | ✅ 可行 | 从三角网格提取边线 → LINELIST；或直接构造（AABB 8 角点 12 边手写更直接） |
| 直接渲染三角网格 + 线框填充 | ⚠️ 不推荐 | `D3D12_FILL_MODE_WIREFRAME` 会画所有三角形边（含细分内部边），不适合调试线 |
| 复杂形状（球/圆柱）线框 | ✅ 可行 | `CreateSphere` 生成后按边提取经纬线，供后续扩展 |

**结论**：GeometryGenerator 可作为线框原语（球体/圆柱等复杂形状）的几何来源；AABB/视锥体等基础线框直接构造线段更简单。当前实现先覆盖 AddLine/AddAABB/AddFrustum（线段直构），GeometryGenerator 预留为扩展点。

## 五、实施步骤

1. **`WireframeManager.h/.cpp`**（`Engine/Renderer/Debug/`）：单例 + 收集 API + `UpdateAndUpload` 固定录制。
2. **`line.hlsl`**（`Shaders/`）：VS 直传 Position+Color → GS 屏幕空间展开（线宽 2.0f）→ PS 直出。
3. **CMake**：`RENDERER_SOURCES` GLOB 自动覆盖新目录（`Renderer/*.cpp|*.h` 递归）。
4. **Editor 挂载**：`Editor.cpp` 初始化 WireframeManager + 主循环固定位置 `UpdateAndUpload`；Gizmo 改调 `AddAABB/AddFrustum`。
5. **Game 挂载**（后续）：`GameWorld::Update()` 固定位置。
6. **文档**：本文件 + `ComponentEditorSystem.md` 引用 + `Docs/README.md` 索引。

## 六、相关文档

- `Docs/architecture/core/EngineOverview.md` §9 — ECS 唯一数据源与 Manager 分工
- `Docs/architecture/scene/SceneSnapshot.md` §2.2 — 运行时唯一数据源
- `Docs/architecture/editor/ComponentEditorSystem.md` §十一/十二 — Gizmo 绘制/计算分离与视锥体修复
- `Docs/architecture/assets/AssetPreviewSystem.md` — FrameScratchAllocator 复用先例

---

## 七、实施记录（2026-08-01 已执行）

### 改动清单

| # | 文件 | 内容 |
|:-:|:-----|:-----|
| 1 | `Engine/Renderer/Debug/WireframeManager.h/.cpp`（新增） | 单例管理器：`AddLine`/`AddAABB`/`AddFrustum`/`Clear` 声明式收集 + `UpdateAndUpload`/`Draw` 固定录制；内部动态 VB（UPLOAD 64KB）+ 参数 CB + 根签名 + GS 展开 PSO（深度测试，不写深度） |
| 2 | `Shaders/line.hlsl`（新增） | VS 直传 Position+Color → GS 屏幕空间展开（线宽 2.0f，w<=0 线段丢弃，消除投影翻转伪影）→ PS 直出 |
| 3 | `Editor/EditorLib/Core/Editor.cpp` | include + Initialize（深度格式取自主交换链，规则 16）+ 主循环固定位置 `SetViewportSize`+`UpdateAndUpload`（Immediate 回调）+ Lighting 渲染阶段 `Draw`（补绑 sceneRTV+DSV + 深度 COMMON→DEPTH_READ→COMMON 对称屏障） |
| 4 | `Editor/EditorLib/Viewport/Systems/EditorGizmoSystem.cpp` | 删除 `DrawBoxWireframe`（屏幕空间投影绘制，含"双 Z 交叉"伪影）；AABB/视锥体分支改为声明式 `AddAABB`/`AddFrustum` |
| 5 | `CMakeLists.txt` | 无需改动：`RENDERER_SOURCES` 的 `GLOB_RECURSE "${ENGINE_DIR}/Renderer/*.cpp|*.h"` 自动覆盖新目录 |

### 关键时序（延迟一帧，顺序确定）

```
帧 N：
  Immediate 回调   → WireframeManager::UpdateAndUpload   （上传【帧 N-1】UI 阶段收集的线列表）
  Lighting 渲染阶段 → WireframeManager::Draw             （录制线框绘制，读帧 N-1 数据）
  UI 阶段（ImGui）  → Gizmo AddAABB/AddFrustum           （收集【帧 N】线列表，供帧 N+1 绘制）
```

- 声明式收集（UI 阶段，顺序无关）与固定录制（渲染阶段，顺序确定）解耦，调试线框延迟一帧可接受。
- `Draw` 挂载点注意：Lighting 阶段原 `OMSetRenderTargets` 未绑 DSV，而 Wireframe PSO 声明了 DSVFormat，因此挂载处补绑 `sceneRTV + DSV` 并对称处理深度屏障（规则 10/16）。

### Game 端接入（未做，后续）

- `GameWorld::Update()` 渲染固定位置调用 `WireframeManager::UpdateAndUpload(fence, camera)` + `Draw(cmdList)`，Game 调试代码直接 `AddLine` 即可。

---

## 八、架构演进备注（2026-08-01）

> WireframeManager 当前是"资源管理器 + 渲染器"混合体（既持资源句柄/收集，又含 PSO/录制），长远方向不确定。

### 观察

| 职责 | 当前实现 | 未来归属 |
|:-----|:---------|:---------|
| 资源管理 | 动态 VB / CB 生命周期、`AddLine` 收集、`UpdateAndUpload` 上传 | **管理器**（保留） |
| 渲染 | `CreateRootSignature`/`CreatePSO`/`Draw` 录制 | **渲染器**（拆出） |

### 项目已有分离先例：GridManager / GridRenderer

`GridManager`（管理器：持 `m_quadVB/m_gridCB` 句柄 + 参数 + 上传，不录制命令）与 `GridRenderer`（渲染器：继承 `IRenderer`，`Draw` 从 GridManager 取资源句柄录制）——这是本项目标准的"管理器 + 渲染器"分离模式，WireframeManager 未来拆分按此模板即可：

```
WireframeManager（管理器，保留）
  ├─ AddLine/AddAABB/AddFrustum/Clear     ← 收集（不动）
  ├─ 动态 VB / CB 资源句柄 + 访问接口      ← 参考 GridManager::GetQuadVBHandle()
  └─ UpdateAndUpload（上传）               ← 保留
WireframeRenderer（渲染器，拆出，IRenderer）
  ├─ CreateRootSignature/CreatePSO        ← 从 WireframeManager 移出
  └─ Draw(cmdList, viewProj, ...)         ← 移出，向管理器取资源
```

### 触发时机

- **粒子系统**（最可能的触发点）：若粒子走"每帧 CPU 填动态缓冲 + 批量绘制"路径（而非纯 GPU 驱动），其渲染设施（动态 VB 上传、批 Draw）与 WireframeManager 渲染侧同构，届时将 `WireframeRenderer` 抽成通用"动态几何渲染器"并复用于粒子。
- 其他多实例动态几何（骨骼调试线、路径线框）出现时也可触发。

### 结论

不是"要不要拆"，而是"何时拆、按什么边界拆"——拆分模板已由 GridManager/GridRenderer 先例确定。当前量级（单文件管理器）暂无需拆分，保持现状。

---

## 九、运行时无效果排查与修复（2026-08-01）

> 反馈：选择主相机实体看不到视锥体（运行时无效果）。沿数据流逐段排查后定位两个问题并修复。

### 排查过程（数据流三段）

| 段 | 结论 |
|:---|:-----|
| **Gizmo 数据生产**（AddFrustum） | ✅ 代码可达：`m_layout->GetSelectedEntity()` → DrawGizmo → 选中实体有 `CameraComponent` 即调用 `AddFrustum`（12 条线） |
| **UpdateAndUpload 上传** | ⚠️ 发现缺陷：`Clear()` 从未被调用 → `m_lines` 无限累积（每帧线数叠加，内存增长） |
| **Draw 录制执行** | ❌ **根因 1**：`EditorLightingRenderSystem` 在 `m_opaqueQueue.Empty()` 时**整体提前 return**——空场景（仅相机实体、无 Opaque 网格）时 `WireframeManager::Draw` 永远不会执行。**线框与实体/光源/采样无关，被场景内容拦截是设计缺陷** |

### 修复 1：线框与场景内容解耦（根因 1）

`EditorLightingRenderSystem` 提前返回条件重构：

```cpp
const bool hasOpaque = !m_opaqueQueue.Empty();
const bool hasWireframe = WireframeManager::GetInstance().GetLineCount() > 0;
if (!hasOpaque && !hasWireframe)
    return;   // 仅当两者都为空才跳过

// 后续对称调整：
// - G-buffer SRV 屏障：仅 hasOpaque 时（Wireframe 不读 G-buffer）
// - LightingRenderer 调用：仅 hasOpaque 时
// - G-buffer SRV → COMMON 回退：仅 hasOpaque 时（对称屏障，规则 10）
// - 场景颜色 RT COMMON → RENDER_TARGET 及回退：始终（Wireframe 画到 sceneColor）
```

### 修复 2：m_lines 生命周期（缺陷）

`WireframeManager` 增加 `m_uploadedLineCount`（最近上传线数），`UpdateAndUpload` 上传后清空 `m_lines`，`Draw` 依据 `m_uploadedLineCount` 而非 `m_lines`——收集、上传、绘制三者解耦，杜绝无限累积：

```cpp
// UpdateAndUpload 末尾
m_uploadedLineCount = m_lines.size();
m_lines.clear();

// Draw
if (m_uploadedLineCount == 0) return;
// ... vbView.SizeInBytes / DrawInstanced 均用 m_uploadedLineCount
```

### 时序确认（延迟一帧，设计如此）

```
帧 N：Immediate 回调 UpdateAndUpload（上传【帧 N-1】UI 阶段收集的线，记录 m_uploadedLineCount 后清空 m_lines）
     → Lighting 渲染阶段 Draw（依据 m_uploadedLineCount 录制）
     → UI 阶段 Gizmo AddFrustum（收集【帧 N】线，供帧 N+1 上传）
```

### 日志（节流 120 帧，定位用，可保留）

| 位置 | 内容 |
|:-----|:-----|
| `WireframeManager::AddFrustum` | 收集 12 线 + 近平面角点坐标 |
| `WireframeManager::UpdateAndUpload` | 上传线数 + 屏幕尺寸 |
| `WireframeManager::Draw` | 录制线数 + PSO 有效性 |
| `EditorLightingRenderSystem` | （已随修复移除，原用于确认提前返回） |

### 验证要点

1. 空场景（仅相机实体）选中主相机：应显示视锥体（近亮青/远橙黄/连接中性白）。
2. 有实体场景：光照与线框均正常，G-buffer 屏障对称。
3. 长时间运行：`m_lines` 不再累积（日志 UpdateAndUpload 线数恒定 ≈12）。

---

## 十、视锥体 Blender 风格绘制 + 距离自适应线宽（2026-08-01）

> 反馈：① 线太细不可观察；② 参考 Blender，视锥体线须汇聚到相机位置（否则近/远两个矩形独立，像两个大小不一的锥体，找不到相机位置）。

### 改动 1：Blender 风格汇聚线

`AddFrustum` 签名增加 `cameraPosition` 参数，汇聚线从**相机位置 → 远平面 4 角点**（而非原"近平面角点 → 远平面角点"连接边），近/远裁剪面矩形保留：

```
之前：近矩形 + 远矩形 + 近↔远连接边（12 线，视觉上像两个独立锥体）
之后：近矩形 + 远矩形 + 相机点→远矩形 4 角点（12 线，线汇聚到相机位置，定位清晰）
```

调用点（`EditorGizmoSystem.cpp`）传入 `tc->position`。

### 改动 2：距离自适应线宽

视锥体在远处投影变小后，固定像素线宽不可观察。GS 按线段到相机深度（clip w）动态放大展开宽度：

```hlsl
// line.hlsl GS
float avgDepth = (p0.w + p1.w) * 0.5f;          // 透视投影下 clip w = view-space 深度
float distScale = clamp(avgDepth / gParams.w, 1.0f, 8.0f); // 参考距离处=1，越远越粗，上限 8×
float2 halfW = normal * (gParams.z * 0.5f) * distScale;
```

- `gParams.w`（原 pad）复用为 `referenceDistance`，默认 50.0f（此距离处线宽=基础宽）。
- 基础线宽 `m_lineWidth` 2.0f → **3.0f**（不可观察问题）。
- 正交投影 clip w = 1 → `distScale < 1` clamp 到 1，保持固定线宽（正交无透视放大需求）。

### 改动文件

| 文件 | 内容 |
|:-----|:-----|
| `Engine/Renderer/Debug/WireframeManager.h` | `AddFrustum` 签名加 `cameraPosition`；CB `pad` → `referenceDistance`；`m_lineWidth=3.0f`；`m_referenceDistance=50.0f` |
| `Engine/Renderer/Debug/WireframeManager.cpp` | `AddFrustum` 汇聚线实现；`UpdateAndUpload` 写 `referenceDistance` |
| `Shaders/line.hlsl` | GS 距离自适应线宽（`distScale` clamp [1,8]） |
| `Editor/EditorLib/Viewport/Systems/EditorGizmoSystem.cpp` | 调用 `AddFrustum(frustum, tc->position, ...)` |

---

## 十一、视锥体线框可观察性调整（2026-08-02）

> 反馈：① 线仍偏粗；② GS 展开的三角面在某些角度被裁剪、显得奇怪；③ 5700 的远裁剪面太夸张，远裁剪面线段几乎不可见；④ 距离自适应线宽的上下限需合理。

### 根因 1：线段被裁剪 —— 深度测试 LESS_EQUAL

`CreatePSO` 原 `DepthFunc = LESS_EQUAL`：线框与场景共用深度缓冲，**场景物体后方的线段被深度剔除**（调试叠加层不应被场景遮挡）。修复：

```cpp
psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS; // 始终通过，不写深度
```

### 根因 2：短线段被展开成超大三角面

distScale 上限 8× + 基础线宽 3.0f 时，展开宽度可超过线段屏幕长度一半 → 短线段（视锥体远端、屏幕边缘）被撑成奇怪三角面/扭曲。修复：展开宽度上限 = 线段屏幕长度一半：

```hlsl
float maxHalfW = len * 0.5f;
halfW = min(halfW, float2(maxHalfW, maxHalfW));
```

> **GS 展开本质**：D3D12 LINELIST 原生线宽固定 1px 不可控，GS 将线段展开为**四边形（2 个三角形）**——期望行为正是"四边形线段"。此前"奇怪三角面"源于展开宽度超过线段长度，非 GS 方案本身问题。

### 根因 3：远裁剪面 5700 极端值

`BuildFromCamera` 直接使用 `camComp->farPlane`，5700 这类极端 far 使远平面矩形投影缩成一点、线段不可见。修复：显示用 far 截断（行业通常，Blender/Unity 默认远裁剪面 1000）：

```cpp
const float kDisplayFarLimit = 1000.0f;
float displayNear = std::max(camComp->nearPlane, 0.01f);
float displayFar = std::min(camComp->farPlane, kDisplayFarLimit);
if (displayFar <= displayNear) displayFar = displayNear + 1.0f;
```

### 根因 4：距离自适应线宽参数

| 参数 | 调整前 | 调整后 | 说明 |
|:-----|:-------|:-------|:-----|
| `m_lineWidth` | 3.0f | **2.0f** | 3.0f 偏粗；2.0f 配合远处自适应放大 |
| `m_referenceDistance` | 50.0f | **100.0f** | 参考距离内保持基础宽，更平缓 |
| distScale 上限 | 8× | **4×** | 远处最大 8px，不过分粗 |

### 改动文件

| 文件 | 内容 |
|:-----|:-----|
| `Engine/Renderer/Debug/WireframeManager.cpp` | `DepthFunc = ALWAYS`（线框始终可见） |
| `Shaders/line.hlsl` | 展开宽度上限 `maxHalfW = len * 0.5`；distScale 上限 8→4 |
| `Engine/Renderer/Debug/WireframeManager.h` | `m_lineWidth=2.0f`；`m_referenceDistance=100.0f` |
| `Editor/EditorLib/Viewport/Systems/EditorGizmoSystem.cpp` | 显示用 far 截断（`kDisplayFarLimit=1000`） |

---

## 十二、属性卡 far 上限 + GS 粗细统一（2026-08-02）

> 反馈：① ECS 属性卡的 farPlane 上下限未同步调整（仍可输入 5700 类极端值）；② GS 展开在较远时线段粗细不一致。

### 根因 1：属性卡 farPlane 上限未对齐

`CameraEditor.cpp` 的 farPlane `SliderFloat` 上限原为 **10000**——用户可通过属性卡输入 5700 等极端值，Gizmo 显示截断（`kDisplayFarLimit=1000`）只是显示层补救，ECS 组件值本身未限制。修复：上限 10000 → **1000**（与显示截断对齐，行业通常 Blender/Unity 默认远裁剪面）。

### 根因 2：GS 较远时线段粗细不一致

两个叠加原因：

1. **distScale 用平均深度 `avgDepth = (p0.w + p1.w) * 0.5`**：汇聚线（近→远深度差大）avgDepth 偏大 → 整条线过粗；同一视锥体内各线段（汇聚线/近矩形/远矩形）深度基准不同 → 粗细不一。修复：改用**线段最近端深度 `min(p0.w, p1.w)`**——视锥体各线段都以近端为基准，同视锥体内粗细统一。
2. **`maxHalfW = len * 0.5` 无保底**：远线段屏幕投影很短时，`len*0.5` 会把展开宽度压缩成极细线甚至不可见。修复：加**保底下限 ≥ 基础线宽的一半**（`gParams.z * 0.25f`）。

```hlsl
// line.hlsl GS
float nearDepth = min(p0.w, p1.w);                          // 最近端深度（同视锥体统一基准）
float distScale = clamp(nearDepth / gParams.w, 1.0f, 4.0f);
float2 halfW = normal * (gParams.z * 0.5f) * distScale;

float maxHalfW = max(len * 0.5f, gParams.z * 0.25f);        // 上限防畸变，保底防远线过细
halfW = min(halfW, float2(maxHalfW, maxHalfW));
```

### 改动文件

| 文件 | 内容 |
|:-----|:-----|
| `Editor/EditorLib/Properties/Editors/CameraEditor.cpp` | farPlane 上限 10000 → 1000（对齐显示截断） |
| `Shaders/line.hlsl` | distScale 用 `min(p0.w,p1.w)`；`maxHalfW` 保底 `gParams.z*0.25` |

---

## 十三、远近裁剪面与单位系统关联（2026-08-02 记录，M 概念为抽象方向）

### 核心事实：远近裁剪面数值与单位系统强关联

视锥体显示截断（`kDisplayFarLimit=1000`）、属性卡 far 上限（1000）、`CameraComponent` 默认 `nearPlane=0.1 / farPlane=1000` 这些**数值本身没有绝对意义**——它们只在特定单位制下成立：

| 单位制假设 | near=0.1 / far=1000 的实际含义 |
|:-----------|:-------------------------------|
| 1 单位 = 1m（Unity 惯例） | 近裁剪 0.1m、远裁剪 1000m（合理，对齐 Unity 默认 far=1000） |
| 1 单位 = 1cm（Unreal 惯例） | 近裁剪 1mm、远裁剪 10m（对大型场景 far 偏小） |
| 1 单位 = 1 英尺 | 近裁剪 3.9 英寸、远裁剪 1000 英尺 |

**结论**：当前默认值（0.1/1000）在 **1 单位 = 1m** 假设下合理；但引擎不应把"数值"硬编码为语义，应把"单位→米"的换算抽象出来。

### 未来方向：M 概念（单位系统抽象）

参考大型引擎（Unity 1 单位=1m、Unreal 1 单位=1cm）存在**单位制配置**，规划抽象：

```
单位系统配置（引擎级，适应具体游戏）
  └─ UnitsPerMeter（1 单位对应的米数）← 核心换算因子
        ├─ nearPlane_m = nearPlane / UnitsPerMeter
        ├─ farPlane_m  = farPlane  / UnitsPerMeter
        ├─ 视锥体显示 far 截断上限（当前硬编码 1000）→ 由单位制推导
        ├─ 属性卡 far 滑条上限 → 由单位制推导
        └─ WireframeManager::m_referenceDistance（距离自适应参考距离 100）→ 由单位制推导
```

- **何时做**：出现需要不同单位制的具体游戏时（当前编辑器/引擎内 1 单位 = 1m 假设已够用）。
- **涉及点**：`CameraComponent` 默认值、`CameraEditor.cpp` far 上限、`EditorGizmoSystem.cpp` `kDisplayFarLimit`、`WireframeManager::m_referenceDistance` 均需改为从单位配置读取。
- **约束**：`fov` 为角度（与单位无关）；`nearPlane/farPlane/orthoSize` 为长度（受单位制影响）；`GridManager` 的 spacing 同样受单位制影响（网格间距是长度）。

### 当前记录

- 所有视锥体相关长度数值（near/far 默认、显示截断 1000、属性卡上限 1000、参考距离 100）均基于 **1 单位 = 1m** 假设，后续抽象 M 概念时统一替换。
