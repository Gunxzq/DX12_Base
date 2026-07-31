# 角色复合资产（Character Asset）

> 日期：2026-07-31
> 状态：方向文档（设计定案）
> 关联：`AssetSpecification.md`、`AssetTypeDefinition.md`、`SkeletalMeshAssetPipeline.md`、`SkinnedAnimation.md`、`AssetTool_ExportPipeline_Snapshot_20260731.md`

---

## 一、动机

场景 JSON 中内嵌角色定义是不合理的：

- 同一角色在场景中摆放 N 处，就需要复制 N 份定义；
- 动画剪辑（idle/run/jump/attack）无处安放——它们既不属于网格也不属于材质；
- AssetTool 导出的 `KD-03.scene.json` 是单角色的临时拼装物，不是通用格式；
- 网格、骨架、动画剪辑、材质是可复用的独立数据，不应绑定在某个场景里。

为此引入**角色复合资产**（`.character`），把骨架、网格、材质槽、动画剪辑打包为一个可复用单元，场景只放"实例引用"。

---

## 二、核心认知：数据资产层与行为实体层分离

参考大型引擎（Unreal / Unity / Godot / Cocos）的共同做法，**"角色"被拆成两层**：

| 引擎 | 数据资产层（可复用、无行为） | 行为实体层（输入/物理/控制） | 动画状态机（独立层） |
|:-----|:-----|:-----|:-----|
| Unreal | SkeletalMesh + Skeleton + AnimSequence | Character/Pawn + CharacterMovementComponent | AnimInstance / AnimBP |
| Unity | Mesh + AnimationClip | GameObject + CharacterController | AnimatorController |
| Godot | MeshInstance3D + Skeleton3D + AnimationLibrary | **CharacterBody3D**（`move_and_slide`） | AnimationPlayer + AnimationTree |
| Cocos Creator | Mesh + AnimationClip | Node + ModelComponent | AnimationComponent |

共同规律：

1. 网格、骨架、动画剪辑是**独立数据资产**，一个骨架可配多个角色，一个剪辑可复用到多个角色；
2. 角色实体只**引用**这些资产，自己持有运行时状态（位置、当前动画、控制输入）；
3. 动画状态机单独成层，切换逻辑与剪辑数据分离；
4. **"可输入控制"是行为层职责**（CharacterBody3D 本质 = KinematicBody + 输入驱动），不属于资产格式。

**本项目定案**：
- 本次设计只到**数据资产层 + 渲染/动画 System 层**；
- 输入控制（CharacterBody3D 式移动）属于 Gameplay 层，**不进引擎 CORE**，留待后续；
- 动画状态机树（3D 场景 - 2D UI 编排）是**后续方向**：Scene 将来是"可切换的内容单元"，角色资产跨场景复用（资产级缓存，切换的是场景实例，角色 Handle 不动）。

---

## 三、资产体系变更总览

原子资产从三元组扩展为**五元组**，新增复合资产 `Character`：

| 资产 | 类型 | 扩展名 | 运行时系统 | 说明 |
|:-----|:-----|:------|:----------|:-----|
| Mesh | 原子 | `.dxmesh` / `.obj` | GeometryResourceManager | 顶点/索引/骨骼影响（boneIndices） |
| Material | 原子 | `.material` | MaterialManager | 着色参数、纹理引用、PSO 状态 |
| Texture | 原子 | `.dds` / `.png` / `.jpg` | TextureManager + GpuResourceManager | 像素数据 |
| **Skeleton** | 原子 | **`.bone`** | SkeletonManager | 骨骼树 + rest pose（HOD 解析导出） |
| **Animation** | 原子 | **`.anim`** | AnimationManager | 骨骼动画剪辑（通道名 = 命名约定） |
| **Character** | 复合 | **`.character`** | SceneConstructor + 动画 System | 骨架 + 网格 + 材质槽 + 动画剪辑 打包 |
| Scene | 复合 | `.scene` | SceneConstructor + ECS | 实体层级、组件、资产引用 |
| Terrain | 复合 | `.terrain` | TerrainManager | 高度图 → 程序化网格 + 材质层 |
| ParticleSystem | 复合 | `.particle` | ParticleManager | 粒子发射器配置 |
| Prefab | 复合 | `.prefab` | SceneConstructor | 实体模板（预留，与 Character 语义不同） |

> **命名决策**：不使用 `Prefab`（预制体是实例模板，语义不同）；不使用 `Model`（静态网格含义，不符合携带动画的角色语义）。角色资产定名 **Character / `.character`**。

---

## 四、Skeleton 原子资产（`.bone`）

### 4.1 HOD 的定位

`hod.json` 只是 AssetTool 为了**预览 HOD 解析结果**的中间格式，**不进入资产体系**（就像 `.x` 源文件导出为 `.dxmesh`）。HOD 解析后的骨骼数据以正式骨架资产 `.bone` 导出。

### 4.2 内容

```json
{
    "version": 1,
    "bones": [
        {
            "name": "Body_d",
            "parentIndex": -1,
            "position": [0, 1.362, 0],
            "rotation": [0, 0, 0, 1],
            "scale": [1, 1, 1]
        },
        {
            "name": "Head",
            "parentIndex": 0,
            "position": [0, 2.5, 0],
            "rotation": [0, 0, 0, 1],
            "scale": [1, 1, 1]
        }
    ]
}
```

- 仅含骨骼名、父子层级、rest pose 矩阵（TRS）；
- **不引用任何资产**，符合原子资产判据；
- 运行时由 `SkeletonManager::LoadFromJSON()` 加载（当前缺该接口，见 `SkeletalMeshAssetPipeline.md` 实施路线 E）。

### 4.3 复用价值（骨架资产化的意义）

| 复用场景 | 含义 | 例 |
|:-----|:-----|:---|
| 一骨架 → 多角色 | 换皮/变体：同骨骼不同 mesh + 材质 | UKW 机体配色变体、机甲换装 |
| 一骨架 → 多动画 | idle/run/attack 全部基于同一套骨骼命名 | 任何角色 |
| 一动画 → 多角色 | 骨骼命名一致时共享剪辑 | 所有"人形"单位共用 walk |

> 跨角色共享动画的前提 = **骨架资产 + 骨骼命名规范**。骨架资产是动画的锚点（类似 Unreal Skeleton），命名契约由骨架定义。

### 4.4 与 mesh / anim 的边界

```
.dxmesh  → "顶点受哪些骨骼影响"（boneIndices 序号约定）
.bone    → "骨骼树长什么样"（命名 + 层级 + rest pose）
.anim    → "骨骼怎么动"（通道名命名约定）
三者零引用，只共享命名/序号约定。
```

### 4.5 骨骼 = 可见部件（UKW 上下文，2026-08-01 确认）

UKW 极老，**无独立骨架资产**：HOD 拼接部件（实体 `.x` 模型）本身就是骨骼。排除武装（gun/sword/Shield/Weapon_point 等）后，剩余部件即全部可见骨骼。

- 含义 1：**IK 修改骨骼矩阵 = 直接驱动可见部件**，无中间蒙皮层级，天然直观
- 含义 2：骨架为**链式层级 + 局部矩阵**（`arm1→arm2→Hand`、`leg1→leg2→leg3`），非锚点扁平结构；真实骨架树见 `02_RobotAndAnimation.md` §8.1
- 含义 3：机体间骨骼命名/结构不统一（KD-06 大写 Arm1/Arm2/Arm3 + Wing/Tail），**链定义必须母版驱动**，不能硬编码骨骼名（详见 `02_RobotAndAnimation.md` §8.3）

---

## 五、Animation 原子资产（`.anim`）

### 5.1 为什么可以是原子资产

原子资产判据是"**不持有其他资产的引用**"。`.anim` 满足：

```json
{
    "version": 1,
    "duration": 1.5,
    "fps": 30,
    "loop": true,
    "channels": [
        { "bone": "arm_L", "position": [...], "rotation": [...], "scale": [...] },
        { "bone": "arm_R", "position": [...], "rotation": [...], "scale": [...] }
    ]
}
```

- 不含 skeleton 路径、不持有 SkeletonHandle、不包含网格引用；
- 骨骼名（`bone` 字段）只是**命名约定字符串**，不是引用——运行时由动画 System 把名字 hash 匹配到骨架的 `BoneNames[]`（一次性建表，顺序无关，比 dxmesh 的 boneIndex 序号对齐更鲁棒）。

该模式在现有体系中已有先例：

| 原子资产 | 隐含依赖 | 性质 |
|:-----|:-----|:-----|
| Material | 内嵌 Texture 路径（运行时 Handle 解耦） | 文档已明确允许 |
| .dxmesh | 顶点 `boneIndices` 隐含依赖骨骼序号 | 已经是原子资产 |
| **.anim** | 通道 `bone` 名隐含依赖骨骼命名 | 同类，✅ 原子 |

### 5.2 与 SkinnedAnimation.md 的衔接

- `SkinnedComponent.currentClip` 目前是 `std::string`。anim 原子化后，运行时改为"名字 → ClipHandle 查表"（`AnimationManager` 持有 Handle），状态机设计不变；
- 动画剪辑是**可寻址、可复用的数据单元**：播放/暂停/Seek（调整帧）/速度/循环全部作用于 ClipHandle；
- ANI 解析（`02_RobotAndAnimation.md` §二 已有二进制结构分析）产出的就是这种剪辑。

---

## 六、Character 复合资产（`.character`）

### 6.1 结构

```json
{
    "version": 1,
    "mesh":       "Content/robo/KD-03.dxmesh",
    "skeleton":   "Content/robo/KD-03.bone",
    "materials":  ["Content/robo/mat_body.material", "Content/robo/mat_head.material"],
    "clips": {
        "idle":   "Content/robo/anim_idle.anim",
        "run":    "Content/robo/anim_run.anim",
        "jump":   "Content/robo/anim_jump.anim",
        "attack": "Content/robo/anim_attack.anim"
    },
    "defaultClip": "idle"
}
```

| 字段 | 引用 | 说明 |
|:-----|:-----|:-----|
| `mesh` | `.dxmesh` | 含 SubMesh 表 + SkinCluster，与 `materials` 一一对应（材质槽） |
| `skeleton` | `.bone` | 骨骼树 + rest pose |
| `materials` | `.material` 数组 | 长度 = SubMesh 数 |
| `clips` | `.anim` 字典 | 动画剪辑引用表（名字 → 剪辑） |
| `defaultClip` | clips 键 | 加载后默认播放 |

> **动画剪辑归属（方案 C 简化版）**：`.anim` 独立原子资产不变；`.character` 声明自己的 clips。骨架资产保持纯净；将来需要"同骨架共享动画"时，在骨架侧加一个**可选 `defaultClips` 段**（纯字符串表，不持有 Handle，不破坏原子性），角色可覆盖。

### 6.2 加载时序

沿用复合资产统一时序：**依赖（原子资产）先于复合资产就绪**。

```
AssetManager::Load("KD-03.character")
  └─ CharacterLoader 解析 JSON
       ├─ 识别依赖: mesh / skeleton / materials[] / clips[]
       ├─ 递归加载原子资产:
       │    ├─ Load("KD-03.dxmesh")  → GeometryHandle
       │    ├─ Load("KD-03.bone")    → SkeletonHandle
       │    ├─ Load("mat_*.material")→ MaterialHandle
       │    └─ Load("anim_*.anim")   → ClipHandle
       └─ 依赖全部就绪后，创建 Character Handle:
            ├─ 持有 GeometryHandle / SkeletonHandle
            ├─ 持有 MaterialHandle[] / ClipHandle 查表
            └─ 记录 defaultClip
```

### 6.3 场景 JSON 引用（实例化）

```json
{
    "name": "KD-03",
    "persistentId": 1001,
    "components": {
        "transform": { "position": [0, 0, 0] },
        "character": {
            "asset": "Content/characters/KD-03.character",
            "startClip": "idle"
        }
    }
}
```

场景只放**实例**（位置 + 角色引用 + 初始动画），角色定义收敛到单个资产文件。

---

## 七、运行时装配（ECS 映射）

```
角色实体（ECS）
 ├─ TransformComponent      ← 位置/旋转
 ├─ MeshComponent           ← materialSlots[]（来自 .character 的材质槽）
 ├─ SkinnedComponent        ← skeletonHandle + currentClip + timePos + boneBufferIndex
 └─ AnimationComponent      ← 状态机状态（播放/暂停/速度/循环）【后续】
```

| System | 类型 | 职责 |
|:-----|:-----|:-----|
| `AnimationAdvancer` | AlwaysRun（高频） | 每帧推进 timePos，插值骨骼矩阵，写 GPU 骨骼缓冲 |
| `AnimationStateMachine` | WithMessage（离散） | Play/Pause/**Seek**/Stop 动画切换 |

- **调帧能力**：Seek = 写 `SkinnedComponent.timePos` + 暂停推进——数据在 ECS 组件上，任何 System 都能读改，天然支持"播放动画和调整动画帧"；
- Builder/Renderer 只读组件（`SkinnedRenderItemBuilder` + `SkinnedRenderer`），不感知游戏逻辑（现有设计，见 `SkinnedAnimation.md`）。

---

## 八、AssetType 扩展与同步修改清单

```cpp
enum class AssetType : uint8_t {
    // 原子资产
    Mesh,        // .dxmesh, .obj
    Texture,     // .dds, .png, .jpg
    Material,    // .material
    Skeleton,    // .bone      ← 新增
    Animation,   // .anim      ← 由预留转正

    // 复合资产
    Terrain,     // .terrain
    Scene,       // .scene
    Character,   // .character ← 新增
    Prefab,      // .prefab    （预留）
    ParticleSystem, // .particle（预留）
    Audio        // 预留
};
```

新增/转正类型需同步修改（详见 `AssetTypeDefinition.md`）：

1. `Engine/Asset/Definitions/AssetType.h` 枚举；
2. `AssetManager::Load()` switch 分支（或按扩展名注册表）；
3. `FileIconProvider.cpp` 扩展名映射 + 图标；
4. `SceneConstructor` 依赖收集（`.character` 的依赖递归收集）；
5. 加载器注册表：`SkeletonLoader`（.bone）、`AnimLoader`（.anim）、`CharacterLoader`（.character）；
6. 资产预览（`.character` 预览可复用 Mesh 预览路径）。

---

## 九、实施路线

| 阶段 | 内容 | 依赖 |
|:----|:------|:------|
| A | `.bone` 格式 + `SkeletonManager::LoadFromJSON` | ✅ 已完成（2026-07-31，含转置 bug 修复，与 nested.x 验证一致） |
| B1 | **ANI 解析器**：文件名头 + HOD/HD2 块序列 → 每帧部件局部矩阵 → 按组提取（Tail 状态机） | ✅ 已完成（2026-07-31，`ANIParser`，1.008 + PUK 双格式，标记法 + 母版驱动，25 机体全量拆解） |
| B1.5 | **ANI 母版管线**：`ANIParser::GetMaster()` 解析母版骨架（部件名 + A/B 层级）；`RobotMerger::MergeFromANI()` 母版驱动合并 → 输出 x/fbx/bone | ✅ 已实现（2026-07-31；绑定矩阵暂用同目录 Robo.hod，母版骨架用于校验，PUK 母版数据区 TRS 格式待逆向） |
| B2 | `.anim` 现代化资产：全量转切分剪辑 + `AnimLoader` 接入 AssetManager | B1 |
| B2.5 | **动画 + Tail 合并输出**：全部动画帧矩阵 + Tail 状态机按组分隔输出到单一 TXT（不分文件夹） | B1 + B1.5 |
| C | `.character` 格式 + CharacterLoader + 依赖收集 | A + B + 材质槽数组化 |
| D | 场景 `character` 组件 + SceneConstructor 装配 | C |
| E | 动画状态机（Play/Pause/Seek） | B + SkinnedAnimation.md |
| F | 挂点系统（socket 消费） | 现有 RelationshipComponent |
| G | **运行时 IK（脚步着地 + 手部瞄准）** | 蒙皮管线（SkeletalMeshAssetPipeline）+ 骨架资产 |

> **IK 方案（2026-08-01 定案，B 方案已实现）**：
> - 关节**必然多轴**（EXVS 目标 + 四台机体实测确认），求解器选 **FABRIK/CCD 无轴约束迭代求解**，不做铰链锁轴
> - **B 方案 ✅ 已完成**：AssetTool/CLI 新增 `ani2ik` 命令 + `IKSolver.h/.cpp`（FABRIK + 母版驱动链识别），KD-03/KD-06 离线验证通过（链识别 4/4、可达目标精确收敛、大写变体 Arm1→Arm2→Arm3→Hand 兼容）。详见 `02_RobotAndAnimation.md` §8.5
> - **求解器暂不移入引擎**（用户决策）；通用 IK 库只覆盖位置求解、链识别/矩阵往返需自研，暂不引入第三方库
> - 引擎侧 IK 链定义必须**母版驱动**（机体间骨骼命名不统一，KD-06 大写变体），详见 `02_RobotAndAnimation.md` §8

> **分工原则（2026-07-31 定案）**：ANI 逆向分两步走——**先实现解析器**（得到骨骼关键帧数据），**如何变为现代化资产（.anim 格式 + 加载器）是下一步的事**。HOD 结果（.bone）已可作初始姿势/动画帧 0，ANI 解析时以母版骨架（HD2 类型=0 / HOD 首块）为命名契约，骨骼变换统一公式（Body_d 修正 + 层级累乘 + Ry180°）对 HOD 与 ANI 通用。
>
> **步骤划分（2026-07-31 更新）**：导入机体的合并部件功能调整为——① 解析 ANI 母版拿骨骼（部件名 + A/B）；② 合并部件输出母版对应 x/fbx/bone；③ 拿全部动画数据，Tail 按动画数据分隔输出到同一 TXT（不分文件夹）。当前完成 ①②（B1.5），③ 为下一任务（B2.5）。最终目标是包含动画信息的 FBX + 状态机 Tail 文本。

---

## 十、相关文档

- `AssetSpecification.md` — 资产规范（原子/复合资产表）
- `AssetTypeDefinition.md` — AssetType 扩展清单
- `SkeletalMeshAssetPipeline.md` — 骨骼网格管线、运行时缺口
- `SkinnedAnimation.md` — 动画组件与 System 设计
- `02_RobotAndAnimation.md` — HOD/ANI 源格式分析
