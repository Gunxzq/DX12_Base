# 资产生成器与预制体

## 概念

**预制体（Prefab）** 是"一组资产 + 逻辑描述 → ECS Entity"的模板。

```
JSON 描述（预制体）
  ├── 引用资产（.dxmesh / .mat / .dds）
  ├── 引用数据表（状态机、基础数值、掉落表...）
  └── 组件列表（Transform, Mesh, Health, AI...）
       ↓
生成器（PrefabGenerator）
  ├── 解析 JSON
  ├── 加载资产（走 AssetManager 异步管线）
  └── 构建 ECS Entity + 组件
```

## 与现有系统的关系

| 层次 | 负责 | 当前状态 |
|:-----|:------|:---------|
| **资产加载** | 文件 → GPU Handle | ✅ AssetManager + 三系统 |
| **场景构造** | Handle 列表 → ECS Entity | ◐ 实施中 |
| **预制体** | JSON 模板 → 场景构造的输入 | 📋 本文规划 |
| **数据表** | 基础数值/状态机/掉落表 → 预制体引用 | ❌ 未规划 |

## 预制体 JSON Schema（草案）

```json
{
    "prefabs": {
        "soldier": {
            "components": {
                "transform": { "scale": [0.05, 0.05, 0.05] },
                "mesh": { "geometry": "soldier", "material": "soldier_mat" },
                "opaque": null
            }
        },
        "health_pickup": {
            "components": {
                "mesh": { "geometry": "cube", "material": "glow" },
                "transparent": null,
                "pickup": { "healAmount": 25, "type": "health" }
            }
        }
    }
}
```

场景 JSON 引用预制体：

```json
{
    "entities": [
        { "prefab": "soldier", "transform": { "position": [0, 30, 0] } },
        { "prefab": "health_pickup", "transform": { "position": [5, 30, 5] } }
    ]
}
```

预制体中的 `"ref:xxx"` 语法表示间接引用，由生成器在加载时解析为具体数据。

## 生成器接口

```cpp
class IAssetGenerator {
    virtual ~IAssetGenerator() = default;
    // 返回此生成器能处理的 JSON 组件名列表
    virtual std::vector<std::string> GetComponentNames() const = 0;
    // 解析 JSON → 添加 ECS 组件
    virtual void Generate(Entity entity, const nlohmann::json &componentJson,
                          GameContext *context, const AssetMap &assets) = 0;
};
```

生成器注册到 `SceneConstructor`，按 entity 的组件名分发。
