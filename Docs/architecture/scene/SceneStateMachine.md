# 场景状态机与实体生命周期（设计定案）

> 日期：2026-08-02
> 状态：📋 设计定案（方向讨论收敛，具体实现留待场景切换系统立项时细化）
> 关联：`Docs/architecture/scene/SceneFileAndLoading.md`（场景文件格式）、`RelationshipModel.md`（实体关系）、`SceneManager.md`（场景序列化器）、`AssetLoaderImprovement.md`（资产加载）、`EngineOverview.md`（ECS Registry 唯一数据源）
> 结论先行：**场景文件 = 扁平纯容器**（实体/环境/依赖，不含语义）；**类型与流转 = 独立全局状态机文件**；**输入 = 输入上下文栈**；**角色生命周期独立于场景**（持久实体层 + 场景容器分离）。

---

## 一、动机与出发点

场景系统设计围绕几个实际需求展开：

1. **角色跨场景存活**：把角色丢进场景做测试时，角色并不独属于场景——角色会进入下一个场景/下一个房间（空洞骑士式），也存在格斗游戏那种（角色选择 → 战斗 → 结算，角色数据跨场景流转）。**游戏角色的生命周期不确定**。
2. **场景可替换性**：狭义的场景（关卡）应能整体换掉，不影响 UI 与系统。
3. **场景间冗余**：场景与场景之间必然存在冗余；扁平化内容内联本身是为了加载原始资产的速度，但 AssetManager 自带缓存，引用复合资产也没关系。
4. **UI 与游戏场景必然分离**：全局菜单/背包/HUD 是系统性 overlay，不是世界内容。

---

## 二、核心结论（定案）

### 2.1 场景文件 = 扁平纯容器

场景文件是**纯资产容器**，只装：

```
场景文件（SceneDescription）
  ├─ dependencies      资产引用（✅ 已实现，AssetManager 按后缀注册表分发）
  ├─ sceneEnvironment  环境参数（✅ 已实现：ambient/skybox）
  └─ entities          实体组合（✅ 已实现）
```

**不包含**：
- ❌ 场景类型枚举（不可枚举，由游戏类型决定）
- ❌ 切换逻辑 / 流转定义（场景对上一个/下一个场景**无感知**）
- ❌ 输入规则（输入语义外置到输入上下文栈）
- ❌ 角色实体（player 不属于场景内容）

### 2.2 场景类型不可枚举

- 场景类型（menu / world / overlay / 房间 / 关卡…）**不具备完全枚举的可能性**，它基于游戏类型而定，甚至层层重叠都可行（颗粒度极高）。
- **编辑器端用 JSON 定义，引擎 CORE 具备读取能力即可，不给出硬编码定义**。
- 引擎 CORE 只读场景与状态机 JSON，不解释类型语义；类型名留给游戏层解释。
- 行为差异（是否 3D 渲染、是否收输入）用**行为标记**而非类型枚举表达（如 `renders3D` / `inputMode` 等标记）。

### 2.3 独立全局状态机文件

- 场景切换使用**独立全局状态机文件**（节点 = 场景，边 = 流转），集中定义。
- 场景文件对前后场景**无感知**——不在场景内声明自己的出边。
- 切换语义由状态机边的"加载语义"字段决定：`replace`（关卡式重建）/ `additive`（叠加）/ `stream`（流送，保留当前活跃点邻接场景以优化切换）。
- 同时兼容两种游戏类型：
  - **关卡式重建**（魂类/关卡制）：状态机边 = `replace`，整块替换
  - **空洞骑士房间式**：状态机边 = 切换但保留世界，只换房间内容

**状态机兼任场景级缓存生命周期管理者**（见 `RendererDataDriven.md` §7.11"编译分层、缓存属地"）：场景级编译产物——PSO 集合、PVS 可见集、烘焙光照（cubemap/光照探针）、静态遮挡数据——按状态机边的加载语义管理生命周期：

- `replace` = 场景级缓存**整体失效重建**（整层丢弃，加载新场景后重新编译）
- `additive` = **增量补编译**（保留已有场景产物，只补新增内容）
- `stream` = **预编译邻接场景**（流送边已知下一批场景 → 提前编译其 PSO 集合/PVS/烘焙光照，这就是"预计算"能力的时间窗口，无需集中式 RDG 的 scan lifetimes）

不设集中式 RDG 器；状态机承担 RDG 思想中时间跨度最大、价值最高的部分（场景级缓存生命周期），帧级屏障/剔除/LOD 归 FrameDriver，进程级缓存归各管理器。

### 2.4 场景切换 ≠ 场景栈

- **编辑器 Tab 不能演化为场景栈**：栈是后进先出（LIFO），而场景 Tab 是**并列无关**的快照集。
- 现在的场景只能当作一种**场景的快照**，具体是什么类型是位置性的（由游戏类型决定）。
- 切换是**状态机/有向图**：节点 = 场景，边 = 流转，可并列、可重叠、可嵌套。

### 2.5 输入上下文栈

- 场景的切换和输入上下文栈有关系——**输入上下文栈就是为了不同场景下的输入正确而设计的**。
- 层层叠加是**渲染上的叠加**（若精确到"不更新"会显得管理复杂）。
- 输入语义：栈顶优先消费输入，未消费的向下传递 → 合成（参考 UE InputMode：GameOnly / UIOnly / GameAndUI）。

### 2.6 双形态：编辑器引用式 / Game 扁平式

| 形态 | 用途 | 说明 |
|:-----|:-----|:-----|
| **引用式**（编辑器端） | 编辑/保存 | 场景引用复合资产（`.prefab`/`.character`），靠 AssetManager 缓存复用原子资产 |
| **扁平式**（Game 端） | 运行时加载 | 内容内联展开，为解析速度；**save 直接用扁平版**；引用展开后续再考虑 |

- 最终在**原子资产**层复用（AssetManager 缓存），不必担心场景间冗余。

---

## 三、大型引擎参考

| 引擎 | 场景文件本身 | "场景是什么"由谁决定 | 输入/UI 处理 | 邻接场景 |
|:-----|:-----------|:-------------------|:------------|:---------|
| **UE** | Level = 纯容器（Actor 集合），无类型枚举 | **GameMode/GameState**（独立于 Level 的规则对象） | **InputMode**（`GameOnly`/`UIOnly`/`GameAndUI`）；UMG Widget 独立于 Level 的 Viewport 层 | **Level Streaming / World Partition**：区块邻接关系在外部定义，不进关卡文件 |
| **Unity** | Scene = 纯容器，无类型枚举 | 加载模式决定语义：`LoadSceneMode.Single`（替换）/ `Additive`（叠加） | Canvas **Overlay 模式**独立于场景；社区标准做法是 Base Scene + Additive UI/Player 场景 | 邻接预加载靠 Addressables/场景脚本，不在场景内声明 |
| **Godot** | 场景 = Node 树容器 | 行为靠根节点脚本；`change_scene_to_file` 直接替换 | UI 是 Control 节点（与 3D 同树但独立层） | `ResourceLoader.load` 缓存预加载 |

**共性规律**：
1. 资产层：资源全局唯一，场景只"引用"不"复制"
2. 场景层：狭义场景 = 可整体替换的游戏世界（实体组合）
3. UI 层：与 3D 世界分离的 overlay
4. 切换 = 状态机（替换 / 叠加 / 流送），由加载动作决定，不由场景内容声明

---

## 四、实体生命周期（角色跨场景）

### 4.1 核心原则：角色不属于任何场景

大型引擎的答案（Unity `DontDestroyOnLoad`/独立 Player 场景、UE `PlayerController`/`PlayerState` 跨关卡存活 + Seamless Travel、Godot Autoload 单例）——**角色是跨场景持久实体**。

```
持久实体层（跨场景存活）：player / 玩家数据 / 全局管理器
        ↑ 场景切换时保留
场景层（可替换的世界内容）：关卡 / 房间 / 菜单
        ↑ 进入时注入 player，退出时回写状态后销毁
```

### 4.2 落地映射（ECS Registry）

本项目已有 `ECS Registry 是唯一数据源` + `SceneTagComponent`（编辑器多 Tab 按 sceneId 过滤）——机制直接延伸：

- **player 实体不带 sceneId**（或标记为持久）
- 场景切换 = 移除该场景的实体，player 实体留在 Registry
- 编辑器多 Tab 已是"多个场景共存于一个 Registry"的验证，player 跨场景 = "一个实体不属于任何场景分区"

### 4.3 两种游戏类型覆盖

| 类型 | 生命周期 | 处理 |
|:-----|:---------|:-----|
| **房间式**（空洞骑士） | 角色跨房间持续存在，房间切换保留世界 | player 在持久层；房间场景销毁/重建，player 通过出生点锚定位置 |
| **格斗式**（角色选择→战斗→结算） | 角色在"对局"流程中跨越多个场景 | player 数据（所选角色/血量）在持久层；每个场景是流程一站，加载语义由状态机边决定（`replace`） |

差异只在状态机的边（`replace`/`additive`/保留世界），player 生命周期始终在持久层，**不需要改场景文件格式**。

---

## 五、与现有架构的映射

| 维度 | 现状 | 差距 |
|:-----|:-----|:-----|
| 资产引用 | ✅ AssetManager 缓存 + 复合资产（.character） | 无 |
| 场景内实体引用 | ✅ RelationshipModel `persistentId`（fnv1a hash） | 无 |
| 场景容器三段式 | ✅ dependencies / entities / sceneEnvironment | 无 |
| 场景类型 | ❌ 无（不可枚举，游戏类型决定） | 编辑器端 JSON 定义，引擎 CORE 只读 |
| 场景间引用 | ❌ 每个 .scene 实体完整展开 | 编辑器端引用式 / Game 端扁平式（save 用扁平版） |
| 场景切换 | ⚠️ 编辑器多 Tab（SceneTagComponent 按 sceneId 过滤）；Game 端单场景 | 抽象为**独立全局状态机文件** |
| 输入合成 | ❌ Game 端相机/拾取直接处理 | 输入上下文栈（渲染叠加，非栈式场景） |
| 角色生命周期 | ❌ 未处理 | 持久实体层（无 sceneId） |

---

## 六、落地路径（讨论用，未排期）

1. **近期（不改架构）**：确认 `.scene` JSON 容器边界（不塞类型/切换/输入/角色）；Game 端加载用扁平版
2. **中期**：独立全局状态机文件（节点=场景，边=加载语义 replace/additive/stream）+ 输入上下文栈（UI overlay 场景接入）
3. **远期**：持久实体层（player 无 sceneId）+ 出生点协议（场景进入时把 player 挂进世界）

---

## 七、待定事项（实现时再细化）

- 状态机 JSON 的 schema（节点字段：场景路径 / 加载语义 / 输入上下文 / 邻接列表）
- 引擎 CORE 暴露的 API（`PushScene`/`PopScene`/`Transition(edge)`？还是只读状态机文件 + 运行时查询）
- 出生点/锚点协议（位置数据在场景里，player 实体在持久层）
- 引擎 CORE 感知的行为标记粒度（`renders3D`/`inputMode` 等）

---

## 八、相关文档

- `Docs/architecture/scene/SceneFileAndLoading.md` — 场景文件格式 + 加载管线
- `Docs/architecture/scene/RelationshipModel.md` — 实体关系模型（ID 引用）
- `Docs/architecture/scene/SceneManager.md` — 场景管理器架构（场景序列化器定位）
- `Docs/architecture/assets/AssetLoaderImprovement.md` — 资产加载器注册表（后缀分发 + 缓存）
- `Docs/architecture/rendering/RendererDataDriven.md` — 渲染器数据驱动（§7.11 状态机兼任场景级缓存生命周期管理者）
- `Docs/architecture/core/EngineOverview.md` — ECS Registry 唯一数据源
- `Docs/todos/archived/remaining_issues.md` — 全局待办清单（本方向登记见"新设计方向"）
