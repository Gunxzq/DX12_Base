# .anim 动画资产格式规范（AnimationAsset）

> 状态：📋 新设计（2026-08-01）
> 关联：`CharacterAsset.md` §五（.anim 草案）、`SkinnedAnimation.md`（动画 System 设计）、`AssetSpecification.md`（原子资产规范）、`07_EngineAssetPipeline.md`（管线优先级）
> 分工定位：**Blender 生产动画 FBX → AssetTool 转换器产出 .anim → 引擎 AnimLoader 加载 + AnimationManager 播放**。引擎不做动画编辑（非建模软件），只做运行时采样/插值/播放。

---

## 一、资产定位与原则

`.anim` 是**原子资产**：一个骨骼动画剪辑（时长、帧率、循环标志、通道曲线），不持有任何其他资产的引用。

| 原子资产 | 隐含依赖 | 性质 |
|:-----|:-----|:-----|
| `.dxmesh` | 顶点 `boneIndices` 隐含依赖骨骼序号 | 原子 |
| `.bone` | 无 | 原子 |
| **`.anim`** | 通道 `bone` 名隐含依赖骨骼命名 | 原子（命名约定，非引用） |

- 通道 `bone` 字段是**命名约定字符串**，运行时由 AnimLoader 一次性 hash 匹配到 `SkeletonData::BoneNames[]`（顺序无关，比序号对齐更鲁棒）；
- 一个 `.anim` 可复用于任何骨骼命名兼容的骨架（换皮/变体）。

---

## 二、文件格式（JSON）

```json
{
    "version": 1,
    "name": "robo_stand_1",
    "duration": 2.0,
    "fps": 10,
    "loop": true,
    "channels": [
        {
            "bone": "Body_d",
            "keyframes": [
                { "t": 0.0, "position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1] },
                { "t": 0.1, "position": [0.5, 0, 0], "rotation": [0.707, 0, 0, 0.707], "scale": [1, 1, 1] }
            ]
        },
        {
            "bone": "arm1",
            "keyframes": [
                { "t": 0.0, "position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1] },
                { "t": 0.1, "position": [0, 0.2, 0], "rotation": [0.5, 0.5, 0, 0.707], "scale": [1, 1, 1] }
            ]
        }
    ]
}
```

### 字段说明

| 字段 | 类型 | 说明 |
|:-----|:-----|:-----|
| `version` | int | 格式版本，当前 = 1 |
| `name` | string | 剪辑名（应与 `.character` 的 `clips` 键一致；转换器默认取动画名） |
| `duration` | float | 剪辑总时长（秒）= 最大键帧时间，转换器从 FBX `mDuration / mTicksPerSecond` 计算 |
| `fps` | float | 采样帧率（转换器从 FBX `mTicksPerSecond` 取；ANI 帧率来自 Tail 头速度值，回退 30） |
| `loop` | bool | 循环标志（默认 true） |
| `channels` | array | 骨骼通道数组，每项对应一根骨骼的完整动画曲线 |

### 通道（channel）

| 字段 | 类型 | 说明 |
|:-----|:-----|:-----|
| `bone` | string | 骨骼名（**去 `_bone` 后缀、过滤 `_end` 末端后的引擎骨骼名**，须与 `.bone` 的 `BoneNames` 匹配） |
| `keyframes` | array | 键帧数组，按 `t` 升序 |

### 键帧（keyframe）

| 字段 | 类型 | 说明 |
|:-----|:-----|:-----|
| `t` | float | 时间（秒） |
| `position` | [x,y,z] | 平移（引擎空间 Y-up） |
| `rotation` | [x,y,z,w] | 旋转四元数（w 在末位） |
| `scale` | [x,y,z] | 缩放（默认 1,1,1） |

> **坐标系**：.anim 内 TRS 为**引擎空间（Y-up 左手系）**局部变换，AssetTool 转换时负责 FBX 右手 Y-up → 引擎左手 Y-up（翻转 Z）。引擎侧不做坐标系转换（与 `.bone` 约定一致）。

---

## 三、键帧插值规则（引擎侧）

引擎复用现有 `SkeletonData` 的插值实现（`SkeletonData.cpp`），不做新的插值器：

| 分量 | 插值方式 |
|:-----|:---------|
| position | 线性插值（`XMVectorLerp`） |
| scale | 线性插值（`XMVectorLerp`） |
| rotation | 四元数球面插值（`XMQuaternionSlerp`） |
| 越界 | `t <= 首帧` 取首帧；`t >= 末帧` 取末帧（不循环时钳制） |

- 引擎现有 `BoneAnimation::Interpolate` 已实现以上规则，AnimLoader 只需把 `.anim` 键帧填充到 `BoneAnimation::Keyframes`（`Keyframe{TimePos, Translation, Scale, RotationQuat}`）；
- 键帧时间轴**允许非均匀**（FBX 导出可能丢帧），插值按实际 `t` 计算，不假定等间距。

---

## 四、FBX → .anim 转换映射（AssetTool 转换器）

### 4.1 数据源

引擎动画唯一来源 = **Blender 优化后的最终动画 FBX**（含 AnimStack）。原始 ANI 经 `ani2anim` 产出的 FBX 只是进 Blender 的桥，最终以 Blender 导出为准。

### 4.2 切分规则（每个 AnimStack → 一个 .anim）

| FBX 元素 | .anim 映射 |
|:-----|:-----|
| 一个 `aiAnimation`（AnimStack） | 一个 `.anim` 剪辑 |
| 动画名 `mName` | 命名规约（2026-08-01 定案）：Blender AnimStack 名形如 `Armature|Armature|动作名_序号`，**三段名只保留最后一段具体动作名**（`rfind('|')` 取末段）作为 `.anim` 的 `name` 与输出文件名；无 `\|` 时原样保留 |
| `mDuration` / `mTicksPerSecond` | `duration` / `fps` |
| `aiNodeAnim` 通道 | 一个 `channels[]` 项 |
| 通道名 `mNodeName`（如 `Body_d_bone`） | 去 `_bone` 后缀 → `Body_d`；过滤 `_end` 末端节点 |
| `mPositionKeys` / `mRotationKeys` / `mScalingKeys` | 键帧数组（按时间轴对齐合并） |

### 4.3 转换步骤

```
AssetTool anim2clip <model_anim.fbx> <output_dir> [--clip name1,name2]
  ├─ assimp 读 FBX
  ├─ 遍历 scene->mAnimations：
  │    1. 按名字过滤（--clip 可选；缺省全部）
  │    2. 生成唯一文件名（重名追加序号，参考 ani2anim 的 {name}_{n} 规则）
  │    3. 对每个 aiNodeAnim：
  │       - 通道名去 _bone 后缀、过滤 _end → 骨骼名
  │       - 合并 position/rotation/scale 键帧（按时间对齐）
  │       - 坐标系翻转 Z（右手 → 左手 Y-up）
  │    4. 写 {name}.anim（JSON）
  └─ 输出摘要：N 个剪辑、每剪辑通道数/键帧数
```

### 4.4 与骨骼名匹配

`.anim` 的 `bone` 名 = `.bone` 的 `BoneNames[i]`（同名同序约定）。AnimLoader 加载时：
1. 读 `.bone` 的 `BoneNames`，建 `名字 → 索引` 哈希表（一次性）；
2. 遍历 `.anim` 通道，按 `bone` 名查表写入 `AnimationClip::BoneAnimations[boneIndex]`；
3. 未匹配的通道丢弃 + 警告（骨架/动画版本不匹配时容错）。

---

## 五、AnimLoader 与 AnimationManager（引擎侧）

### 5.1 AnimLoader

```
纯 CPU 函数（线程安全，后台线程解析）
bool ParseAnimFile(const std::string &animPath, AnimationClip &outClip,
                   const std::vector<std::string> &boneNames);
```

- 解析 JSON → 填充 `AnimationClip`（`BoneAnimations` 按骨骼索引对齐）；
- 依赖 `.bone` 的 `BoneNames` 做名字匹配（**非线程安全数据由调用方保证**）；
- 与 `SkeletonLoadTask` 同模式：`cpuWork` 后台解析，`onComplete` 主线程注册。

### 5.2 AnimationManager

```
ClipHandle（句柄 + 代际，参照 SkeletonHandle）
AnimationManager::LoadFromJSON(animPath, boneNames) → ClipHandle
```

- 管理 `.anim` 的注册/查询/引用计数（Retain/Release/Reclaim），与 `SkeletonManager` 同构；
- 剪辑数据存 `AnimationClip`（复用 `SkeletonData` 的结构），**不复制到骨架**——骨架 `SkeletonData::Animations` 保持为空，剪辑独立持有，`ComputeFinalTransforms` 改为接收 `ClipHandle` 采样（改造点见 §5.3）。

### 5.3 与现有 SkeletonData 的衔接（改造点）

当前 `SkeletonManager::ComputeFinalTransforms(handle, clipName, timePos)` 从骨架内置 `Animations` 取剪辑（M3D 遗留结构）。.anim 原子化后：

| 现状 | 改造后 |
|:-----|:-----|
| `SkeletonData::Animations`（内嵌剪辑） | 移除/置空；剪辑由 AnimationManager 独立持有 |
| `ComputeFinalTransforms(handle, clipName, t)` | `ComputeFinalTransforms(handle, clipHandle, t)` 或 AnimationSystem 内部：采样 `AnimationClip` → 与骨架层级/偏移矩阵合成 |
| `GetClipDuration(handle, clipName)` | 从 ClipHandle 查 |

合成逻辑不变（`toParent` → `toRoot` → `× BoneOffsets`，见 `SkeletonData::GetFinalTransforms`），只是剪辑数据来源从骨架改为 ClipHandle。

---

## 六、播放与状态机（复用既有设计）

- **AnimationAdvancer**（AlwaysRun 常驻）：每帧推进 `SkinnedComponent.timePos`，采样当前 ClipHandle → 骨骼最终矩阵 → 上传 GPU 骨骼缓冲 → 写 `boneBufferAddress`（详见 `SkinnedAnimation.md`）；
- **AnimationStateMachine**（WithMessage）：Play/Pause/Seek/Stop 切换（`SkinnedAnimation.md`）；
- `SkinnedComponent.currentClip` 由 `std::string` 改为 `ClipHandle`（查表语义不变，见 `CharacterAsset.md` §5.2）。

---

## 七、优先级与实施（并入 07_EngineAssetPipeline.md 待办）

| # | 任务 | 优先级 | 依赖 |
|:-:|:-----|:-------|:-----|
| 1 | **AssetTool `anim2clip` 转换器**（FBX 动画 → .anim） | P1 | FBX 动画资产（已有 kd-03 动画 FBX） |
| 2 | **AnimationManager + AnimLoader**（.anim 加载/注册/ClipHandle） | P1 | .bone 加载（已完成） |
| 3 | **ComputeFinalTransforms 改造**（clipName → ClipHandle） | P1 | #2 |
| 4 | **AnimationAdvancer 骨骼缓冲上传**（每帧采样 → GPU） | P1 | #3 + 蒙皮渲染链路 |
| 5 | **动画视口**（角色/动画预览面板，见 AnimationViewport.md） | P1 | #4 |
| 6 | `.character` 复合资产 + 场景实例化（NPC 场景引用） | P2 | #5 |

> **范围约束（2026-08-01 用户定案）**：角色**不进主 viewport**，通过独立动画视口查看；`SceneConstructor`（场景生成器）**暂不修改**——角色场景化（NPC）留到 .character 资产就绪后（#6），不改现有生成逻辑。

---

## 八、相关文档

- `CharacterAsset.md` — 角色复合资产（.character 打包 .anim 剪辑表）
- `SkinnedAnimation.md` — AnimationAdvancer / AnimationStateMachine 设计
- `SkeletonData.h/.cpp` — 引擎插值与最终变换合成（复用）
- `SkeletonLoader.h` — ParseBoneFile（同名匹配的 .bone 依赖）
- `07_EngineAssetPipeline.md` — 管线优先级与分工
