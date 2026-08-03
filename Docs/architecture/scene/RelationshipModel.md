# 实体关系模型

> 日期：2026-07-27
> 状态：设计草案

---

## 一、动机

场景中的实体之间存在各种关系——相机跟随角色、武器挂到角色手上、敌人属于同一波刷怪点。需要一种统一的方式表达这些关系，同时保持场景 JSON 扁平化和引擎 CORE 的零处理原则。

### 关键设计原则

- **场景 JSON 是扁平的**，不嵌套、不递归
- **关系是数据，不是树**——引擎 CORE 只存储关系，不解释、不维护
- **关系可以被破坏**——切断关系后实体自然独立，不需要级联处理
- **骨架层级不是场景关系**——基于骨骼的角色是单一实体，骨骼树属于动画系统，不在此模型范围内

---

## 二、关系模型

### 2.1 场景 JSON 格式

```json
{
  "entities": [
    {
      "name": "character",
      "persistentId": "a1b2c3d4e5f67890",
      "components": {
        "transform": { "position": [0, 0, 0] },
        "mesh": { "geometry": "player.dxmesh", "material": "player_mat" }
      }
    },
    {
      "name": "main_camera",
      "persistentId": "b2c3d4e5f67890a1",
      "components": {
        "transform": { "position": [0, 2, -5] },
        "camera": { "fov": 60, "near": 0.1, "far": 1000, "projection": "perspective", "isMain": true }
      },
      "relationships": [
        { "kind": "parent", "targetId": "a1b2c3d4e5f67890" }
      ]
    },
    {
      "name": "weapon_sword",
      "persistentId": "c3d4e5f67890a1b2",
      "components": {
        "transform": { "position": [0.5, 0, 0] },
        "mesh": { "geometry": "sword.dxmesh", "material": "weapon_mat" }
      },
      "relationships": [
        { "kind": "socket", "targetId": "a1b2c3d4e5f67890", "socketName": "hand_r" }
      ]
    }
  ]
}
```

### 2.2 JSON 字段

| 字段 | 类型 | 必需 | 说明 |
|:-----|:------|:------|:------|
| `kind` | string | ✅ | 关系类型（见下文） |
| `targetId` | string | ✅ | 目标实体的 `persistentId`（fnv1a 64-bit 十六进制 hash 字符串） |
| `socketName` | string | ❌ | 挂点名称，仅 `socket` 类型使用 |

### 2.3 关系类型

| kind | 语义 | 典型用途 | 消费者 |
|:-----|:------|:---------|:-------|
| `parent` | 变换父级 | 相机跟随角色、子物体绑父物体 | Editor 多选联动、Gameplay 脚本 |
| `socket` | 骨骼挂点绑定 | 武器挂到角色 "hand_r"、护甲挂到 "spine" | Gameplay 武器系统 |
| `group` | 逻辑分组 | 属于同一波敌人、属于同一房间 | Gameplay 空间管理 |
| `follow` | 跟随目标 | Camera 的显式跟随声明 | CameraSystem |

**`parent` vs `socket` 的区别**：
- `parent`：子实体的位置是父实体的本地坐标，无骨骼也不依赖骨骼
- `socket`：子实体挂在父实体某块骨骼的世界坐标上，需要动画系统提供骨骼世界矩阵

---

## 三、ECS 组件

### 3.1 运行时关系组件

引擎 CORE 提供一个极简组件，**运行时直接存储 `entt::entity` handle**，不经过 hash 查找，O(1) 访问：

```cpp
// Engine/ECS/Core/Components/Relationship.h

enum class RelationshipKind : uint8_t {
    Parent,
    Socket,
    Group,
    Follow
};

// 运行时关系组件——targetEntity 是 ECS 实体 handle
// 由 SceneConstructor 在加载时从 JSON 的 targetId(hash) 解析得到
struct RelationshipComponent {
    entt::entity targetEntity = entt::null;  ///< 目标实体的 ECS handle（O(1) 访问）
    RelationshipKind kind;                    ///< 关系类型
};

// socket 名称单独拆分为可选组件，避免变长 string 浪费内存
struct SocketAttachmentComponent {
    std::string socketName;                  ///< 骨骼挂点名称（仅 RelationshipKind::Socket 时使用）
};
```

### 3.2 加载时解析流程

```
场景 JSON:
  entities[0]: persistentId="a1b2c3..."
  entities[1]: persistentId="b2c3d4...", relationships=[{kind:"parent", targetId:"a1b2c3..."}]

SceneConstructor::ConstructEntities:
  第 1 遍：遍历所有实体 → registry->create() → 建 hash→entt::entity 映射表
    map["a1b2c3..."] = entity_0
    map["b2c3d4..."] = entity_1

  第 2 遍：遍历所有实体的 relationships → 用映射表解析 targetId
    entity_1.relationships[0].targetEntity = map["a1b2c3..."]  // = entity_0

运行时:
  registry->valid(rel.targetEntity)           // O(1) 检查目标是否存活
  registry->get<T>(rel.targetEntity)           // O(1) 直接访问
  // entt::entity 自带 generation，目标销毁后自动变为 invalid
```

### 3.3 导出流程（Editor 端）

导出不依赖 ECS——关系数据来自 `SceneSnapshot::entityDescs` 中的扁平结构：

```
ExportToDescription:
  1. 遍历 SceneSnapshot::entityDescs（内存中的 JSON 原始数据）
  2. 每个 EntityDesc 中已包含 persistentId（hash 字符串）
  3. RelationshipDesc.targetId 直接使用该 hash（不需要从 ECS 反查）
  4. 写入 JSON 时直接序列化 hash 字符串

场景文件 JSON:
  "relationships": [
    { "kind": "parent", "targetId": "a1b2c3d4e5f67890" }
  ]
```

> **关键**：hash（persistentId）是在场景内容中的静态标识，不依赖于运行时 ECS。导出时从 SceneSnapshot 的 entityDescs 缓存中直接读取，不需要从 entt::entity 反查。

### 3.4 引擎 CORE 的职责边界

| 操作 | 引擎 CORE |
|:-----|:----------|
| 加载场景时创建 `RelationshipComponent` | ✅ 仅创建，不验证 targetId 是否存在 |
| 保存场景时序列化 | ✅ 导出 `relationships` 数组 |
| 关系破坏/修改 | ❌ 不处理——删除组件或修改 targetId 是脚本的职责 |
| 级联删除 | ❌ 不处理——父实体销毁不影响子实体 |
| 树缓存/索引 | ❌ 不维护 |
| Transform 传播 | ❌ 不自动同步——Gameplay 脚本决定 |
| Editor 多选联动 | ❌ Editor 端自行实现，不在 CORE 中 |

---

## 四、与骨骼系统的边界

**基于骨骼的角色是单一的 ECS 实体**，不通过 `RelationshipComponent` 表达：

```
实体会话:
  player_entity
    ├─ TransformComponent（根变换）
    ├─ MeshComponent（全身模型，骨骼蒙皮）
    ├─ SkinnedComponent（骨骼数据、当前动画）
    └─ RelationshipComponent（可选，指向场景中的其他实体）
```

- 角色的骨骼层级（head/spine/hand_r/...）在蒙皮网格的 skinned 数据中，**不在 ECS 组件中**
- 动画系统内部处理骨骼世界矩阵的计算
- 外部实体通过 `RelationshipComponent + socketName` 引用角色的某块骨骼，但这些实体**不是角色的子实体**

```
场景 JSON:
  entities:
    - name: "player"              ← 单一实体
      components:
        mesh: { geometry: "player.dxmesh" }
        skinned: { ... }
    - name: "weapon"              ← 独立实体，挂在骨骼上
      relationships:
        - { kind: "socket", targetId: 1001, socketName: "hand_r" }
```

---

## 四、挂载实体的动画模型（两层模型）

> 2026-08-01 补充。解决"挂载实体（武器/盾）自己是否也有动画，如何与父骨骼叠加"的问题。参考现代引擎（Unreal/Unity/Godot）通行做法。

### 4.1 核心认知：挂载实体有两个动画通道

```
角色骨骼（动画驱动）
  └─ Socket 挂点（骨骼上的命名锚点）
       └─ 挂载实体（武器/盾：自己的 Mesh + 可选自己的动画）
```

| 通道 | 含义 | 例子 |
|:-----|:-----|:-----|
| **父驱动**（跟随层） | 挂载实体继承挂点骨骼的世界矩阵，位置随角色动画走 | 手挥动时剑跟着手 |
| **自身层**（自驱动） | 挂载实体在父骨骼变换之上叠加**自己的局部动画** | 后坐力、换弹、盾防御姿态 |

**最终变换 = 父骨骼世界矩阵 × 挂载实体自身局部动画 × socketOffset(state)**

### 4.2 "盾防御时对着正面"——三个解法（现代引擎都在用）

| 解法 | 做法 | 典型实现 | 适用 |
|:-----|:-----|:---------|:-----|
| **A. 状态驱动挂点偏移**（socket offset，最常用） | 不同动作状态给挂点不同的局部旋转偏移；防御状态下覆写为"对准正面"，其他动作恢复默认 | Unreal `AnimNotify` + 状态机切换设置 socketOffset | 盾是纯静态 mesh，零自驱动 |
| **B. 挂载实体自带局部动画**（self-anim） | 盾自己有一个"防御姿态"局部动画剪辑，由动画系统在挂载实体上播放 | Unreal 武器自己的 `AnimInstance`（weapon AnimBP）、Unity 武器独立 `Animator` | 盾是独立可复用动画实体，换角色不重做；武器换弹/后坐力同一机制 |
| **C. 附着规则**（attachment rule，Unreal 特有） | `AttachToComponent` 选 `KeepWorldTransform`——子物体**只跟位置不跟旋转**，永远保持世界朝向 | `FAttachmentTransformRules::KeepWorldTransform` | "防御时盾永远对着正面"的最简实现；但全局行为，按状态切换需配合 A |

**实践中 A+B 组合最常见**：静态挂载（武器）用 A，需要自带动画的挂载（盾的姿态、武器后坐力）用 B。

### 4.3 叠加 vs 覆盖

| 模式 | 语义 | 适用 |
|:-----|:-----|:-----|
| **满覆盖** | 挂载实体动画**完全取代**父骨骼变换 | FPS 第一人称武器（相机相对武器，不随骨骼） |
| **叠加** | 在父骨骼变换**之上**叠加局部偏移 | TPS 武器后坐力、盾防御姿态、换弹 |

盾防御 = 叠加模式（父骨骼还在动，盾在父基础上再转一个角度）。

### 4.4 本项目实现模型

```
挂载实体 = 独立 ECS 实体
  ├─ TransformComponent          ← 局部变换（挂点本地坐标）
  ├─ MeshComponent               ← 武器/盾网格
  ├─ RelationshipComponent(socket, socketName)  ← 挂在角色哪块骨骼
  └─ SocketAnimComponent(可选)   ← 自身局部动画（新增）
        finalWorld = parentBoneWorld × localAnim × socketOffset(state)
```

- **静态挂载**（枪、剑）：无 SocketAnimComponent，纯父驱动 + 状态偏移（解法 A）
- **姿态挂载**（盾）：SocketAnimComponent 播"防御姿态"局部剪辑（解法 B），或按状态覆写 socketOffset
- **与蒙皮链路的关系**：骨骼世界矩阵来自蒙皮链路（GPU StructuredBuffer 骨骼缓冲），socket 解算在动画/蒙皮 System 之后跑（见待办 U4/U7）

### 4.5 回看 UKW 原始数据

原始 HOD/ANI 里武器和盾是**带矩阵的部件**（母版骨架里就有 gun/sword/Shield 的骨骼和动画通道）——武器本身有动画。现代化映射：

**骨骼 → socket 挂载实体**，原来的"武器骨骼动画" → **挂载实体自身的局部动画或状态偏移**。

这样武器保持可替换、可掉落、可换装的模块化（现代引擎不把武器放进角色骨架——放进骨架就焊死了，无法换装/掉落）。

#### 4.5.1 ANI 帧 = 局部矩阵，武器挂载动画数据可直接复用（2026-08-01 确认）

文档核实（`01_AssetFormatOverview.md` §二、"动画"行；`02_RobotAndAnimation.md` §2.1 HOD 块内部结构）：

- **ANI 文件 = 文件名头 + 连续 HOD 块序列**（1.008 原版；PUK 2.008 为 HD2 块）
- **每个 HOD 块 = 一帧的部件局部矩阵快照**（固定父子结构，无传统骨骼概念）：部件名区 + A/B 层级 + **每部件局部 4×4 矩阵**（相对父部件）
- 帧矩阵语义为**局部矩阵**，不是世界矩阵（`RobotMerger.cpp` 的"层级累乘 local × parent"即基于此）

**推论——两层模型的自身层数据源现成**：

```
最终武器世界矩阵 = 机体父骨骼世界矩阵（层级累乘，机体动画）× 武器局部矩阵（ANI 帧直提）

武器局部矩阵 = ANI 帧里 gun/sword/Shield 部件的局部 4×4 矩阵
             → 直接作为挂载实体自身层动画（SocketAnimComponent）的 .anim 剪辑数据
             → 零转换、零丢失，"旧时代一起摆动作"的表现完整迁移
```

即：盾防御姿态、武器后坐力/挥舞等自身层动画，**数据从 ANI 帧里就有**（武器部件每帧局部矩阵在动），现代化只把数据从"骨架节点"改挂到"挂载实体"，提取路径是直的。

---

## 五、单一实体 submesh 与多实体关系的边界

关系模型不适用于"同一个实体内部的部件拆分"。需要明确区分两种场景：

### 5.1 单一实体 + Submesh（共享骨骼）

适用场景：**吉翁号爆头、角色装备换装、部件级显隐控制**

```
吉翁号 = 1 个 ECS 实体
  ├─ SkinnedComponent（骨骼 + 动画状态，唯一）
  ├─ MeshComponent
  │   ├─ submesh[0]: body（躯干 + 手臂）
  │   ├─ submesh[1]: head（头部）
  │   └─ submesh[2]: backpack（背包）
  └─ PartsVisibility { bool visible[3] }  ← 控制各 submesh 显隐
```

- **动画**：SkinnedComponent 每帧计算一次 bone transforms，所有 submesh 共享
- **部件显隐**：PartsVisibility 控制渲染器跳过某些 submesh 的 DrawCall
- **不需要关系模型**——头不是独立实体，是同一个实体的部件

```cpp
// 吉翁号爆头——显隐控制，不需要关系断裂
void ZeongOnDamage(Entity entity, float hp) {
    auto* vis = registry->TryGetComponent<PartsVisibility>(entity);
    if (!vis) return;
    if (hp <= 0) {
        vis->visible[0] = false;  // body 隐藏
        vis->visible[1] = true;   // head 保留
        // 实体仍然是同一个，骨骼动画继续播放
    }
}
```

### 5.2 多实体 + RelationshipComponent（独立实体绑定）

适用场景：**武器挂到手上、相机跟随角色、史莱姆分裂**

```
武器 = 独立 ECS 实体（有自己的 TransformComponent、MeshComponent）
  └─ RelationshipComponent: kind=socket, targetId=角色, socketName="hand_r"

角色 = 独立 ECS 实体（有自己的 SkinnedComponent、TransformComponent）
  └─ 没有武器相关的组件——武器是另一个实体
```

- **动画**：武器自己不需要骨骼（骨骼在角色身上）
- **位置同步**：Gameplay WeaponAttachmentSystem 读取角色的"hand_r"骨骼矩阵，设置武器的 Transform
- **关系切断**：删除 RelationshipComponent 后武器自然独立（掉落地上）

### 5.3 判断准则

| 条件 | 用 Submesh（单一实体） | 用 Relationship（多实体） |
|:-----|:----------------------|:--------------------------|
| 共享同一套骨骼？ | ✅ 是 | ❌ 各自独立骨骼/无骨骼 |
| 部件之间是否有独立的 Transform？ | ❌ 否，相对骨骼偏移 | ✅ 是，每实体有自己的 Transform |
| 显隐是否独立？ | ✅ 是 | ✅ 是 |
| 是否可以独立销毁？ | ❌ 否，销毁整个实体 | ✅ 是，销毁其中任意一个 |
| 动画是否联动？ | ✅ 一次计算，全部更新 | ❌ 需手动同步骨骼矩阵 |

**示例映射**：

| 游戏案例 | 采用方案 | 原因 |
|:---------|:---------|:------|
| 吉翁号爆头（身体消失，头保留） | Submesh + PartsVisibility | 头/身体共享同一套骨骼动画 |
| 角色换装（头盔/护甲切换） | Submesh + PartsVisibility | 装备是同一骨骼上的部件 |
| 武器挂到手上 | RelationshipComponent (socket) | 武器独立于角色，可掉落 |
| 相机跟随 | RelationshipComponent (parent/follow) | 相机是独立实体，有自己的逻辑 |
| 史莱姆分裂 | RelationshipComponent + Remove | 子实体从父断裂后独立运行 |

---

## 六、使用模式

### 6.1 相机跟随（第三人称）

```cpp
// GameCameraSystem.cpp — Gameplay 层，非引擎 CORE

void FollowCameraSystem::Update(float dt) {
    auto view = m_registry->View<RelationshipComponent, CameraComponent, TransformComponent>();
    for (auto [entity, rel, cam, tf] : view.each()) {
        if (rel.kind != RelationshipKind::Follow) continue;

        // 通过 targetEntity 检查目标是否存活
        if (!registry->valid(rel.targetEntity)) continue;  // 目标已销毁，关系自然断裂

        auto* targetTf = m_registry->TryGetComponent<TransformComponent>(rel.targetEntity);
        if (!targetTf) continue;

        // 自定义跟随逻辑——完全由脚本决定
        XMVECTOR targetPos = XMLoadFloat3(&targetTf->position);
        XMVECTOR desiredPos = targetPos + XMVECTOR{0, 2, -5, 0};
        XMVECTOR smoothPos = XMVectorLerp(XMLoadFloat3(&tf->position), desiredPos, dt * 5.0f);
        XMStoreFloat3(&tf->position, smoothPos);
    }
}
```

### 5.2 关系断裂（史莱姆分裂）

```cpp
// 史莱姆死亡逻辑
void SlimeOnDeath(Entity entity) {
    auto* rel = m_registry->TryGetComponent<RelationshipComponent>(entity);
    if (!rel) return;

    // 切断父子关系——子实体（小史莱姆）成为独立实体
    m_registry->RemoveComponent<RelationshipComponent>(entity);
    // 子实体此时完全独立，所有其他组件保留
    // 后续行为由该实体的 AI System 接管

    // 父实体（大史莱姆）正常销毁
    DestroyEntity(parentEntity);
}
```

### 5.3 骨骼挂点武器同步

```cpp
// WeaponAttachmentSystem.cpp

void WeaponAttachmentSystem::Update(float dt) {
    AnimationManager& anim = AnimationManager::GetInstance();

    auto view = m_registry->View<RelationshipComponent, SocketAttachmentComponent, TransformComponent>();
    for (auto [entity, rel, socket, tf] : view.each()) {
        if (rel.kind != RelationshipKind::Socket) continue;

        // 获取目标实体的骨骼世界矩阵
        XMMATRIX socketWorld = anim.GetBoneWorldMatrix(
            static_cast<EntityHandle>(rel.targetId), socket.socketName);

        // 武器位置 = 骨骼世界矩阵 * 武器本身偏移
        XMMATRIX weaponMatrix = socketWorld * XMLoadFloat3(&tf->position);
        XMStoreFloat3(&tf->position, weaponMatrix.r[3]);
    }
}
```

---

## 六、Editor 端处理

Editor 需要"拖子物体时父物体一起动"，但这是 Editor 端的实现，不在引擎 CORE 中：

```cpp
// EditorTransformSystem.cpp — Editor 专用

void EditorTransformSystem::OnEntityMoved(Entity entity) {
    uint64_t entityId = GetPersistentId(entity);

    // 临时建反向索引——用完即弃
    auto childMap = BuildTemporaryChildIndex(m_registry);
    // childMap: targetId → vector<Entity>

    // 找到所有指向该实体的 parent 关系实体
    auto it = childMap.find(entityId);
    if (it != childMap.end()) {
        for (Entity child : it->second) {
            SyncTransform(child, entity);  // 按相对偏移同步
        }
    }
    // childMap 在此函数结束时销毁
}
```

---

## 七、相关问答

### Q: Game 端需要向下查找吗？

Game 端的使用模式是**从自身出发，向上读目标**，不需要反向索引：

```
camera 知道 targetId == 1001 → 直接读 character 的 TransformComponent → O(1)
weapon 知道 targetId == 1001 + socketName == "hand_r" → 读骨骼矩阵 → O(1)
```

如果 Game 端偶尔需要向下查找（如"谁指向我"），可以用线性扫描——几百个实体的 Registry 遍历一次是微秒级的。

### Q: 关系会被连带销毁吗？

不会。销毁实体时，其他实体中指向它的 `RelationshipComponent.targetId` 变为 dangling 引用。脚本自己负责：
- 访问前检查目标是否仍然存在（`FindEntityByPersistentId` 返回 nullptr）
- 或者在关系断裂后执行特定逻辑（如史莱姆分裂）

这正是"关系可破坏"的设计目标。

### Q: 为什么不开双向链表/树缓存？

运行时父子关系几乎不变（一次绑定，从不变动或极低频变动），维护双向链表的代价在运行时几乎永远用不到。Editor 端需要时临时建索引，用完即弃。贯彻 **Game 端不为 Editor 端买单** 的原则。

---

## 八、相关文档

- `Docs/architecture/scene/SceneFileAndLoading.md` — 场景文件格式（含关系序列化）
- `Docs/architecture/editor/ComponentEditorSystem.md` — 属性卡系统（含 CameraComponent 编辑）
- `Docs/todos/remaining_issues.md` — 待办清单
