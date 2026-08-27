# Snapshot System 设计文档

## 1. 概述

Snapshot System 是一个**引擎专用的轻量级版本化事务引擎**，提供工作区 → 暂存区 → 提交三区模型。它不是资产监听器，而是**操作与状态的快照管理器**。

### 1.1 设计目标

- 编辑器模式下，用户连续操作不被打断，完成一系列工作后**统一提交**
- 每次提交是一个**原子变更集**，要么整体生效，要么整体回退
- 支持提交历史回退（类似 `git log` + `git checkout`）
- 只在编辑器模式下启用（`#if WITH_EDITOR`），发布版没有快照系统

### 1.2 三层抽象

| 层级 | 关注什么 | 实现模块 | 对应现实 |
|------|----------|----------|----------|
| **L3** | 用户的操作意图（命令序列） | `OperationRecorder` | undo/redo、双向同步回放 |
| **L2** | 资产的属性级变更 | `PropertyTracker` | diff、冲突检测、合并 |
| **L1** | 文件的完整内容快照 | `FileSnapshot` | Git 式回退、完整性验证 |

Snapshot System 是顶层的组合者——三层的职责独立，但一次 Commit 可能同时包含三层的数据。

### 1.3 L3 记录范围的约束

操作序列（L3）**只记录引擎内部的 UI 操作**。外部工具（PS、Blender、VS Code）保存文件导致的变更绕过了引擎，无法记录操作意图，但引擎会通过文件系统监知晓文件发生了变更。

三层的关系受此约束：

| 变更来源 | L1 文件快照 | L2 属性跟踪 | L3 操作序列 |
|----------|------------|------------|------------|
| 引擎 UI 操作（调参数、拖实体） | ✅ 有 | ✅ 有 | ✅ **有** |
| 外部工具保存文件（PS 存 DDS） | ✅ 有 | ✅ 有（可推导） | ❌ 空（ExternalFileChange 占位） |

**多帧包含**（即双向同步中的命令序列多帧回放）仅适用于 L3 不为空的 Commit。对于外部工具变更，游戏进程直接重载文件，不涉及逐帧操作回放。

---

## 2. 三区模型

```
┌──────────────────────────────────────────────────────────────┐
│                    Snapshot System                            │
│                                                              │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐   │
│  │   工作区      │    │   暂存区      │    │   提交历史    │   │
│  │ Working Dir  │ →  │  Staging     │ →  │  Commits     │   │
│  │              │    │              │    │              │   │
│  │ 用户改文件    │    │ 累积变更      │    │ commit_0017  │   │
│  │ 记录操作序列  │    │ 可审查/丢弃   │    │ commit_0016  │   │
│  │ 不触发加载    │    │ 不触发加载    │    │ commit_0015  │   │
│  └──────────────┘    └──────────────┘    │    ...        │   │
│                                           └──────────────┘   │
└──────────────────────────────────────────────────────────────┘
```

### 2.1 工作区

用户操作的累积区，不触发任何资源加载。

- 文件保存 → 计算哈希，记入暂存索引
- 操作记录 → 追加到操作序列缓冲区
- 属性变更 → 记录旧值和新值到 diff 缓存

### 2.2 暂存区

用户可以查看"哪些资产变了"、"做了哪些操作"，可以丢弃部分或全部变更。

```json
// .snapshot/staging.json
{
  "version": 1,
  "files": {
    "Textures/brick_d.dds": {
      "old_hash": "abcdef...",
      "new_hash": "1234abcd...",
      "status": "modified"
    }
  },
  "operations": [
    {"type": "set_param", "target": "brick_mat", "param": "roughness", "new_value": 0.8},
    {"type": "replace_texture", "target": "brick_mat", "new_source": "Textures/brick_v2.dds"}
  ],
  "properties": {
    "Materials/brick_mat.json": {
      "roughness": {"old": 0.5, "new": 0.8}
    }
  }
}
```

### 2.3 提交

暂存区的内容经过验证后打包为 Commit，触发真正的资源加载和原子切换。

---

## 3. 三层实现

### 3.1 L3 — OperationRecorder（操作序列）

记录用户的操作意图，而非操作结果。

> **命令序列与录制功能紧密联系（2026-08-11 用户补充）**：录制（OperationRecorder /
> `CameraCommandRecorder`）产出**命令序列**（有序 Operation / 相机增量命令），命令序列是
> **回放与对比的共同载体**——单帧步进（`SingleFrameStep.md` §2.5）按帧重演相机命令触发
> 相机移动、SnapshotSystem 游戏进程回放按帧重演操作序列，二者都消费录制的命令序列；
> 录制 ↔ 命令序列 ↔ 回放是一体三态，同一模式的两个实例（操作级 vs 相机输入级）。

```cpp
// 记录的操作类型（示例）
enum class OperationType {
    SetMaterialParam,    // 设材质参数
    ReplaceTexture,      // 替换纹理
    MoveEntity,          // 移动实体
    RotateEntity,        // 旋转实体
    AddEntity,           // 添加实体
    RemoveEntity,        // 删除实体
    SetEnvironmentLight, // 设置环境光照
};

struct Operation {
    OperationType type;
    std::string targetId;    // 目标标识（实体 ID、材质路径等）
    std::unordered_map<std::string, std::string> params; // 操作参数
    Timestamp timestamp;
};
```

操作序列的两大用途：

| 用途 | 场景 |
|------|------|
| **游戏进程回放** | Editor → Game 下行同步，游戏端按帧重演操作序列 |
| **Undo/Redo** | 用户撤销时反向执行操作（或切换到上一个 Commit） |

### 3.2 L2 — PropertyTracker（属性跟踪）

关注资产内部字段级别的变化，用于冲突检测和差异比较。

```cpp
// 属性变更条目
struct PropertyChange {
    std::string assetPath;     // 资产路径
    std::string propertyPath;  // 属性路径（如 "materials[0].roughness"）
    std::vector<uint8_t> oldValue; // 旧值（序列化 blob）
    std::vector<uint8_t> newValue; // 新值
};
```

解决的问题：
- 冲突检测：两个操作改了同一个属性 → 决定保留哪个
- 合并时精细选择：可以逐属性决定"用旧版"还是"用新版"

### 3.3 L1 — FileSnapshot（文件快照）

最底层的内容寻址存储，提供完整性保证和回退能力。

```cpp
struct SnapshotEntry {
    std::string filePath;      // 相对项目根的文件路径
    std::string contentHash;   // SHA256
    uint64_t fileSize;
};
```

存储布局（类似 Git 的对象存储）：

```
.snapshot/
├── objects/
│   ├── ab/
│   │   └── cdef1234567890abcdef1234567890abcdef12    # 纹理文件内容
│   ├── 01/
│   │   └── 2345abcdef6789abcdef0123456789abcdef34    # JSON 配置文件内容
│   └── ...
├── refs/
│   └── heads/
│       └── main                                        # 当前 HEAD → commit hash
├── commits/
│   ├── commit_0017.json                                # 完整提交记录
│   ├── commit_0016.json
│   └── ...
└── staging.json                                        # 暂存区
```

### 3.4 三层组合关系

```cpp
// Snapshot System 的 Commit 数据结构
struct Commit {
    CommitId parent;                   // 父提交 ID

    // 三层数据（可选，可以只包含其中某几层）
    std::vector<Operation> operations; // L3: 操作序列
    PropertyDiff properties;           // L2: 属性变更
    AssetSnapshot snapshot;            // L1: 文件快照

    std::string message;
    Timestamp timestamp;
};
```

一次 Commit 不要求三层全满。例如：

- 只改材质参数（不涉及文件）→ L3 + L2 即可，L1 为空
- 批量替换纹理（不记录操作意图）→ L1 即可，L2/L3 为空
- 完整的"调参数 + 换纹理"组合 → 三层全满

---

## 4. 与帧驱动器的集成

### 4.1 主循环中的位置

```cpp
bool FrameDriver::Tick() {
    // ── [新增] 消费快照系统事件 ──
    SnapshotSystem::GetInstance().DispatchEvents();

    // ── 原有的输入/消息处理 ──
    m_gameContext->InputMgr->BeginFrame();
    m_gameContext->Window->ProcessMessages();
    // ...
}
```

### 4.2 提交 → 加载 → 切换流程

```
帧 N:
  DispatchEvents → 检测到新的 Commit
  开始后台加载 Commit 涉及的所有资产到新堆
  ... 主循环继续运行，旧堆正常服务 ...

  后台加载完成
  FrameSync → 原子切换堆指针（新堆生效）
  旧堆标记为"待回收"（3 帧后释放）

帧 N+1 开始:
  Render Phase → 使用新堆渲染
```

Commit 的原子性保证：

- 加载阶段：如果任何一个资产加载失败 → **提交失败**，不切换堆，编辑器保持之前的状态
- 切换阶段：FrameSync 回调中指针切换是 O(1) 的，不会因为 Commit 内容多少而卡顿
- 回退：如果新效果不满意，可以 `checkout` 上一个 Commit，重新加载旧资产

---

## 5. 与游戏进程双向同步的关系

Snapshot System 的 Commit 天然就是双向同步的通信单元。

### 下行（Editor → Game）

Editor 提交一个 Commit → Commit 序列化 → 通过数据驱动通道发送给 Game 进程 → Game 端解析 Commit 的三层数据：
- L3 操作序列：游戏端逐帧重演
- L2 属性变更：直接更新对应组件
- L1 文件快照：触发资源加载

### 上行（Game → Editor）

游戏进程的操作（如"AI 推动了一个物体"）被录制为 L3 操作序列 → 反向 Commit → 编辑器端应用，更新场景图。

### 冲突解决

当 Editor 和 Game 同时修改了同一个属性时（双向同步的经典难题）：

1. **L2 PropertyTracker** 可以提供旧值和新值的对比
2. **L3 操作序列** 提供了"为什么改"的上下文
3. 冲突解决规则：以时间戳更新的为准，或由用户手动选择

---

## 6. 与热重载 / 用户替换资源的关系

| 场景 | 模式 | 快照系统 | 是否需要重启 |
|------|------|----------|-------------|
| 编辑器中改参数 → 提交 | 编辑器 (`WITH_EDITOR`) | ✅ 完整工作 | ❌ 不重启 |
| 编辑器中改纹理 → 提交 | 编辑器 (`WITH_EDITOR`) | ✅ 完整工作 | ❌ 不重启 |
| 发布版用户替换 .pak 文件 | 发布版 | ❌ 不编译 | ✅ 必须重启 |

---

## 7. 与 Git 的共存关系

| | Git | Snapshot System |
|--|-----|----------------|
| 用途 | 团队协作、代码版本管理、CI/CD | 引擎资产热重载、操作序列、运行时回放 |
| 粒度 | 文件行级 | 操作级 + 属性级 + 文件级 |
| 存储 | 完整历史、跨项目 | 编辑器会话期间，重启后从磁盘重建 |
| 实时性 | 离线 | 实时（每帧检查暂存区） |

两者不冲突——Git 管理代码，Snapshot System 管理引擎运行时的资产状态。

---

## 8. 待细化

- 暂存区与文件系统的同步机制（编辑器中文件保存到工作区和暂存区更新的映射）
- Commit 验证流程的详细设计（资产加载并行化 + 错误回滚）
- OperationRecorder 的操作类型枚举的完整定义
- 与现有 ECS 系统的集成方式（操作序列直接修改 Component？）
- 游戏进程回放操作的确定性保证
