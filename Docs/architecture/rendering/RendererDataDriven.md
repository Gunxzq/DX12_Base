# 渲染器数据驱动与绑定架构（设计定案）

> 日期：2026-08-02
> 状态：📋 设计定案（多轮讨论收敛，实施留待渲染架构重构立项时细化）
> 关联：`Docs/architecture/rendering/SubMeshMaterialSlots.md`（材质槽）、`EngineOverview.md`（ECS-构建器-渲染器模式）、`FrameResourceManager.md`（帧资源）、`AssetLoaderImprovement.md`（资产加载注册表）
> 结论先行：**渲染器差异 = PSO 集合（几何条件共享 + 着色器变体各一）；绑定 = 静态描述 + 动态列表分离；资源取值 = RenderContext 外观（get 方法透传管理器 + 占位兜底）；条件校验 = UI 源头过滤 + 构建器兜底；渲染项收集 = RenderSlotComponent + RenderSlotCache 分桶（事件驱动，§4.1b/c/d；材质组件模式已废弃）**。

---

## 一、动机与出发点

1. **渲染器增加后通用性变化**：14 个管线渲染器（Opaque/Skinned/Terrain/Water/Billboard…）各自硬编码 `Create*PSO()` / `Create*RootSignature()`，样板代码重复。
2. **材质槽模式推进**（待办 #22）：submesh[i] → material[i] → 每个材质声明自己的渲染方式，自动得到渲染项——需要"材质 → PSO"的映射机制。
3. **渲染项异构**：OpaqueRenderItem / SkinnedRenderItem 只差 1-2 个字段（`probeIndex` vs `boneBufferAddress`），但各自的 BeginFrame/Draw 参数签名不同。
4. **热重载可能性**：着色器 / PSO 状态描述化后，可经 SnapshotSystem 文件变更检测 + PSO 重建实现热重载。

---

## 二、核心结论（定案）

### 2.1 渲染器差异 = PSO 集合

D3D12 的本质：`ID3D12PipelineState` 是"着色器 + 固定功能状态（混合/深度/光栅/输入布局/RT 格式）+ 根签名"的不可变打包体。**任何一项不同 → 不同 PSO 对象**。

- **几何条件相同、着色器不同 = 不同的 PSO**（DX12 底层事实）
- **一个渲染器 = 一组 PSO**（PSO 集合），而非单 PSO

**项目实证**（已存在"一渲染器多 PSO"模式）：

| 渲染器 | PSO 持有 | 变体维度 |
|:-------|:---------|:---------|
| `ShadowRenderer` | **4 个**（方向光/点光实例化/点光 GS/聚光灯） | 光源类型 + 展开方式 |
| `SsaoRenderer` | **2 个**（SSAO / Blur） | 着色器不同 |
| `OpaqueRenderer` | 1 个 | 当前无变体 |
| `SkinnedRenderer` | 1 个 | 当前无变体 |

数据驱动的对象是"**PSO 集合**"（几何条件共享 + 变体各一），不是单 PSO。

### 2.1a 渲染器差异的本质（2026-08-02 定案）——资源消费者，三处差异

> 讨论触发：自定义管线（渲染器）的需求从何而来？渲染器差异到底有多少？分析大型引擎（Unity SRP / UE）后定案：**外部资源是写死的（引擎统一管理），自定义渲染器只能选择性使用这些资源，不具备自定义资源的能力**——因此渲染器差异**限定在 PSO + 根签名 + 着色器代码三处**。

**核心原则：渲染器是资源的消费者，不是资源的所有者**：

| 引擎 | 资源层（写死，引擎管理） | 渲染器层（差异） |
|:---|:---|:---|
| **Unity SRP** | 纹理/CB 由引擎 + C# 创建管理 | ShaderPass 声明绑定，渲染器**不能创建自定义资源类型** |
| **UE** | 几何/材质/纹理由资产管理器统一持有 | `FMeshPassProcessor` 用引擎提供的绑定，pass 只声明消费不拥有 |
| **本项目** | GeometryResourceManager / MaterialManager / TextureManager / GpuResourceManager | RendererInstance 只声明"用哪些槽位绑什么资源"（desc） |

**渲染器差异 = 三处，其余全共享**：

| 差异维度 | 内容 | 数据化落地 |
|:---|:---|:---|
| **PSO 状态** | 混合/深度/光栅/RT 格式 | renderer.json 的 variants（blend/depthStencil/rasterizer/rtvFormats）✅ |
| **根签名** | 槽位布局（哪些资源、类型、可见性） | rootSignature.params（slot/type/reg/space）✅ |
| **着色器代码** | 如何消费资源、如何输出 | shaderFiles + vsEntry/psEntry ✅ |

**推论**：**新增渲染器 ≠ 新增资源类型**——新增渲染器只需 renderer.json + 着色器 + 配对注册（§4.2a），资源层零改动。即"**资源封闭，渲染器开放**"（§4.1a 开放封闭的根基）。

### 2.2 静态描述 / 动态绑定分离

根参数绑定在生命周期里出现在两个时点、两种性质：

| 身份 | 时点 | 性质 | 内容 |
|:-----|:-----|:-----|:-----|
| **静态描述** | PSO/根签名初始化时 | 固定（根签名长什么样） | 槽位布局：槽0=CBV、槽1=DescriptorTable… |
| **动态绑定** | 每帧 BeginFrame/Draw | 可变（这个槽绑什么） | 具体地址/句柄 |

两套结构分开定义：

```cpp
// 静态：根签名/PSO 描述（初始化时一次消费）
struct RootSigDesc { std::vector<RootParamDesc> params; };  // 槽位布局：slot + type + visibility

// 动态：帧绑定（每帧）
struct BindValue { BindType type; uint32_t slot; union { D3D12_GPU_VIRTUAL_ADDRESS gpuAddr; D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle; uint32_t consts[4]; }; };
std::span<const BindValue> passBinds;   // BeginFrame 公共绑定
std::span<const BindValue> itemBinds;   // Draw 差异绑定
```

**渲染项字段 ↔ 槽位全声明（2026-08-03 补充，呼应 §4.2a1）**：渲染项字段最终都会被绑定到管线槽位——**槽位集 = 渲染项字段集**，renderer.json 的 `rootSignature.params` 应**全声明**可能使用的槽位（**不限于 PassConstants**：光源 CBV、水 CBV、骨骼 RootSRV、反射探针等都有使用可能性），渲染器按字段有效性选择性消费（§4.2a1：字段无效即不绑定）：

| 渲染项字段 | 槽位（BindSlot） | 类型 | 资源提供者 | 有效性（组件存续） |
|:---|:---|:---|:---|:---|
| passConstantsAddress | PassConstants | CBV | FrameResourceManager | 恒有 |
| lightConstantsAddress | LightConstants | CBV | LightManager | 场景有光源 |
| waterConstantsAddress | ObjectCB/WaterCB | CBV | WaterManager | WaterComponent |
| materialIndex | MaterialBuffer | DescriptorTable | MaterialManager | RenderSlotComponent |
| textureHeapStart | TextureHeap | DescriptorTable | DescriptorHeaps | 恒有 |
| instanceBuffer | InstanceBuffer | RootSRV | FrameResourceManager | 恒有 |
| boneBufferAddress | BoneBuffer | RootSRV | SkeletonManager | SkinnedComponent |
| probeIndex | （InstanceData 内） | — | ReflectionProbeManager | ReflectionConsumerComponent |
| billboardTexture | BillboardTexture | DescriptorTable | TextureManager | BillboardComponent |

**槽位集是"全可能"的并集**（§2.1a：资源封闭）：渲染器声明自己消费的子集（params），未声明槽位在 `ApplyBindList` 时跳过（`slotToIndex == UINT32_MAX`）——**一个渲染项字段对应一个槽位，渲染器按需绑定，新增字段 = 新增槽位声明 + Builder 推断，无需新增渲染项类型**。

**第三类：标量/标志字段走 Constants32 根常量（2026-08-03 补充）**：渲染项字段**不全是资源句柄**——那些"不够 ECS 组件推断"的小数据/参数（材质索引、探针索引、接收阴影标志、LOD 档位、深度偏置等），**不应造资源**，而应作为 **32 位根常量**（`Constants32`，`SetGraphicsRoot32BitConstants`）或并入 **CBV**（结构化小数据）绑定。字段 → 绑定形态三分类：

| 字段性质 | 绑定形态 | 例 |
|:---|:---|:---|
| **资源句柄/地址**（ECS 组件推断） | DescriptorTable / RootSRV / CBV | vb/ib/instanceBuffer/boneBuffer/materialBuffer/textureHeap |
| **结构化小数据**（多个相关标量） | CBV（ObjectCB 等） | 世界矩阵、光源参数（LightManager） |
| **单标量/标志**（≤4 个 32 位） | **Constants32 根常量** | materialIndex、probeIndex、receivesShadow、LOD 档位 |

**Constants32 链路现状**（2026-08-03 核查）：`BindType::Constants32` + `BindValue::consts[4]` + `RootParamDesc::numConstants` 已定义（BindSlot.h）；PSO 工厂 JSON 解析 `"Constants32"` + `InitAsConstants(numConstants, reg, space)` 已支持（PSOFactory.cpp）；**`ApplyBindList` 的 `case Constants32` 仍为 `break;`（待实现）**——实施时需按物理索引查 `numConstants`（数量在 RootParamDesc，`BindValue` 只有 `consts[4]`），调用 `SetGraphicsRoot32BitConstants(phys, numConstants, consts, 0)`。对齐 UE `FMeshDrawShaderBindings`：小参数直接内联根常量，不占描述符。

**CBV 数据源判别（2026-08-03 补充）——按渲染器需要选择，不强制归属**：结构化小数据走 CBV 时，数据来源**视渲染器需要而定**，核心准则是"**数据是否随场景/资源状态变化**"：

| 数据性质 | 数据源 | 现实依据 |
|:---|:---|:---|
| **随场景/资源状态变化**（相机/光源/骨骼/水体参数） | **管理器提供**（FrameResourceManager/LightManager/SkeletonManager/WaterManager），渲染器只消费 | ✅ **管理器 PSO 的着色器已消费管理器参数**（PassConstants 来自 FrameResourceManager、材质表来自 MaterialManager 等）——本条已被现状实证 |
| **渲染器/着色器专属配置**（后处理强度/阈值/步长等，与场景资源无关） | 管理器不介入：编译期固定走 renderer.json 静态配置；每帧小参数走 Constants32 根常量；结构化临时数据走渲染器自持 CBV（FrameScratchAllocator） | 管理器管理的是场景/资源状态，不持有"某渲染器用什么参数" |

**判别结论**：**不预先规定 CBV 数据必须归属管理器或渲染器**——渲染器按自身需要声明槽位并消费；数据若随场景/资源状态变化则管理器是自然来源（现状已实证），若为渲染器配置则走 renderer.json / 根常量 / 自持 CBV。防止两类错误：渲染器配置错误塞进管理器（管理器膨胀）、每帧小参数错误造 CBV 资源（本可走根常量）。

### 2.2a 根签名数据驱动化（2026-08-02 定案）——JSON 声明 + D3DReflect 校验分层

> 讨论触发：PSO 已数据驱动（§6.2 Step 4），但 `CreateGBufferRootSignature` 仍硬编码（4 params + 6 静态采样器）。着色器外置修改（新增 t4 纹理槽、改 space、加常量）→ 根签名与着色器强耦合失配 → 必须数据驱动。

**核心认识：根签名与着色器强耦合**（寄存器布局是着色器声明的），数据驱动必须覆盖它。但 D3DReflect 反射**不是最终呈现**，而是"事实核验"手段——最终形态是**声明 + 校验分层**：

| 层 | 角色 | 内容 | 性质 |
|:---|:---|:---|:---|
| **JSON 声明（语义层）** | 意图定义 | 语义槽位（PassConstants/MaterialBuffer/TextureHeap/InstanceBuffer/BoneBuffer...）+ 物理映射（physicalIndex）+ PSO 状态 | 设计意图，单一事实来源 |
| **D3DReflect（物理层，L4 热重载时启用）** | 事实核验 | 从着色器字节码读实际寄存器/space/可见性 | 编译产物事实 |
| **冲突检测（校验层）** | 防漂移 | 反射结果 vs JSON 声明不一致 → 报错（捕获"着色器改了 JSON 忘了改"） | 一致性门卫 |

**分层协作**（大型引擎同款：UE 收集 shader bindings 与管线状态合并、Unity pass 声明绑定）：

```
JSON（语义层）声明语义槽位 + PSO 状态
  → PSO 工厂构建根签名（RootParamDesc → CD3DX12_ROOT_PARAMETER）
  → L4：D3DReflect 从着色器字节码核验实际寄存器 → 与 JSON 比对 → 不一致报错
```

**分阶段落地（避免过度设计）**：

| 阶段 | 内容 | 反射角色 |
|:---|:---|:---|
| **现在（L2 收尾）** | 根签名 JSON 化：`opaque.renderer.json` 的 rootSignature.params → 工厂构建根签名 | 无（纯声明） |
| **L4（热重载前置）** | D3DReflect **校验模式**：着色器重编译后反射 vs JSON 声明比对，不一致则阻止热重载 + 报错 | **只校验不生成**（最小侵入） |
| **远期（可选）** | D3DReflect 自动补全：语义槽位 JSON 声明 + 物理寄存器反射填入，JSON 只写意图 | 补全物理细节 |

> **"只校验不生成"是最优形态**：JSON 保持单一事实来源（意图），反射仅作一致性门卫——既解决"着色器改了就失配"，又不引入反射生成根签名的复杂度。

**现状**：`RootParamDesc` 定义于 `BindSlot.h`（§2.2，含 visibility）；`opaque.renderer.json` 已有 `rootSignature.params`（slot/type/physicalIndex）；`ApplyBindList` 已按语义槽位→物理索引映射。根签名 JSON 化的基础设施已具备。

### 2.2b 渲染器管理器（RendererManager，2026-08-02 定案）——数据驱动渲染器本体

> 讨论触发：接入工厂只是"用 JSON 替换硬编码"（`OpaqueRenderer` 类仍存在，仅内部走工厂），**不具备消灭渲染器类的能力**。目标：像 `DescriptorHeapCollection::AddPartition` 一样——**读取配置 → 初始化内容 → 驻留 → 热重载**，得到**渲染器管理器**统一初始化数据驱动渲染器，并与世界状态机协作完成预初始化和缓存。

**核心认知：接入工厂 ≠ 数据驱动渲染器**：

| | 现状（接入工厂） | 目标（渲染器管理器） |
|:---|:---|:---|
| 渲染器 | `OpaqueRenderer`/`SkinnedRenderer` 类仍存在（内部走工厂） | **类消亡**——渲染器 = 数据实例 + 通用执行引擎 |
| 差异表达 | 每个类手写 BeginFrame/Draw/PSO 集合 | **全部在 renderer.json**（变体集合 + 根签名 + 绑定描述 + 几何条件） |
| 生命周期 | 每个类各自 Initialize/Shutdown | **管理器统一**：读配置→初始化→驻留→热重载 |

**与描述堆同构**（`DescriptorHeapCollection::AddPartition(tag, type, count, heapTag)` 即"读配置→初始化→驻留"模板）：

```cpp
// 渲染器管理器（目标）
class RendererManager {
    void AddRenderer(const std::string &name);       // 读 Content/Renderers/{name}.renderer.json → 创建 RendererInstance
    RendererInstance *Get(const std::string &name);  // 驻留查询
    void Reload(const std::string &name);            // 热重载：重读 JSON/着色器 → fence 同步重建（§5）
    void OnScenePreload(const SceneAssets &assets);  // 世界状态机协作：预初始化场景需要的渲染器（§7.11）
};

// 数据驱动渲染器（RendererInstance，取代手写类）
struct RendererInstance {
    RendererDesc desc;             // JSON 描述（变体集合/根签名/绑定/几何条件）
    ID3D12RootSignature *rootSig;  // 工厂构建（§2.2a）
    std::vector<ID3D12PipelineState *> psoSet; // 变体各一（§2.1）
    // 通用执行：BeginFrame/Draw 由 desc 的绑定描述驱动（ApplyBindList），无手写类差异
};
```

**与世界状态机协作（§7.11 场景级缓存归状态机的落地）**：

| 时机 | 动作 |
|:---|:---|
| **场景加载** | 状态机确定场景资产（几何/材质集合）→ `RendererManager::OnScenePreload` 预初始化所需渲染器（PSO 编译 + 缓存驻留） |
| **场景切换** | 驻留常用渲染器、释放不用缓存 |
| **热重载** | 配置/着色器变化 → `Reload`（fence 同步重建）+ D3DReflect 校验（§2.2a 防漂移） |

**串联路线图**：渲染器管理器是 L2（工厂）+ L3（铺开）+ L4（热重载/RenderContext）的**统一容器**——`OpaqueRenderer::CreateGBufferPSO/根签名/LoadShader/Initialize` 全部归入 `AddRenderer` 通用流程，类被数据消灭。

**落地状态（2026-08-02）**：
- **步 1（骨架）**：`RendererManager.h/.cpp`（AddRenderer 读配置→工厂建根签名+PSO 集合→驻留 / Get / Reload / ClearCache）已落地，编译通过。
- **步 2（绑定描述提升到渲染器级）**：`RendererInstance` 增加 `slotToIndex`（语义槽位→物理索引映射，`BuildBindings` 从 desc.rootSignature.params 构建，替代硬编码 switch）+ `ApplyBindList` 通用执行；`PSOFactory::LoadRenderer` 补 slot 字符串→BindSlot 枚举解析；**OpaqueRenderer 绑定路径已切换到 RendererInstance**（删除内部 static ApplyBindList，3 处调用改走 `m_rendererInstance->ApplyBindList`；PSO/根签名从 RendererInstance 获取）。渲染器本体的"读配置→初始化→驻留→通用执行"闭环打通，`OpaqueRenderer` 类只剩 BeginFrame/Draw 流程壳（步 4 类消亡前置）。

### 2.3 BeginFrame（pass 级）/ Draw（item 级）拆分

```
BeginFrame（pass 级）              Draw（item 级）
  = 批次公共绑定                      = item 差异绑定
  ├─ passConstants（槽0）            ├─ instanceBuffer（槽3）
  ├─ materialBufferSRV（槽1）        └─ boneBuffer（槽4，Skinned 才有）
  └─ textureHeapStart（槽2）         
  一批共享，每帧一次                  每个 item 不同，遍历设置
```

拆分收益：**避免每 item 重复绑定公共槽位**——同批次 item 遍历时，公共绑定（pass/material/texture heap）已在 BeginFrame 设好，Draw 只切换差异绑定。

### 2.4 绑定类型（"三系统句柄"问题）

D3D12 根参数有四种形态，对应四个不同的 `SetGraphicsRoot*` 调用：

| BindType | D3D12 调用 | 值形态 |
|:---------|:-----------|:-------|
| `CBV` | `SetGraphicsRootConstantBufferView(slot, addr)` | `D3D12_GPU_VIRTUAL_ADDRESS` |
| `DescriptorTable` | `SetGraphicsRootDescriptorTable(slot, handle)` | `D3D12_GPU_DESCRIPTOR_HANDLE` |
| `RootSRV/RootUAV` | `SetGraphicsRootShaderResourceView(slot, addr)` | `D3D12_GPU_VIRTUAL_ADDRESS` |
| `32BitConstants` | `SetGraphicsRoot32BitConstants(slot, ...)` | `uint32_t[]` |

`materialBufferSRV`/`textureHeapStart` 是 **DescriptorTable**（GPU 描述符句柄），`passConstantsAddress` 是 **CBV**（GPU 地址）——在绑定列表里是不同 `type` 的条目，渲染器按 type 分派。

### 2.5 RenderContext 外观（资源取值统一入口）

**不是字段快照**（每帧填充、生命周期管理复杂），而是 **get 方法外观（Facade）**：实时透传管理器，零缓存、零生命周期、帧帧实时。

```cpp
class RenderContext {
    D3D12_GPU_DESCRIPTOR_HANDLE GetSkyboxSRV()     { return m_skyboxMgr->GetSRV(); }
    D3D12_GPU_DESCRIPTOR_HANDLE GetShadowSRV(int i){ return m_lightMgr->GetShadowSRV(i); }
    D3D12_GPU_DESCRIPTOR_HANDLE GetReflectionSRV() { return m_reflectionMgr->GetSRV(); }
    D3D12_GPU_DESCRIPTOR_HANDLE GetAOSRV()         { return m_aoMgr->GetSRV(); }
    // ... 透传各管理器
};
```

- **取值来源统一**：天空盒/反射/阴影/AO 地址从 RenderContext 拿，不直接碰管理器类
- **占位纹理兜底**：管理器在资源无效时返回 1×1 白色/黑色/法线占位 SRV——渲染器永远拿到有效句柄，免判空（UE `GWhiteTexture` / Unity `Texture2D.whiteTexture` 同款）
- **多视口不需要**：管理器全局单例、切换即重建，get 天然返回当前状态

### 2.6 条件校验：几何条件（硬）∧ 材质变体（软）

渲染器/PSO 选择是**条件匹配**，不是自由指定：

| 条件 | 内容 | 来源 | 硬性/软性 |
|:-----|:-----|:-----|:---------|
| **几何条件** | 顶点格式（`DxMeshFlag_Skinned`）、网格子类型（TriangleMesh/PatchMesh）、索引宽度 | 几何数据（.dxmesh flags + 子类型） | **硬性**（不匹配 = 顶点输入布局错） |
| **材质条件** | shaderType（PBR/NPR/蒙皮）、混合/深度状态 | 材质数据（.mat） | 软性（决定 PSO 变体） |

**分层防错**：

| 层 | 职责 | 作用 |
|:---|:-----|:-----|
| **UI 绑定层**（首选） | 按几何条件过滤可选材质，不透出不匹配槽位 | **源头防错**（用户操作路径上不可能出错） |
| **JSON 手改**（绕过 UI） | 用户直接改 scene.json | 唯一能"绕过"的路径 → 需要兜底 |
| **构建器校验**（兜底） | 运行时检查几何事实 vs PSO 条件，不满足则跳过 + 警告 | 防手改 JSON 的异常 |

**关键**：UI 过滤和构建器校验用**同一个条件数据源**（PSO 描述里的 `requiresGeometry`/`requiresSkinned`/`vertexInput`）——一处定义两处消费。

**校验依据是几何数据而非 ECS 组件**：组件可能引用错误几何（材质槽/网格替换后不同步），几何的 flags/子类型是唯一不可变的事实。组件提供"路径意图"，几何校验"事实"，材质选"变体"。

### 2.6a 前置约束：顶点布局统一（2026-08-02 定案，跨渲染器）

数据驱动 PSO 的**几何条件**（顶点输入布局）判定，依赖一个前置：**顶点布局头部字段均一**（详见 `SubMeshMaterialSlots.md` §2.3a）。

- **现状问题**：`DxMeshSkinnedVertex` 头部为 `position/tangentU/normal/texC`（M3dVertex 旧布局），与 `DxMeshStaticVertex` 的 `position/normal/tangentU/texC` **顺序互换**。蒙皮网格走静态 Opaque 渲染器时 `NORMAL@12` 读到硬编码 `tangentU=(1,0,0)` → 法线全同、光照错误（RenderDoc 实测）。
- **定案**：头部（position/normal/tangentU/texC，44B）静态/蒙皮**完全一致**，蒙皮仅**尾部追加** `boneWeights@44 / boneIndices@60(R8G8B8A8_UINT)`。
- **对数据驱动 PSO 的意义**：
  - 几何条件（硬）中"顶点格式"的判定简化为**尾部是否有骨骼字段**（`flags & Skinned`），头部恒定无需逐字段比对
  - 渲染器头部输入布局可复用 → PSO 集合按"尾部差异"产生变体，而非头部混乱造成互斥
  - 换渲染器（如蒙皮网格临时走 Opaque）时前 44 字节仍正确，不会灾难性错位——**即使 PSO 选错也只是性能/语义问题，不会是数据错乱**
- **资产重导**：`.dxmesh` 二进制写死顶点结构体顺序，改布局需重跑 AssetTool（`KD-03.dxmesh` 必须重导；静态网格 44B 不变无需重导）。

### 2.7 特殊渲染器保留管理器

阴影、天空盒不适合纯数据化，保留"管理器"形态（SkyboxManager 已有模式），且**保留实例有价值**：

- **阴影**：不是"画法差异"而是"管线结构差异"（多 pass、光源视锥剔除、级联矩阵）——是逻辑不是描述
- **天空盒**：全屏/半球特殊绘制 + 资源生命周期（SRV 堆域管理）——是资源管理不是描述
- **保留实例**：兜底（数据驱动渲染器失败/未注册时默认实例兜底）、默认使用（场景默认有天空盒/阴影）、性能（PSO 缓存驻留）

**双轨制**：
```
普通渲染器 → 数据驱动（.renderer JSON + 通用 PSO 工厂）→ 可热重载
特殊渲染器 → 管理器模式（ShadowManager/SkyboxManager，保留实例）→ 兜底 + 默认
```

---

## 三、大型引擎参考（2026-08-02 调研）

### 3.1 UE FMeshDrawCommand 机制

- `FMeshBatch` → `FMeshPassProcessor` → `FMeshDrawCommand`（**无状态绘制描述**，缓存 + 合并）
- `FMeshDrawShaderBindings`：记录绑定（`Add(Parameter, Value)`）→ `SetOnCommandList` 一次性提交
- `FMeshDrawCommandStateCache`：提交时**只设变化的绑定**（PipelineId/ShaderBindings/StencilRef/VertexStreams 缓存）
- **内联分配器**：存 10 个 shader bindings，溢出才堆分配
- **CachedMeshDrawCommands**：`AddToScene` 时预构建静态 mesh 的绘制命令，每帧只选命令不重建
- 合批条件：identical shader bindings（`MatchesForDynamicInstancing`）

### 3.2 Unity SRP 机制

- `ScriptableRenderContext` + `CommandBuffer`：**延迟执行**（构建命令列表 → `Submit()` 提交）
- `DrawRenderers(cullingResults, drawingSettings, filteringSettings)`：`ShaderTagId`（LightMode/Pass Tag）过滤
- `SetGlobalTexture`（nameID 句柄）：全局 shader 属性绑定，不塞结构体

### 3.3 对本项目的映射

| 我们的设计 | UE/Unity 对应 | 印证 |
|:-----------|:--------------|:-----|
| PSO 集合描述 | FMeshPassProcessor 选 shader + pipeline state | ✅ |
| bindings 静态/动态分离 | FMeshDrawShaderBindings + SetOnCommandList | ✅ |
| BeginFrame/Draw 拆分 | pass uniform buffer vs primitive 参数（GPUScene） | ✅ |
| 条件校验 | ShaderTagId + RenderType/Pass Tag 过滤 | ✅ |
| 绑定列表 | FMeshDrawCommand 内联 10 bindings / CommandBuffer 命令列表 | ✅ |

### 3.4 三个关键借鉴点（UE 踩坑后的成熟解法）

1. **绑定状态缓存（StateCache）**：提交时只设变化的绑定——"同 PSO 分批、绑定只切差异"的正式化，**实现时必须有**，否则每 item 全量设绑定浪费（⚠️ **2026-08-03 暂缓**：渲染器 ↔ system 一对一、每 system 独占命令列表，多 PSO 顺序执行需"自动组装 system + RDG"模式才出现（且 9 阶段划分进一步降低共用性）——当前无收益，实施留待 RDG 阶段）
2. **内联分配器（inline allocator）**：bindings 固定容量（如 8-16 槽位）内联，超限才走临时缓冲——符合项目规则"system 中不能内存分配"
3. **绘制命令缓存（CachedMeshDrawCommands）**：静态部分（材质/几何绑定）预构建缓存，只有 instanceBuffer 等逐帧变化才动态填充

---

## 四、可行性分层确认

### ✅ 已被项目实证（不是空想）

| 能力 | 实证依据 |
|:-----|:---------|
| 一渲染器多 PSO | ShadowRenderer 已持有 4 个 PSO |
| 几何条件决定输入布局 | SkinnedRenderer 输入布局含 BLENDINDICES（R8G8B8A8_UINT），Opaque 不含 |
| 材质槽 → 渲染路径 | MeshComponent.materialSlots[] + MaterialData.shaderType 已实现 |
| 渲染项异构收敛基础 | Opaque/Skinned RenderItem 高度同构，startIndex/indexCount 子网格绘制已支持 |

### ⚠️ 设计部分（可行但未落地，风险可控）

| 设计 | 风险点 | 可控性 |
|:-----|:-------|:-------|
| PSO 描述 JSON + 工厂缓存 | 描述 schema；PSO 重建/热重载 fence 同步 | 低（成熟 D3D12 路径） |
| bindings[] 绑定外置 | 类型安全（union 槽位）；性能 | 可控（槽位强类型 enum；同 PSO 分批，绑定量 <8） |
| UI 源头过滤 + 构建器兜底 | 编辑器材质槽过滤逻辑 | 低（同一条件数据源） |
| RenderContext 外观 | 无（get 透传 + 占位兜底） | 无风险 |

### ❌ 明确不做（避免过度设计）

- 行为层（绘制逻辑/多 pass 结构）**不数据驱动**——阴影多 pass、天空盒特殊绘制保持代码
- 渲染项**不大一统**成单一结构体——骨架统一 + bindings[] 差异进槽位
- 渲染器**不全部 JSON 化**——普通渲染器数据驱动，特殊渲染器管理器保留（兜底 + 默认）

### 全链路闭环检查点

1. ✅ 静态描述：PSO/根签名 JSON 化（ShadowRenderer 多 PSO 已实证）
2. ✅ 动态绑定：pass/item 绑定列表（BeginFrame/Draw 拆分）
3. ✅ 条件校验：几何条件（硬）∧ 材质变体（软），UI 源头过滤 + 构建器兜底
4. ✅ RenderContext：get 方法透传 + 占位兜底
5. ✅ 特殊渲染器保留管理器（阴影/天空盒不动）
6. ✅ 热重载（SnapshotSystem 已有，PSO 重建走 fence 同步）

**推进纪律：全链路确认可行性后才能实施，不能只做其中一段**（否则与现有硬编码绑定两套并存更乱）。

### 4.1 材质→渲染器路由（正交维度定案，2026-08-02 补充）

> 讨论触发：材质槽固化（#22/#24）后，"材质最终决定渲染器"成为自然走向——材质 shaderType 决定渲染路径，不再是实体组件标志（OpaqueTag/TransparentTag/SkinnedTag）驱动。大型引擎印证：UE `FMeshBatch → FMeshPassProcessor`（材质选 shader+pipeline）、Unity `DrawRenderers` 按 `ShaderTagId`（LightMode/Pass Tag）过滤。

**核心认知：子网格展开（几何切分）与材质→渲染器分发（渲染路径选择）是两个正交维度，不冲突**：

| 维度 | 内容 | 数据源 | 粒度 |
|:---|:---|:---|:---|
| **几何切分**（子网格展开） | `submesh[i]` = 索引区间 → SubMeshInfo{startIndex, indexCount} | .dxmesh SubMesh 表（无表 = 1 个整体，已兜底） | 网格内 |
| **渲染路径选择**（材质路由） | `material[i].shaderType` → 决定进哪个渲染器/阶段 | .mat 材质数据 | 子网格级 |

**组合效果**：一个网格 8 个子网格、材质各异 → 展开得 8 个渲染项，每个按材质分发到对应阶段（PBR 子网格→Opaque、透明子网格→Transparent、蒙皮材质→Skinned）。这与 UE/Unity 行为一致：**同一 StaticMesh 的多个 Section 走不同 pass**。子网格统一语义（§2.3/§4 前置，2026-08-02 完成）正是该模式的前提——展开产生渲染项，渲染项按材质分发。

**路由表设计**（材质→队列，替代组件标志过滤）：

```cpp
// 材质声明 → 渲染路径（shaderType 是路由键）
enum class ShaderType { PBR, NPR, Skinned, Transparent, Water, ... };  // 来自 .mat

// 路由：Builder 收集时按 shaderType 分发到对应 TRenderQueue
struct MaterialRoute {
    ShaderType shaderType;                    // 路由键（材质数据）
    RenderPhase phase;                        // 目标阶段（Opaque/Transparent/...）
    // 几何条件（硬）由网格数据校验，见 §2.6
};
```

**现状差异**：

| | 现状（标志模式，`EngineOverview.md` §四） | 目标（材质驱动） |
|:---|:---|:---|
| 选择依据 | 组件标志（OpaqueTag/TransparentTag/SkinnedTag） | 材质 shaderType（.mat 数据） |
| 网格多材质 | 整实体一个标志，无法细分 | 子网格级分发（已具备 materialSlots[]） |
| 校验 | 无 | 几何条件（硬）∧ 材质变体（软），UI 过滤 + Builder 兜底（§2.6） |
| 对应大型引擎 | — | UE FMeshPassProcessor / Unity ShaderTagId |

**过渡路径**：`MeshComponent.materialSlots[]` 已就绪；**L1 材质路由已落地（2026-08-02）**——新增 `ShaderRoute.h`（`ShaderType` 枚举 + `MaterialRoute` 路由表 + `ParseShaderType`），`OpaqueRenderItemBuilder` / `SkinnedRenderItemBuilder` 已改为按材质 shaderType 分发（view 去除 OpaqueTag/SkinnedTag，改为 `MeshComponent + TransformComponent` + 材质路由过滤）：

- **路由键来源**：`MaterialData.name` 即 .mat 的 shader 字符串（`MaterialLoadTask` L78），`ParseShaderType(name)` 前缀匹配（大小写不敏感，无内存分配）
- **Opaque**：`RoutesToOpaque`（PBR/NPR → opaque；Unknown fallback 默认 opaque 兼容历史资产）
- **Skinned**：`RoutesToSkinned`（shader 前缀 "skinned" → skinned；Unknown **不** fallback，防普通 PBR 误路由）+ **几何条件校验**（硬，§2.6：`TriangleMesh::IsSkinned()`）
- **子网格级分发**：SubMesh 展开时逐槽位材质路由，同一网格不同材质子网格自然进不同阶段（正交维度，§4.1 核心）

剩余：Transparent/Water 路由为预留（`MaterialRoute` 已登记，对应 Builder 后续迁移）；场景 JSON 的 `opaque: null` / `skinned: null` Tag 仍存在（未清理，作为过渡兼容），后续 L3 横向铺开时移除。逐步迁移见 §六。

### 4.1a 材质组件化（L1.5 定案，2026-08-02 补充，**已废弃**）——被 §4.1b/c/d 缓存分桶取代

> ⛔ **废弃声明（2026-08-02）**："组件类型 = pass"（PBR/Skinned/Transparent 各一材质组件）被 §4.1b 证伪——把廉价的查找做成了类型差异（组件类型爆炸），而真正昂贵的计算（InstanceData 矩阵/合批）反而没被独立出来。代码中已落地的 `PBRMaterialComponent/SkinnedMaterialComponent/TransparentMaterialComponent` 为**过渡实现，随缓存分桶迁移回退移除**；SceneConstructor 改为填充单个 `RenderSlotComponent`（Slot.shaderType = 渲染器标记），桶由 `RenderSlotCache` 自动按渲染器标记分/重建（§4.1c/d）。本节保留仅作演进记录。

> 讨论触发（历史）：L1 的 `RoutesToOpaque/RoutesToSkinned`（实体 view + 槽位过滤）是**补丁式**——筛选粒度（实体）≠ 产出粒度（子网格），复数 Builder 各扫全量实体做运行时过滤，材质仍是 `MeshComponent.materialSlots[]` 附属数组，非可查找的一等公民。当时定案：**实体 = 整个网格，挂载多个与渲染器对应的材质组件（内含子网格信息）**——子网格仍是整体，材质组件类型即路由结果（结构静态化，替代运行时过滤）。大型引擎印证：Unity DOTS 渲染的 `MaterialMeshInfo`（实体持 MeshID+MaterialID，渲染按材质分组收集）正是同款架构。

**核心模型**：

```cpp
// 实体 = 整个网格（transform/剔除/生命周期仍在实体级，子网格不拆实体）
struct MeshComponent {
    Resource::LODMeshHandle lodMeshHandle;
    bool receivesShadow = true;
    // materialSlots[] 数组废弃 → 由挂载的材质组件取代
};

// 子网格区间（本材质覆盖的索引段，可多个：同材质重复槽位）
struct SubMeshRange { uint32_t startIndex; uint32_t indexCount; };

// 材质组件 = 渲染槽位（一个实体可挂多个，按渲染器种类区分组件类型）
struct PBRMaterialComponent {
    Resource::MaterialHandle material;        // 材质句柄
    std::vector<SubMeshRange> subMeshRanges;  // 本材质覆盖的子网格区间
};
struct SkinnedMaterialComponent {
    Resource::MaterialHandle material;
    std::vector<SubMeshRange> subMeshRanges;
};
struct TransparentMaterialComponent { /* 同构 */ };
// ... 渲染器种类 = 组件类型集合（有限集，非每材质一种，不会爆炸）
```

**关键设计**：

| 项 | 内容 |
|:---|:---|
| **组件类型 = pass（渲染器）** | PBR/Skinned/Transparent... 对应渲染器，静态差异在类型层 |
| **组件字段 = 变体（variant）** | shaderType/blendMode（.mat 数据）对应 PSO 变体，动态差异在值层（复用 §2.1 分层） |
| **并行** | 各 Builder `view<MeshComponent + 自己的材质组件>` **互不重叠** → 调度器天然并行（ECS 并行 system 正统前提） |
| **查找能力** | ECS 原生组件 view 即查找表（替代 B 方案手工缓存表） |
| **几何条件前置** | SkinnedMaterialComponent 挂载时校验 `IsSkinned()`（§2.6 硬条件），不合法不挂——错误在挂载期暴露 |
| **子网格整体性** | 子网格信息内聚在材质组件（subMeshRanges），实体语义不拆分 |
| **扩展性** | 新增渲染器 = 新增组件类型 + Builder 注册 + PSO 描述，不动现有 Builder（开放封闭） |

**构建器注册表（数据驱动）**：

```cpp
// 渲染器/构建器注册：声明关注的材质组件类型（模板/类型注册）
struct RendererRegistration {
    std::type_index materialComponentType;   // 关注的材质组件类型
    std::string rendererName;                // 目标渲染器（opaque/skinned/...）
    const char *defaultVariant;              // 默认 PSO 变体
};
// Builder 侧：view<MeshComponent, T> 按注册的组件类型收集（无运行时过滤）
```

**与 ShaderRoute 的关系**：`MaterialRoute` 表 = "shaderType → 渲染器名"（§4.1）；shaderType 作为 `RenderSlotComponent.Slot` 的**渲染器标记字段**（§4.1b），`RenderSlotCache` 桶随它自动分/重建（§4.1c/d）；L1 的 `RoutesToOpaque/Skinned` 补丁式过滤随迁移移除。

**四端同步**（#23 规则）：`MeshDesc.materials[]`（JSON 数组）→ SceneConstructor 按 shaderType **分发挂载**到对应材质组件（替代 materialSlots[] 数组）；`ExportToDescription` 反向聚合；`SceneLoader::ParseMesh` 读取不变。迁移是**结构替换**，非叠加。

**合批键 = 实例化键（2026-08-02 补充）**：材质组件化后，BatchKey 的"子网格分组"维度冗余（`slot.subMeshRanges` 已固化"材质→区间"），合批键语义退化为**跨实体实例化**——相同 (geometry + materialIdx + 区间) 的多个实体合并实例化（DrawCall N→1，与 UE identical bindings / Unity MaterialMeshInfo 分组一致）。要点：

- **区间维度必须保留**：`DrawIndexedInstanced(startIndex, indexCount, instanceCount)` 一次只画一个区间，不同区间不能合并——合批键 = `{geometry, materialIdx, startIndex, indexCount}`
- **startVertex 维度移除**：恒为 0（§2.3 绝对索引语义，BaseVertexLocation 恒 0），从合批键/hash/compare 中删除，仅保留记录语义
- **跨实体实例化价值保留**：合批粒度 = 材质组件粒度，场景规模上去（大量同类实体）后是标配收益（L2 渲染项自包含时随渲染项字段自然简化）

### 4.1b 方向修正（2026-08-02 定案，**唯一定案**）——通用渲染槽位组件 + 集中粗筛选 + 并行计算

> ✅ **唯一定案**：§4.1b/c/d 是渲染项收集的终态定案（通用组件 + 变更驱动缓存 + 分桶分发），§4.1a 材质组件模式废弃（见废弃声明）。

> **修正 §4.1a 的"组件类型 = pass"设计**。讨论触发：**渲染器的并行压力在于计算（InstanceData 矩阵/合批），不在于找材质项**（ECS view 查找廉价）。因此"按渲染器添加材质组件类型"（PBR/Skinned/Transparent 各一）是**错误行为**——把廉价的查找做成了类型差异（组件类型爆炸），而真正昂贵的计算（每实例矩阵、世界逆转置、合批键哈希）反而没被独立出来。

**正确模式：集中粗筛选（廉价赋值）+ 并行计算（昂贵计算）分离**——同 CPU broad-phase/narrow-phase、同 CullingSystem 的 CulledSet / LODSystem 的临时结构模式：

```
ECS 组件遍历（集中粗筛选，廉价）
  for entity in view<MeshComponent, RenderSlotComponent>():
    临时结构.push_back({ entityId, subMeshRanges, 选择的渲染器 })   // 简单赋值，无复杂计算
      ↓
并行构建器（复杂计算）
  for slot in 临时结构:                                            // 可并行
    计算 InstanceData（矩阵/世界逆转置/材质索引/合批键哈希）          // 昂贵计算
      ↓
FrameSync 统一上传
```

**设计**：

```cpp
// 通用渲染槽位组件（一个组件，不分渲染器类型）——存子网格数据 + 被选择的渲染器
struct RenderSlotComponent {
    struct Slot {
        Resource::MaterialHandle material;        // 材质句柄
        std::vector<SubMeshRange> subMeshRanges;  // 本槽覆盖的子网格区间
        ShaderType shaderType;                    // 选择的渲染器（字段值，非组件类型）
    };
    std::vector<Slot> slots;                      // 本实体的全部渲染槽位
};

// 集中粗筛选输出（临时结构，CPU 粗筛；类比 CulledSet）
struct SlotCandidate {
    ECS::Entity entityId;          // 宿主实体（取 transform）
    std::vector<SubMeshRange> ranges;  // 基本子网格信息（简单赋值，不计算）
    ShaderType shaderType;         // 渲染器选择
};
// 集中步骤产出 SlotCandidate[]（遍历 + 赋值），丢给并行 Builder 做复杂计算
```

**关键认知**：

| 项 | §4.1a（组件类型 = pass，修正前） | §4.1b（通用组件 + 集中粗筛选，修正后） |
|:---|:---|:---|
| 渲染器选择 | **组件类型**（PBR/Skinned/Transparent 各一）→ 类型爆炸 | **字段值**（Slot.shaderType）→ 一个组件 |
| 查找 | Builder `view<组件类型>`（廉价，但分散在各 Builder） | 集中遍历 view<RenderSlotComponent>（一次） |
| 计算 | 与查找混在 Builder（每 Builder 重复 InstanceData 计算） | **集中粗筛选后并行计算**（Builder 只算，不找） |
| 新增渲染器 | 新增组件类型 + Builder | 新增 shaderType 枚举值 + 配对注册（§4.2a），组件零改动 |
| 并行 | Builder 间互斥（view 不同） | **临时结构分块并行**（同模式内并行，粒度更细） |

**与现有落地的衔接（2026-08-02 更新）**：§4.1a 的 `PBRMaterialComponent/SkinnedMaterialComponent/TransparentMaterialComponent` **已废弃并回退移除**；`RenderSlotComponent` 是**唯一定案**——SceneConstructor 填充单个组件（Slot.shaderType 即渲染器标记，四端同步 #23），Builder 消费缓存桶。迁移见 §六。

### 4.1c 两层筛选 + 变更驱动缓存表（2026-08-02 定案）——§4.1b 的精确化

> **修正 §4.1b 的"每帧集中遍历 ECS 产出临时结构"**。讨论触发：**我们的筛选不是可见性筛选，而是材质所关心的管线筛选**——目标不同（可见性 = 不渲染不可见；管线筛选 = 渲染项归属哪个渲染器），概念必须分开。**从八叉树筛选后的结果再筛材质是合理的**（两层串联），但**管线筛选是变更驱动（事件驱动）而非每帧**——需要**缓存机制**。编辑器端实体**增删查改**（CRUD），Game 端实体**只增删**，变更驱动更新缓存。最终：**与渲染器匹配的构建器配合剔除结果（CulledSet）查缓存表中的渲染项信息**。

**两层筛选，目标不同**：

| 筛选 | 目标 | 频率 | 驱动 |
|:---|:---|:---|:---|
| **可见性筛选**（八叉树/视锥，CullingSystem） | 不渲染不可见的 | 每帧 | 时间驱动 |
| **材质管线筛选**（材质 → 渲染器） | 渲染项归属哪个渲染器 | **变更时** | **事件驱动** |

**流水线：`八叉树 → CulledSet（可见集）→ 材质管线（查缓存表）→ 并行计算 InstanceData`**

**变更驱动缓存表**：

```cpp
// 缓存表 = 材质管线筛选的驻留结果（事件驱动更新，非每帧重建）
// key: entityId → 该实体的渲染槽位列表（含 shaderType）
class RenderSlotCache {
    // 编辑器端：实体增删查改（CRUD）→ 变更时更新
    void OnEntityAdded(ECS::Entity e);     // 增
    void OnEntityRemoved(ECS::Entity e);   // 删
    void OnEntityModified(ECS::Entity e);  // 改（编辑器查改；Game 端只增删无改）
    // 查表（Builder 用）：实体 → 槽位列表
    const std::vector<RenderSlotComponent::Slot> *Lookup(ECS::Entity e) const;
};
```

**Builder 流程（查缓存表，非遍历 ECS）**：

```
每帧：
  ① CullingSystem → CulledSet（可见实体，廉价，已有）
  ② Builder（与渲染器匹配）：遍历 CulledSet → 查 RenderSlotCache
     → 得到渲染项信息（子网格/材质/shaderType）
  ③ 并行计算 InstanceData（昂贵，narrow-phase）
```

**编辑器 vs Game 端变更**：

| 端 | 实体操作 | 缓存更新 |
|:---|:---|:---|
| **编辑器** | 增删查改（CRUD 全量，属性卡/大纲面板操作） | 增/删/改均触发缓存更新 |
| **Game** | 只增删（玩家 spawn/销毁，无运行时属性修改） | 仅增/删触发 |

**与大型引擎对齐**：UE `FStaticMeshSceneProxy` 的 `FStaticMeshBatch[]` 在 **AddToScene（变更时）** 构建并驻留，每帧只做可见性筛选 + 提交缓存命令——**槽位驻留（事件驱动）+ 每帧可见性筛选**正是本形态。§4.1b 的 `RenderSlotComponent`（通用槽位，字段 shaderType）**保留**（组件设计对），"集中粗筛选步骤"改为**复用 CulledSet + 查缓存表**（非每帧重建临时结构）。

### 4.1d 分桶 + 分发（2026-08-02 定案，**终态**）——避免全量遍历，开销与 ECS view 同量级

> ✅ **终态落地要点（2026-08-02）**：
> - **缓存表与桶分离**：`RenderSlotCache` 内部两层——**缓存表**（实体 → 槽位，驻留，**CRUD 驱动重建**：实体增删查改时 `MarkDirty`，BuilderUpload 每帧仅检查 `IsDirty` 才 `Rebuild`）与**桶**（shaderType → 可见条目，每帧 `Dispatch` 由 CulledSet × 缓存表派生）
> - **桶键 = shaderType（渲染器标记）**：按 `RenderSlotComponent::Slot.shaderType` 自动分桶；实体增删改（事件驱动）自动重建对应桶，无需手动维护
> - **桶内容即粗筛可见**：**无 visible 标志**——`Dispatch` 直接把八叉树粗筛可见实体写入桶，Builder 消费桶时不做可见性判断
> - **CulledSet 仅八叉树粗筛**：**Builder 消费桶时仍在粗筛基础上做精确筛选**（视锥 FrustumCull + LOD 选择），`m_frustum` 保留，`m_entityFilter` 移除
> - **实体过滤保留到分发前**：编辑器端 `SceneTagComponent` 场景过滤在 `RenderSlotCache` 分发时执行（按 sceneId），Builder 保持无过滤状态
> - **Builder 遍历自己的桶**（子集，非全量）；InstanceData 计算仍可并行（§4.1b）

> **修正 §4.1c 的"Builder 遍历 CulledSet 查表 return"**。讨论触发：**开销担忧**——朴素方案每 Builder 遍历剔除后**全量** CulledSet + 每实体哈希查表 + 大部分 continue（N 个 Builder → N × 可见实体数），比 ECS view 子集遍历差。**大型引擎不做全量遍历**：UE 命令按 pass 分桶驻留（`per-pass` 命令缓存），可见性筛选后**一次分发到桶**，每 pass 只遍历自己的桶（子集）。

**核心修正：RenderSlotCache 按 shaderType 分桶（驻留），可见结果分发到桶，Builder 遍历自己的桶（子集，非全量）**：

```cpp
// 分桶缓存：shaderType → 该渲染器的实体+槽位列表（驻留，事件驱动更新）
class RenderSlotCache {
    std::unordered_map<ShaderType, std::vector<SlotEntry>> m_buckets; // 桶 = 渲染器
    // 变更时：实体增删改 → 从旧桶移除 → 按槽位 shaderType 加入新桶（事件驱动，非每帧）
    void OnEntityAdded(ECS::Entity e);
    void OnEntityRemoved(ECS::Entity e);
    void OnEntityModified(ECS::Entity e); // 编辑器查改；Game 端只增删
};

// 每帧：CullingSystem 输出可见实体 → 一次分发到桶（廉价赋值）
//   for entity in CulledSet:
//      for b in cache.GetBuckets(entity):     // 该实体在哪些桶（可能多槽位多种 shaderType）
//          b.visible.push_back(entity);        // 简单赋值，无复杂计算
// 然后各 Builder 只遍历自己的桶（子集，非全量）：
//   OpaqueBuilder 遍历 bucket[PBR].visible → 并行计算 InstanceData
```

**开销对比（修正后）**：

| 项 | 现状（ECS view） | 分桶 + 分发（修正后） |
|:---|:---|:---|
| Builder 遍历 | view 子集（PBR 实体，ECS 存储稀疏集命中） | **bucket[PBR].visible（PBR 可见实体）——等价子集** |
| 全量遍历 | 无 | 仅 CullingSystem 一次（已有，廉价） |
| 分发到桶 | — | O(可见实体 × 槽位数)，每帧一次廉价赋值 |
| 总开销 | Σ(各子集) | CulledSet 一次 + Σ(各桶) ≈ **同量级** |

**关键**：Builder 遍历的仍是**自己的子集**（分桶后），**不是全量**——开销与 ECS view 同量级，且**不重复全量遍历**（CullingSystem 分发一次，各 Builder 各取各桶）。这与 UE per-pass 命令缓存同构：**分桶驻留（变更驱动）+ 每帧一次分发 + Builder 子集计算**。

### 4.2 渲染项自包含（无状态绘制描述，2026-08-02 补充）

> 讨论触发：子网格 + 对应材质 = **完整渲染项**。渲染器 Draw 阶段不应再通过"几何-材质-纹理"三系统取信息（现状：`OpaqueRenderer::DrawInstancedGBuffer` 仍需 `GetGeometry<TriangleMesh>` + `GpuResourceManager::GetResource` 取 VB/IB）。目标：**VB/IB 地址、索引区间、材质索引等在 Builder 构建时固化为渲染项字段，Draw 阶段零查询、直接消费**。

**现状（Draw 阶段查询，待消除）**：

```cpp
// OpaqueRenderer::DrawInstancedGBuffer（现状）
const TriangleMesh *mesh = m_geometryManager->GetGeometry<TriangleMesh>(geometryHandle); // ← 查询
ID3D12Resource *vb = gpuMgr.GetResource(mesh->vertexBufferHandle);                        // ← 查询
ID3D12Resource *ib = gpuMgr.GetResource(mesh->indexBufferHandle);                        // ← 查询
// 再组装 vbView/ibView → IASetVertexBuffers/IASetIndexBuffer
```

**目标（渲染项自包含，UE FMeshDrawCommand 同款）**：

```cpp
// 渲染项在 Builder 构建时固化全部绘制所需信息
struct RenderItem {        // OpaqueRenderItem 扩展（SkinnedRenderItem 同理）
    GeometryHandle geometryHandle;   // 保留（用于校验/诊断），但 Draw 不再解引用
    D3D12_GPU_VIRTUAL_ADDRESS vbAddress;  // ← Builder 已解析
    uint32_t vbStride;                     // ← Builder 已解析
    D3D12_GPU_VIRTUAL_ADDRESS ibAddress;   // ← Builder 已解析
    uint32_t ibSizeBytes;                  // ← Builder 已解析
    uint32_t indexFormat;                  // ← Builder 已解析
    uint32_t startIndex; uint32_t indexCount; // 子网格索引区间（已具备）
    uint32_t materialIndex;                // 材质 GPU 索引（已具备）
    D3D12_PRIMITIVE_TOPOLOGY topology;     // ← Builder 已解析
    // instanceBuffer / boneBufferAddress 已具备
};
```

**收益**：

| 项 | 现状 | 目标 |
|:---|:---|:---|
| Draw 阶段查询 | `GetGeometry` + `GetResource`（每 item 两次查询 + 判空） | 零查询，直接消费渲染项字段 |
| 判空/兜底 | 每个渲染器重复写 `!mesh || !isGpuReady` 守卫 | Builder 构建时一次校验，无效项不产出 |
| 与材质路由协同 | — | 材质→渲染器路由（§4.1）决定进哪个队列；渲染项字段由该队列对应渲染器消费 |
| 对应大型引擎 | — | UE `FMeshDrawCommand`（无状态绘制描述，缓存 + 合并）/ Unity `CommandBuffer` 命令列表 |

**关键点**：

- **GPU 资源生命周期仍是引用计数**（`GpuResourceManager`）：Builder 固化的是 **GPU 虚拟地址**（`D3D12_GPU_VIRTUAL_ADDRESS`），不是 `ID3D12Resource*` 指针——地址在资源存活期间有效，不增加生命周期耦合；资源释放后地址失效由"引用计数保证资源存活于渲染项消费前"兜底（与 §五 FrameSync 多缓冲一致）。
- **isGpuReady 校验前移到 Builder**：异步加载未完成的网格，Builder 构建时 `!isGpuReady` 直接不产出渲染项（与 §7.4 实体级条件入图同源），渲染器不再需要守卫。
- **SkinnedRenderItem 同理**：`vbAddress/ibAddress/indexFormat` 固化，`boneBufferAddress` 已具备，Draw 阶段同样零查询。
- **不违背"构建器只录制命令"约束**：构建器固化的是**数据字段**（地址/区间），不是命令；渲染器仍负责 `IASet*` + `DrawIndexedInstanced`（规则 §7：system 录制命令，构建器只收集）。

### 4.2a 渲染项组合约束（2026-08-02 定案）——公共字段提升 + 类型隔离 + 显式配对

> **演进声明（2026-08-03）**：本节的"类型隔离"（Opaque/Skinned 各一渲染项类型）已被 **§4.2a1 全可能大渲染项 + bindings 模式**演进取代——渲染项 = 资源的投影（ECS 组件推断），字段 = JSON 可选部分（组件存续决定用哪些），无效字段直接不绑定。本节保留作 2026-08-02 定案历史与约束动机记录。

> 讨论触发（2026-08-02 历史）：渲染项（Opaque/Skinned）差异大（`probeIndex` vs `boneBufferAddress` + 不同 PSO/输入布局），强行大一统需引入运行时 `passTag` 路由字段，反而更脆弱。**核心担忧：渲染项与渲染器是否存在随意组合的可能性**——当时答案：不强行大一统，公共字段提升 + 类型隔离双保障组合约束（大型引擎同构：UE 统一命令结构 + pass 处理器配对、Unity 材质 pass 声明）。

**设计：公共字段提升 + 类型隔离（不强行大一统）**：

```cpp
// 公共字段提升（所有渲染项共享，§4.2 自包含在这里）
struct RenderItemCommon {
    Resource::GeometryHandle geometryHandle;   // 校验/诊断
    uint32_t materialIndex;
    D3D12_GPU_VIRTUAL_ADDRESS instanceBuffer;
    uint32_t instanceCount;
    // §4.2 自包含：vbAddress/vbStride/vbSizeBytes/ibAddress/ibSizeBytes/indexFormat/topology
    uint32_t startIndex; uint32_t indexCount;
    uint32_t tempSlot;
};

// 差异字段留在各自类型（类型隔离 = 组合约束）
struct OpaqueRenderItem : RenderItemCommon { uint32_t probeIndex; };          // 反射
struct SkinnedRenderItem : RenderItemCommon { D3D12_GPU_VIRTUAL_ADDRESS boneBufferAddress; }; // 骨骼
```

**组合约束两重保障**：

| 保障 | 机制 | 时点 |
|:---|:---|:---|
| **类型层** | `TRenderQueue<T>` 强类型队列：OpaqueRenderer 签名只收 `TRenderQueue<OpaqueRenderItem>&`，编译器阻止 Skinned 队列传入 | 编译期 |
| **配对表** | `RendererPairing` 显式声明"渲染项类型 ↔ 渲染器"（渲染器管理器注册时一并声明，§2.2b） | 注册期（数据驱动） |

```cpp
// 显式配对表（替代隐式"碰巧类型匹配"约束，渲染器管理器消费）
struct RendererPairing {
    std::type_index itemType;   // OpaqueRenderItem / SkinnedRenderItem
    const char *rendererName;   // "opaque" / "skinned"（renderer.json 键）
    const char *defaultVariant; // 默认 PSO 变体（gbuffer）
};
```

**效果**：
- **公共字段共享**：`ApplyBindList`/Draw 辅助只依赖 `RenderItemCommon` 字段，泛型复用（VB/IB 自包含、实例化合批逻辑一份）
- **类型隔离保持组合约束**：`TRenderQueue<T>` 编译期阻止随意组合（Opaque 不能消费 Skinned）
- **配对表显式化**：让"谁消费谁"成为**数据驱动注册时的显式声明**（与 `RendererRegistration`（§4.1a）并列），不再隐式——新增渲染器 = 新增配对 + 描述 + 组件，开放封闭

**与大型引擎对齐**：UE `FMeshDrawCommand` 统一命令结构 + `FMeshPassProcessor` 按 pass 配对（谁产生谁消费）；我们用"公共字段 + 类型隔离 + 注册表配对"实现同构约束——UE 用运行时路由，我们用类型 + 注册表（编译期 + 注册期双保险）。

**落地状态（2026-08-02）**：`RenderItemCommon.h`（公共字段基类，含 §4.2 自包含字段）、`OpaqueRenderItem : RenderItemCommon`（差异 probeIndex）、`SkinnedRenderItem : RenderItemCommon`（差异 boneBufferAddress）、`RendererPairing.h`（RenderItemKind + 配对表：Opaque↔opaque/gbuffer、Skinned↔skinned/gbuffer）均已落地——组合约束双保险（类型层 + 配对表）基础设施就绪，渲染器管理器（§2.2b）注册时消费。

### 4.2a1 渲染项统一定案（2026-08-03）——全可能大渲染项 + bindings 模式（字段无效即不绑定）

> **演进取代 §4.2a 的类型隔离**。讨论触发：渲染项差异字段（`probeIndex`/`boneBufferAddress`）本质都是**可选资源槽位**，且都可由 ECS 组件推断（`TryGetComponent<ReflectionConsumerComponent>` / `TryGetComponent<SkinnedComponent>`）。既然**渲染项 = 资源的投影**（ECS 组件 → 管理器资源 → 渲染项字段），就不必为每个 PSO/渲染器定义渲染项类型——**一个全可能大渲染项**（字段集 = JSON 可选部分，组件存续决定用哪些），无效字段直接不绑定。大型引擎印证：UE `FMeshDrawCommand` 统一命令结构 + `FMeshDrawShaderBindings`（Add 只记录实际绑定，未设置即不消费）。

**核心模型**：

```cpp
// 全可能大渲染项 —— 取代 Opaque/Skinned/Transparent 类型隔离
struct RenderItem : RenderItemCommon {
    // ── 可选字段（JSON 可选部分类比：组件存续决定是否有效） ──
    uint32_t probeIndex = UINT32_MAX;                // ReflectionConsumerComponent → 有效（无=无效）
    D3D12_GPU_VIRTUAL_ADDRESS boneBufferAddress = 0; // SkinnedComponent → 有效（无=无效）
    // ... 未来差异字段按需追加（水/地形/透明排序等），均为"无效值缺省"
};
```

**bindings 模式（UE FMeshDrawShaderBindings 同款）**：

| 环节 | 机制 |
|:---|:---|
| **Builder 推断** | `TryGetComponent<SkinnedComponent/ReflectionConsumerComponent>` → 填对应字段；组件存续 = JSON 键存在，缺省 = 无效值 |
| **渲染器消费** | `ApplyBindList` 按字段有效性**选择性绑定**——`boneBufferAddress != 0` 才绑骨骼槽、`probeIndex != UINT32_MAX` 才绑探针槽；**字段无效直接不绑定** |
| **PSO 选择** | 仍由 renderer.json `variants[].conditions`（requireSkinned/shaderType）决定，同一渲染项可进不同变体 |

**组合约束从类型层移到注册期**：

| 保障 | §4.2a（类型隔离，2026-08-02） | §4.2a1（全可能渲染项，2026-08-03） |
|:---|:---|:---|
| **类型层** | `TRenderQueue<OpaqueRenderItem>` 编译期阻止误传 | 统一 `TRenderQueue<RenderItem>`，约束移到注册期 |
| **配对表** | `RendererPairing` 声明"渲染项类型 ↔ 渲染器" | `RendererPairing` 声明"渲染项 ↔ 渲染器 ↔ **消费的槽位清单**"（注册期校验） |
| **兜底** | — | `itemKind` 字段（Opaque/Skinned/...）供配对表校验与诊断 |

**与大型引擎对齐**：UE `FMeshDrawCommand` 统一结构 + `FMeshDrawShaderBindings` 记录差异绑定；我们用"全可能渲染项 + 字段有效性绑定"实现同构——组件推断产生字段（谁提供资源），ApplyBindList 按有效性消费（谁用资源）。

**收益**：
1. **不必为每个 PSO 定义渲染项**——一个 `RenderItem` 配多个变体（gbuffer/shadow/depth prepass），PSO 由 conditions 选
2. **新增渲染器零渲染项改动**——只加 PSO 变体 + Builder 推断，开放封闭（§2.1a 推论延伸）
3. **字段 = 槽位**：渲染项字段最终都绑定到管线槽位，与 renderer.json 的 `rootSignature.params` 一一对应（§2.2 全声明，见下）

**代价（已评估可接受）**：失去编译期类型安全 → 由配对表注册期校验 + `itemKind` 兜底；结构含少量冗余字段（probeIndex/boneBufferAddress 共 12 字节左右）→ 换取统一性与扩展性。

---

## 五、资源生命周期保障

- **重建在主线程固定阶段**：LightManager 的 `RebuildShadowConstants`（方向光每帧/点聚光脏时）在 FrameDriver 的 Update 阶段执行——主线程、阶段固定
- **FrameSync 多缓冲**：L4 交换回调保证 GPU 完成前不回收资源
- RenderContext 的 get 在**渲染提交阶段**被调用时，管理器状态是当前帧已更新的最新值，句柄帧内有效

**注意点**：`LightManager` 有 `GpuResourceManager::Release(handle, 0)`（fence 传 0 = 立即释放语义），若重建释放走该路径，理论上存在 GPU 采样旧资源的悬空窗口——属既有资源释放问题（待办 #49 范畴），与 RenderContext 设计解耦，实现时一并修正。

---

## 六、推进路线图（难易度分级 + 步骤划分，2026-08-02 更新）

> 近期目标（材质槽模式 #22/#23/#24 + 子网格统一语义）已于 2026-08-02 完成，本路线图由此向下展开。

### 6.1 难易度总览

| 档位 | 内容 | 难度 | 风险 | 前置 |
|:---|:---|:---:|:---:|:---|
| **L0 纯定义层** | 绑定槽位强类型 enum + BindValue/绑定列表结构（§2.2/§2.4）；PSO 描述 JSON schema 定稿（§8） | ⭐ | 零（纯新增，不碰现有路径） | 无 |
| **L1 材质路由** | `ShaderType` 枚举 + `MaterialRoute` 路由表（§4.1）；Builder 按材质 shaderType 分发到 TRenderQueue | ⭐⭐ | 低（TRenderQueue 已有，只改收集逻辑） | L0 的 enum |
| **L1.5 缓存分桶**（原"材质组件化"已废弃） | 通用 `RenderSlotComponent`（Slot 含 material + subMeshRanges + shaderType，shaderType = 渲染器标记）+ `RenderSlotCache` 按 shaderType 分桶（事件驱动更新）+ CulledSet 分发 + Builder 消费桶（§4.1b/c/d） | ⭐⭐ | 低-中（结构替换三材质组件 + materialSlots[]，四端同步） | L1 |
| **L2 试点数据化** | 选 Opaque/Skinned 渲染器走通"描述→PSO 工厂→bindings"全链路（§4 闭环检查点）+ **渲染项自包含**（VB/IB 地址等固化为渲染项字段，Draw 阶段零查询，§4.2）+ **渲染项统一**（全可能大渲染项 + bindings 模式，§4.2a1） | ⭐⭐⭐ | 中（动 PSO 创建路径 + 渲染项结构） | L0 + L1 |
| **L3 横向铺开** | 其余普通渲染器（Transparent/Water/Billboard）迁移；UI 源头过滤（材质槽按几何条件过滤，§2.6） | ⭐⭐⭐ | 中（逐个迁移 + 回归） | L2 |
| **L4 收尾** | RenderContext 外观（get 透传 + 占位兜底，§2.5）；热重载（SnapshotSystem + PSO 重建 fence 同步）；`Release(handle,0)` fence 修正（§五注意点，#49） | ⭐⭐⭐⭐ | 中高（热重载需 fence 同步） | L3 |

### 6.2 步骤划分（逐步推进）

| 步骤 | 内容 | 说明 |
|:---|:---|:---|
| **Step 1（L0）** | 绑定槽位 enum + BindValue 结构 | 纯新增定义，一次会话可完成，所有渲染器共用地基 |
| **Step 2（L0）** | PSO 描述 JSON schema 定稿（geometryConditions + variants） | 纯文档产出 + 1 个渲染器描述 JSON 样例，与 Step 1 可并行 |
| **Step 3（L1）** | ShaderType 枚举 + MaterialRoute 路由表 + Builder 按材质分发 | 材质 shaderType 决定渲染路径，替代组件标志；子网格展开结果按材质路由（✅ 2026-08-02 完成：ShaderRoute.h + Opaque/Skinned Builder 分发） |
| **Step 3.5（L1.5，2026-08-02 定案修订）** | 缓存分桶（取代材质组件化）：定义通用 `RenderSlotComponent`（Slot 含 material + subMeshRanges + shaderType，shaderType = 渲染器标记）→ SceneConstructor 填充单个组件（替代三材质组件分发挂载 + materialSlots[]，四端同步 #23）→ 新增 `RenderSlotCache`（按 shaderType 分桶，事件驱动增删改）→ CulledSet 分发到桶（Editor 场景过滤保留到分发前）→ Builder 消费桶 → 移除 `PBRMaterialComponent/SkinnedMaterialComponent/TransparentMaterialComponent` + RoutesToOpaque/Skinned 补丁 | 桶跟随渲染器标记自动分/重建；组件查找廉价（ECS view），昂贵计算（InstanceData）留在 Builder 并行（§4.1b/c/d） |
| **Step 4（L2）** | 试点 Opaque/Skinned 数据化：描述→PSO 工厂→bindings→StateCache + **渲染项自包含**（VB/IB 地址/格式固化进渲染项，Draw 阶段零查询，§4.2） | ✅ **大部分完成（2026-08-02）**：`PSOFactory.h/.cpp`（描述→PSO 缓存，输入布局按 requireSkinned 选型 §2.6a）；`BindSlot.h` BindValue/BindList 落地 Opaque BeginFrame/Draw（ApplyBindList 语义槽位→物理索引）；OpaqueRenderItem 固化 VB/IB 地址/格式 + DrawInstancedGBuffer 零查询路径（vbAddress 非 0 直接消费，0 回退过渡）；**OpaqueRenderer 已接入工厂**（CreateGBufferPSO 改走 `GetOrCreatePSO("opaque","gbuffer")`，`Content/Renderers/opaque.renderer.json` 匹配实际 5 RT + D32_FLOAT）；**根签名 JSON 化（§2.2a）**：`RootParamDesc` 扩展 reg/space/rangeCount，`PSOFactory::GetOrCreateRootSignature/BuildRootSignature`（RootParamDesc → CD3DX12_ROOT_PARAMETER + 6 静态采样器），`CreateGBufferRootSignature` 改走工厂。剩余：StateCache 待补（§3.4 借鉴点 #1；2026-08-03 评估暂缓——渲染器↔system 一对一独占命令列表，多 PSO 顺序执行待 RDG 阶段，见 §3.4 标注） |
| **Step 4.5（L2.5，2026-08-03 定案）** | 渲染项统一 + JSON 槽位全声明：全可能大渲染项（§4.2a1，`RenderItem : RenderItemCommon` 取代 Opaque/Skinned 类型隔离）→ Builder 按组件推断可选字段（`TryGetComponent<Skinned/ReflectionConsumer>`）→ `ApplyBindList` 按字段有效性选择性绑定（无效即不绑定）→ renderer.json `rootSignature.params` 全声明可能槽位（§2.2 补充：PassCB/LightCB/WaterCB/BoneBuffer... 不限于 PassConstants）→ `RendererPairing` 扩展"渲染项 ↔ 渲染器 ↔ 消费槽位清单"（注册期校验替代类型隔离） | 槽位集 = 渲染项字段集；新增渲染器零渲染项改动（只加 PSO 变体 + Builder 推断）；组合约束从编译期类型移到注册期配对表（§4.2a1） |
| **Step 5（L3）** | 其余渲染器迁移 + UI 源头过滤 | 逐个迁移，材质槽按几何条件过滤防错 |
| **Step 6（L4）** | RenderContext 外观 + 热重载 + fence 修正 | 收尾，热重载依赖 PSO 工厂 |

> **推进纪律不变（§4）**：全链路确认可行性后才能实施，不能只做其中一段——Step 3/4 必须一起评估，避免"材质路由改一半 + 硬编码标志并存"的混乱。

---

## 七、RDG 展望（屏障推导与并行录制，2026-08-02 补充）

RDG（渲染图）对我们不是"必做"，而是 PSO 数据化后的**自然延伸**——但价值层级必须分清：

### 7.1 RDG 价值分层（关键认知）

| 层级 | 做法 | 价值 | 结论 |
|:-----|:-----|:-----|:-----|
| **L1 自动化** | 自动在 system 首尾插屏障（代替手写对称屏障，规则 #10） | 少写代码，**屏障数量不变** | 价值有限（只是自动化） |
| **L2 优化** | 图依赖分析 → 屏障**合并/消除/延迟** | **减少屏障转换次数** | RDG 的真正价值 |
| **L3 并行+生命周期** | 多队列、别名复用 | 性能/内存 | 已论证缓行 |

> **认知**：只做 L1 不值得（现在手写对称屏障已是"正确"的）；RDG 的增量在 L2（屏障合并/消除/延迟）。

### 7.2 L2 屏障优化手段（图依赖分析）

1. **屏障合并（batch）**：相邻多个转换合并为一次 `ResourceBarrier(N, barriers)` 调用
2. **屏障消除**：连续写同一 RT（RENDER_TARGET → RENDER_TARGET 中间态）不需要逐对转换，消除冗余边
3. **屏障延迟（deferred barrier）**：转换到最晚可用时刻，避免"转早 + 中间被意外写入"

屏障可视为 system 内/相邻 system 之间的**局部依赖边**（依赖是局部的，图是分布式的——见 7.11/7.12）。合并/消除/延迟**限定为单个 system 组内部**（如阴影链、后处理链）的局部优化，**不做全局图编译**——不设集中式 RDG，屏障仍归各 system（规则 #10）+ FrameDriver 阶段桶。

### 7.3 并行录制（当前模式已具备前提）

- **现状已是多列表模式**：每个 system 一个命令列表，`SubmitRenderCommand(phase, handle)` 收集到阶段桶，`ExecuteRenderPhase` 批量 `SubmitBatch`（FrameDriver.cpp 实证）
- **并行录制可行**：system 列表独立录制，命令顺序与录制线程无关
- **屏障与录制线程解耦**：D3D12 屏障可跨命令列表（列表 B 开头的转换作用于列表 A 结束后的状态），屏障位置编译期固定插到 system 列表首尾，system 录制时零屏障感知（UE `FRDGBuilder`/Frostbite FrameGraph 同款）
- **成本 ≈ 0**：`CommandListPool` 是 deque 常驻 + 只增不减（inUse 原子标志 + 自动 Reset），异步资源加载（MeshLoadTask/TextureLoadTask 等每任务 COPY+DIRECT 两条）已达**上百列表峰值**，池已撑大，并行录制 Acquire 命中空闲项，不再创建

### 7.4 条件入图（避免无意义屏障）

- **问题**：无反射探针的 pass 会跑但内部 return——RDG 化后若该阶段入图，空 pass 也会产生无意义屏障转换
- **解法**：**构图前实体级 cull**——阶段声明 `ShouldExecute(Registry)` 谓词，Builder 收集结果为空 → 该阶段不入图 → 不产生屏障（比 RDG 的资源级 cull 更简单）

### 7.5 关键澄清（屏障与 PSO 数量解耦）

- **PSO 切换零屏障**（`SetPipelineState` 不产生 ResourceBarrier），屏障只来自**资源读写状态转换**（RT→SRV、连续写 RT 等）
- ShadowRenderer 把 4 个 PSO 塞一个 system 反而无额外屏障（列表内顺序切换），RDG 只管 system 边界的资源转换
- 大型引擎 PSO 切换上百次/帧是常态，但屏障量 = 资源读写转换（经 L2 合并后可控），与 PSO/system 数量**解耦**

### 7.6 生命周期外置（RDG 不管生命周期）

- 渲染项来自**活实体收集**（Builder 每帧从 ECS 收集），销毁实体结构性不可能进渲染管线
- 资源生命周期归：状态机（场景切换重建）+ 事件（实体销毁）+ 引用计数（GpuResourceManager）+ 临时资源池（RenderTargetPool/DepthStencilPool）
- RDG 管生命周期反而引入风险（提前释放/延迟膨胀/与现有池冲突）——**RDG 是屏障推导器，不是资源管理器**

### 7.7 别名复用（衔接现有规划）

- `Docs/API_Constraints.md` 已规划"层 4：大堆 + PlacedResource（未来方向）"——**显存不足时的压缩策略，非默认模式**
- RDG 的生命周期扫描（scan lifetimes）未来是别名的决策输入——RDG 是别名复用的**使能者**，不冲突

### 7.8 多队列并行（不需要）

- DX12 只有三种队列（DIRECT/COMPUTE/COPY），项目已用 COPY（上传）+ DIRECT（渲染），BackgroundExecutor 的 COPY→DIRECT fence 等待已实现
- 资源上传在主线程/后台线程、命令录制快 → **开不开队列没区别**，单 DIRECT 队列顺序执行足够

### 7.9 结论

RDG 对我们 = **L2 屏障优化 + 实体级条件入图 + CPU 并行录制**（池化成本已摊平），生命周期/别名/多队列全部外置或缓行。当前 9 个阶段 L2 收益有限，但架构上把"屏障作为依赖边、编译期合并"的设计定下来，pass 增多时自然受益。**RDG 不作为独立系统实施，思想分散化定案见 7.11（编译分层、缓存属地）。**

### 7.10 进阶优化（2026-08-02 补充，可行性确认）

| 优化 | 可行性 | 符合系统？ | 说明 |
|:-----|:-------|:-----------|:-----|
| **Pass 融合** | ✅ 已实现先例 | ✅ 完全符合 | 后处理链确定性极强（SSAO→Blur→合成，顺序固定），**不需要 RDG 自动融合**——渲染器显式多 PSO 顺序执行即手动融合（`SsaoRenderer` SSAO+Blur、`ShadowRenderer` 4 PSO 已是先例）。固定阶段 + 渲染器持有 PSO 集合，融合是渲染器内部的事 |
| **编译结果缓存** | ✅ 高价值低难度 | ✅ 天然适配 | 阶段拓扑固定（9 阶段），编译一次帧帧复用，只在条件入图变化时重判（UE hybrid rebuild 同款）——白拿收益 |
| **状态机跟踪** | ⚠️ 中等复杂度 | ✅ 符合 | 逐资源跟踪当前状态、只在状态变化时插入转换，比"固定首尾插"更精细（UE `FMeshDrawCommandStateCache` 资源版）。**归属 FrameDriver 帧级，且是逐 system 的局部资源状态表**（每个 system 只跟踪自己管理的资源），**非全局资源状态表**——不构成集中式 RDG 的雏形（见 7.11/7.12） |
| **D3D12 Enhanced Barriers** | 🔮 未来 | — | Win11 + 新驱动特性（`D3D12_OPTIONS12` 检测），当前兼容面用不上，作为未来迁移选项 |
| **ExecuteIndirect / GPU-Driven** | ❌ 缓行 | — | 实体数少，CPU 绘制提交非瓶颈（NVIDIA 推荐但规模未到） |
| **子分配（size bucketing）** | ❌ 缓行 | — | 衔接 docs 层 4（显存不足才启用） |

> **结论**：三个可纳入的进阶优化中，**编译缓存**性价比最高（固定阶段拓扑让"编译一次、帧帧复用"几乎白拿）；**Pass 融合已由现有渲染器多 PSO 模式天然实现**，无需 RDG 支持；**状态机跟踪**作为屏障精细化的升级方向记录，实现时再定。

### 7.11 定案：RDG 思想分散化——编译分层、缓存属地（2026-08-02 讨论收敛）

> 关联：`SceneStateMachine.md`（场景级缓存生命周期管理者）、`OctreeCullingAndRaycaster.md`（PVS 预计算，原 cull.md 已并入）、`Reflection.md`（烘焙光照/光照探针）

**澄清"编译缓存"的完整含义**：RDG 语境下的"编译缓存"不是单一物，而是**全局范围资源图的三层编译产物**——内容与 PSO 并不完全一致：

| 层 | 编译产物 | 时间跨度 | 内容源 | 生命周期管理者 |
|:---|:---------|:---------|:-------|:---------------|
| **场景级静态** | PSO 集合、PVS 可见集、烘焙光照（cubemap/光照探针）、静态遮挡数据 | 场景生命周期（分钟~小时） | 场景 JSON + 资产（.dxmesh/.mat/.scene） | 全局状态机（replace 失效 / additive 增量 / stream 预编译） |
| **帧级动态** | 屏障图（L2 合并）、剔除结果、LOD 结果 | 帧（16ms） | 每帧实体收集 + 相机 | FrameDriver 阶段调度 + 双缓冲 |
| **进程级常驻** | 资产缓存（AssetManager）、GPU 资源池、描述符堆 | 进程生命周期 | 加载管线 | 各管理器 |

**RDG 经典意义的"图编译"只是帧级动态层里屏障那一小块**。若存在一张统一的资源图（节点 = 场景/资产/渲染阶段，边 = 依赖），RDG 只是它在"帧级屏障"一个维度上的投影——PVS 不产生 PSO，烘焙光照不产生 PSO，它们是同一张资源图的其他编译产物。

**不存在集中式 RDG 器——依赖是局部的，图就是分布式的**：

| 模块 | 它知道的依赖 | 它承担的 RDG 功能 |
|:-----|:------------|:-----------------|
| 全局状态机 | 场景 ↔ 资产集合 | 场景级缓存生命周期（换场景 = 整层失效/增量/预编译） |
| 渲染器（管理器模式） | 几何 + 材质 → PSO | PSO 集合缓存（已实证：ShadowRenderer 4 个） |
| 构建器 | 实体 + 组件 → 渲染项 | 实体级条件入图（ShouldExecute 谓词） |
| system（规则 #10） | 资源进出 → 屏障 | 对称屏障（局部图的首尾插） |
| FrameDriver | 阶段 → 命令列表 | 帧拓扑 + 多列表并行录制 |
| AssetManager | 后缀 → 加载器 | 资产缓存（注册表模式已落地） |

每个模块的依赖关系都**局部可知**：渲染器不需要知道 PVS 怎么算，PVS 不需要知道 PSO 怎么编译。强行收拢成一个 RDG 器，反而要求该器知道所有模块的依赖语义——过度设计。

**最终形态 = 资源缓存与预计算策略的抽象集合**，落成一条架构约定（与规则 #10 同级）：

> **编译分层、缓存属地**：每个模块为自己的依赖图负责；编译产物按时间跨度分层缓存——场景级静态产物（PSO 集合/PVS/烘焙光照）归状态机管理，帧级动态产物（屏障/剔除/LOD）归 FrameDriver 管理，进程级常驻缓存归各管理器管理。不设集中式 RDG。

要点：
- **状态机是最大的缓存生命周期管理者**：`replace` = 场景级缓存整体失效重建，`additive` = 增量补编译，`stream` = 预编译邻接场景——这就是"预计算"的时间窗口，无需 RDG 的 scan lifetimes
- **屏障部分保留现有对称规则（#10）**，7.10 的"状态机跟踪"作为升级选项，不改变归属
- **PVS / 烘焙光照**是场景级静态产物的一部分，与 PSO 集合平级，一起受状态机生命周期管理
- 此结论修正 7.9：**RDG 不作为独立系统实施**，其思想按上述约定分散落地

### 7.12 结构性论证：有状态 ⇒ 无 RDG（2026-08-02 补充）

**"pass 拓扑运行时动态"不构成 RDG 的理由——动态 ≠ 不可计算**。我们的 pass 是**收集来的**，其内容完全来自一条确定性状态链：

```
全局状态机（边静态定义）
  → 当前状态 = 活跃场景集合（确定）
  → 场景 JSON（声明实体/资产/环境，确定）
  → SceneConstructor → ECS Registry（实体，确定）
  → Builder 每帧收集（读 ECS 实体 + 组件，确定性映射）
  → RenderItem（几何/材质/PSO 条件，确定）
  → 渲染器从 PSO 集合中选择（确定）
```

每一环都是确定性映射：状态确定 → 场景确定 → ECS 内容确定 → 收集结果确定。"收集"是**执行手段**（每帧从 ECS 读一遍），不是**内容来源**——内容来源是场景声明，在场景加载那一刻已被状态机决定，不需要运行时现场推导依赖。

**动态性仅存于"值"层面，不碰"结构"层面**：

| 动态的是什么 | 影响 | 静态的是什么 | 影响 |
|:------------|:-----|:------------|:-----|
| 相机/光源参数（每帧变） | 只改绑定值（passConstants） | 用哪些 PSO / 哪些资源 | 场景加载时已确定 |
| 实例变换（每帧变） | 只改 instanceBuffer 内容 | 屏障拓扑 | 9 个固定阶段 |
| 实体增删（运行时） | 改实体集合；PSO 需求从新实体引用的几何/材质推导 | PSO 变体全集 | 场景资产声明已确定 |

运行时新增实体（玩家 spawn、游戏逻辑）不在场景 JSON 里，但其 PSO 需求仍从"它引用了哪些几何/材质"推导，实体创建时按需编译（构建器兜底）——不需要全局构图。

**集中 RDG 的隐含前提是"无状态"**：依赖关系每帧可能从零重建，所以必须每帧构图。但无状态的完全自由流转意味着依赖关系理论无限、预计算无从谈起、每帧从零开始——那不是"动态"，而是**设计缺陷**。**状态是预计算的必要条件**：状态空间有限可枚举，当前状态的出边确定，邻接节点的内容确定。

**结论**：不设集中 RDG 不是因为"当前规模小"，而是因为"有状态 ⇒ 资源边界可计算 ⇒ 预编译可覆盖 ⇒ 无图需求"。规模增长只会增加状态/节点数量，不改变这个结构。集中 RDG 的生存前提（结构层面运行时未知）在我们这里不成立——**RDG 亡于状态，而非亡于规模**。

### 7.13 重新评估触发信号（2026-08-02 补充）

不设集中 RDG 是结构性结论，但**若以下信号连续触发，应重新评估**（届时优先走 7.10 的升级路径，而非直接上全图 RDG）：

| 信号 | 阈值参考 | 说明 |
|:-----|:---------|:-----|
| 渲染器/PSO 数量 | > 30 个渲染器、> 60 个 PSO | 屏障组合数 ≈ O(n²)，手写开始漏 |
| 每帧屏障调用次数 | 单帧 > 100 次 ResourceBarrier | L2 合并收益开始显著 |
| 显存峰值 | 接近显存预算 80%+ | 别名复用成为刚需而非优化（衔接层 4 大堆+PlacedResource） |
| 新增 pass 的平均成本 | 加一个 pass 需要改 5+ 处文件 | 图声明比手写接线便宜了 |

**升级路径是渐进的，不是"要么无图要么全图"**：档位 1 = 状态机跟踪（逐 system 局部资源状态表）；档位 2 = 局部图（仅对某条链，如后处理链内部构图，跨组仍用阶段桶）；档位 3 = 全图 RDG（仅当全局 pass 拓扑都失控时）。档位 1/2 不改变"编译分层、缓存属地"的归属，屏障仍归 FrameDriver。

---

## 八、待定事项（实现时再细化）

- PSO 描述 JSON schema（geometryConditions + variants 结构）
- 绑定槽位强类型 enum 定义（Slot_PassConstants / Slot_InstanceBuffer / Slot_BoneBuffer…）
- 绑定列表容量（内联 8-16 槽位）与批处理比较策略
- RenderContext 接口方法集（哪些管理器透传、占位纹理类型）
- `Release(handle, 0)` fence 语义修正（并入待办 #49）

---

## 九、相关文档

- `Docs/architecture/rendering/SubMeshMaterialSlots.md` — 材质槽模式（#22/#23/#24）
- `Docs/architecture/scene/SceneStateMachine.md` — 全局状态机（场景级缓存生命周期管理者，§7.11）
- `Docs/architecture/rendering/FrameResourceManager.md` — 帧资源（PassConstants 等 CB 布局）
- `Docs/architecture/core/EngineOverview.md` — ECS-构建器-渲染器模式
- `Docs/architecture/assets/AssetLoaderImprovement.md` — 资产加载器注册表
- `Docs/todos/archived/remaining_issues.md` — 全局待办清单
