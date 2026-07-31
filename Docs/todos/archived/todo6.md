基于你的需求——**JSON 场景配置 + 解释器加载 + 运行时静态场景 + 无需冒泡事件**——我建议选择**扁平化 ECS + 加载时预计算世界矩阵**的设计。

## 结论：选择扁平化 ECS，场景加载时预计算矩阵

### 核心理由

| 需求 | 场景树方案 | 扁平化 ECS 方案 |
|------|-----------|-----------------|
| JSON 嵌套存储 | ✅ 天然支持 | ✅ 可以支持（加载时展平） |
| 一次加载预计算 | ✅ 需要遍历树 | ✅ 遍历扁平数组即可 |
| 运行时父子关系 | ❌ 必须维护树结构 | ❌ 不需要 |
| 事件冒泡 | ❌ 低效（你说了不需要） | ✅ 无 |
| ECS 查询性能 | ❌ 查询需考虑层级 | ✅ 直接查组件 |
| 确定性/帧同步 | ❌ 树状态难同步 | ✅ 扁平数据易同步 |

**场景树是为“运行时动态父子关系”和“编辑器 UI 友好”设计的，你的需求都不需要。**

---

## 推荐方案：JSON 嵌套 + 加载时展平

### JSON 配置示例

```json
{
  "entities": [
    {
      "name": "LevelRoot",
      "transform": { "position": [0, 0, 0] },
      "children": [
        {
          "name": "Player",
          "mesh": "cube",
          "transform": { "position": [0, 2, 0] }  // 相对父级
        },
        {
          "name": "Ground",
          "mesh": "plane",
          "transform": { "position": [0, -1, 0] }  // 相对父级
        }
      ]
    }
  ]
}
```

### 加载过程

```cpp
class SceneLoader {
    void LoadScene(const std::string& jsonPath) {
        // 1. 解析 JSON 为嵌套结构
        auto root = ParseJSON(jsonPath);
        
        // 2. 递归遍历，计算每个实体的世界矩阵
        std::vector<EntityData> flatEntities;
        TraverseNode(root, XMMatrixIdentity(), flatEntities);
        
        // 3. 批量创建 EnTT 实体
        for (auto& data : flatEntities) {
            auto entity = registry.create();
            registry.emplace<TransformComponent>(entity, data.worldMatrix);
            registry.emplace<MeshComponent>(entity, data.meshHandle);
            registry.emplace<NameComponent>(entity, data.name);
        }
    }
    
    void TraverseNode(const JSONNode& node, XMMATRIX parentMatrix, std::vector<EntityData>& out) {
        // 计算当前节点的世界矩阵
        XMMATRIX localMatrix = ComputeLocalMatrix(node.transform);
        XMMATRIX worldMatrix = localMatrix * parentMatrix;
        
        // 存储展平后的实体（丢失父子关系）
        out.push_back({ node.name, node.mesh, worldMatrix });
        
        // 递归子节点
        for (auto& child : node.children) {
            TraverseNode(child, worldMatrix, out);
        }
    }
};
```

### 加载后的状态

```
EnTT Registry:
  Entity 1: TransformComponent(世界矩阵) + MeshComponent + NameComponent("Player")
  Entity 2: TransformComponent(世界矩阵) + MeshComponent + NameComponent("Ground")
  ...
```

**运行时完全不需要父子关系**，所有实体都是独立的。

---

## 如果需要“运行时动态父子关系”

如果以后需要（如武器附着、载具上的人），加一个可选的 `ParentComponent`：

```cpp
struct ParentComponent {
    Entity parent;
    XMFLOAT3 localPosition;  // 相对于父级的偏移
    XMFLOAT3 localRotation;
};

// 每帧由系统更新世界矩阵
class HierarchySystem {
    void Update() {
        auto view = registry.view<ParentComponent, TransformComponent>();
        for (auto entity : view) {
            auto& parent = view.get<ParentComponent>(entity);
            auto& transform = view.get<TransformComponent>(entity);
            auto& parentTransform = registry.get<TransformComponent>(parent.parent);
            
            // 计算世界矩阵 = 局部矩阵 * 父级世界矩阵
            transform.worldMatrix = transform.localMatrix * parentTransform.worldMatrix;
        }
    }
};
```

**但你的场景不需要——你说“场景不太容易发生变化”，所以加载时预计算就够了。**

---

## 对比总结

| 特性 | 场景树 (运行时) | 加载时展平 (推荐) |
|------|----------------|-------------------|
| JSON 存储 | 嵌套，直观 | 嵌套，直观 |
| 加载速度 | 每次遍历树 | 一次遍历，存储为数组 |
| 运行时内存 | 需存储父指针 | 无额外开销 |
| 运行时计算 | 每帧更新矩阵 | 0 |
| 查询性能 | 需考虑层级 | 直接查 ECS |
| 确定性/帧同步 | 困难（树状态） | 容易（扁平数据） |
| 代码复杂度 | 高（维护树） | 低（遍历一次） |

---

## 最终建议

**采用“JSON 嵌套存储 + 加载时预计算世界矩阵 + 扁平化 ECS”方案。**

- 编辑器友好：JSON 可以嵌套，方便美术/策划配置
- 运行时高效：无需维护树，无需每帧计算，ECS 查询快速
- 确定性：所有实体独立，适合帧同步
- 简单：加载逻辑 ~100 行代码

**冒泡事件：你已经有事件系统（`MessageDispatcher`），那是全局的、并发的，比树形冒泡高效得多。**

你的项目方向很清晰——不需要被 Cocos/Godot 的模式带偏。



当然能找到同类设计。你提出的这种 **JSON 配置 + 加载时展平 + ECS 查询** 的思路，其实正是许多现代化引擎和框架为了解决传统场景树性能瓶颈而采用的主流方案。

我帮你梳理了几个可以参照的例子：

### 🎯 同类设计参考

| 方案/项目 | 核心特点 | 与你的设计相似之处 |
| :--- | :--- | :--- |
| **Bevy (Rust)**  | ECS 核心，支持**层级变换，但推荐加载时预计算**；其 **`bevy_archive` 子系统**使用 TOML/JSON 描述场景，加载时处理依赖和层级，与你的 JSON 配置 + 运行时构建思路高度一致。 | JSON配置、加载时处理、扁平化存储 |
| **Unity ECS** | 引擎深入整合 ECS，其 **Transform 系统**的理念：**逻辑层使用局部坐标（父子关系），渲染层使用预计算的世界矩阵**。官方推荐在必要时（如射线检测）才调用接口实时计算世界位置。 | 矩阵预计算、逻辑/渲染分离 |
| **freecs (Rust)**  | 提供 **World Merging** 功能，支持克隆一个世界中的实体层级到另一个世界，并自动维护和更新 Entity 间的引用关系。 | 场景加载时批量创建实体并维护关系 |
| **Godot & Cocos** | 这类引擎证明了树形结构是必要的，因为它便于编辑器操作和 UI 事件冒泡。但你完全可以在**运行时忽略此结构**，只在加载时解析 JSON 并转换为扁平 ECS 实体。 | 编辑器友好（树形存储），运行时高效（扁平使用） |
| **glitchypixel**  | 一位开发者的实践，展示了一种通用策略：用独立的 `Parent` 组件表达关系，用专门的系统管理层级，只在必要时（如 `Parent` 组件被修改）才重新计算矩阵。 | 用组件表达父子关系、按需矩阵计算 |

### 🔍 方案对比

| 特性 | 你的方案（目标） | Godot / Unity | Bevy / flecs |
| :--- | :--- | :--- | :--- |
| **存储格式** | JSON (嵌套) | 自定义二进制/文本场景格式 | TOML/JSON/Ron |
| **场景加载** | 预计算世界矩阵 | 运行时维护树 | 加载时处理，可选预计算 |
| **运行时表示** | 扁平 ECS | 树形 Node/GameObject | 扁平 ECS |
| **事件冒泡** | 不需要（有独立事件系统） | 需要（UI 交互） | 不需要 |
| **主要优势** | **性能、确定性** | **易用性、工作流** | **性能、灵活性** |

### 📝 核心设计要点总结

#### 1. 明确区分“逻辑父子”与“渲染父子”
你的理解非常到位。你的设计是基于现代 ECS 模式的典型方案：**“逻辑父子”仅作为一种数据关系，通过 `Parent` 组件来承载，而“渲染父子”则通过预计算的世界矩阵来消除层级依赖，实现逻辑与渲染的解耦**。

#### 2. 事件冒泡
这取决于应用场景。**你的项目（高性能帧同步游戏）完全不需要**。但如果未来要开发编辑器，树形结构的冒泡机制会更方便。

#### 3. ECS 中的层级变换管理
这是关键的技术点。成熟的做法是引入一个“脏标记”系统：
*   **核心组件**：`LocalTransform`（相对父级的变换）和 `GlobalTransform`（世界变换）。
*   **管理系统**：专门用一个系统来维护层级关系。当一个实体的 `LocalTransform` 或其 `Parent` 被修改时，就为其（及其所有子物体）的 `GlobalTransform` 打上“脏标记”（Dirty flag）。
*   **计算系统**：另一个或同一个系统，会检查所有“脏标记”，**在每帧的固定阶段（如你框架中的 `FrameSync` 点）统一重新计算和更新 `GlobalTransform`**。这种方式保证了计算的确定性（所有矩阵在固定时间点同步更新）和高性能（只计算变化的节点）。

---

你的设计思路完全正确，且与许多前沿 ECS 项目的方向一致。Bevy 的 `bevy_archive` 是一个非常有参考价值的案例，建议直接看它的源码实现。另外，Unity ECS 关于 `LocalToWorld` 的文档也很有启发性，能清晰看到官方推荐的用法，这与你预计算矩阵的思路不谋而合。

总的来说，**“用树形结构编辑，用扁平 ECS 运行”** 正是当前大型项目普遍追求的最佳实践。

如果你想深入参考 Bevy 的实现细节，或者需要我帮你设计 JSON 配置的具体 schema，随时可以告诉我。


