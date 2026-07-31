# BugFix: LightDesc Missing falloffStart/falloffEnd/spotPower/shadowBias 序列化字段

> 日期：2026-07-28
> 关联：属性卡（LightEditor）↔ LightDesc（序列化）↔ LightComponent（ECS）+ SceneLoader::ParseLight 四端一致性检查

---

## 现象

编辑器中通过属性卡修改点/聚光灯的 Falloff Start、Falloff End、Spot Power 参数后，保存场景再重新加载，修改的值全部丢失，恢复为默认值。

## 根因

### 第一层：LightDesc 结构体缺失字段

`LightDesc`（`Engine/Asset/IO/Loader/SceneDescription.h`）是光源数据的 JSON 序列化结构体，负责 ECS `LightComponent` 与 JSON 文件之间的传递。但该结构体缺少以下字段：

| 字段 | LightComponent | LightDesc（初始） | 结果 |
|:-----|:---------------|:-----------------|:------|
| `falloffStart` | ✅ 存在 | ❌ 缺失 | 编辑后无法保存 |
| `falloffEnd` | ✅ 存在 | ❌ 缺失 | 编辑后无法保存 |
| `spotPower` | ✅ 存在 | ❌ 缺失 | 编辑后无法保存 |
| `shadowBias` | ✅ 存在 | ❌ 缺失 | 编辑后无法保存 |

### 第二层（隐藏根因）：SceneLoader::ParseLight 是独立函数

更隐蔽的问题：`SceneLoader::ParseLight`（`Engine/Asset/IO/Loader/SceneLoader.cpp`）是**独立的硬编码解析函数**，不共用 `from_json`。即使 `from_json`/`to_json`/`ExportToDescription`/`SceneConstructor` 四端全部更新，**只要 `ParseLight` 没更新，加载时仍然丢失字段**。

```cpp
// SceneLoader::ParseLight — 最初版本
LightDesc SceneLoader::ParseLight(const nlohmann::json &j) {
    LightDesc l;
    if (j.contains("type"))     l.type = j["type"].get<std::string>();
    if (j.contains("color"))    l.color = j["color"].get<std::vector<float>>();
    if (j.contains("intensity")) l.intensity = j["intensity"].get<float>();
    if (j.contains("range"))    l.range = j["range"].get<float>();           // ← 旧：直接赋 float
    if (j.contains("spotAngle")) l.spotAngle = j["spotAngle"].get<float>();
    if (j.contains("castsShadow")) l.castsShadow = j["castsShadow"].get<float>();
    // ❌ 缺 falloffStart, falloffEnd, spotPower, shadowBias
    return l;
}
```

`ParseLight` 与 `from_json` 重复相同的解析逻辑，但彼此独立。新增字段时必须**同时更新两处**。

### 默认值碰撞（第三层）

`LightDesc::range = 0.0f` 与 `LightComponent::range = 10.0f` 默认值不一致。当 JSON 无 `"range"` 字段时，`from_json`/`ParseLight` 不赋值 → LightDesc 保持 0.0f → SceneConstructor 无条件赋值 `lightComp.range = 0.0f` → 覆盖了 LightComponent 的默认 10.0f。

修复：改为 `std::optional<float>`，`value_or(lightComp.range)` 保留原默认值。

## 修复

四端同步补全（详见 `.atomcode.md` 规则 23）：

| 端 | 文件 | 改动 |
|:---|:-----|:------|
| **Desc 结构体** | `SceneDescription.h` `LightDesc` | `falloffStart/falloffEnd/spotPower/shadowBias` → `std::optional` |
| **Desc 序列化** | `SceneDescription.h` `to_json`/`from_json` | 四个字段的读写 |
| **加载解析** | `SceneLoader.cpp` `ParseLight` | 四个字段的 `j.contains()` → 赋值 |
| **ECS → Desc** | `EditorSceneManager.cpp` `ExportToDescription` | `lDesc.* = lightComp->*` |
| **Desc → ECS** | `SceneConstructor.cpp` `ConstructEntity` | `lightComp.* = ld.*.value_or(...)` |
| **快照捕获** | `EditorSceneManager.cpp` `OnSceneConstructReady` | `ld.* = light->*` |
| **快照恢复** | `EditorSceneManager.cpp` `ApplyTabState` | `light.* = ld.*.value_or(...)` |

## 教训

ECS 组件、序列化 Desc、属性卡编辑器、SceneLoader::Parse* 加载器**四端必须显式同步**——任何一端增加了字段，其余所有端必须同步更新。`SceneLoader::Parse*` 函数与 `from_json` 重复了相同的解析逻辑，是容易被遗漏的第四端。
