# 资产加载器改进方案

> 当前 AssetManager 使用 `switch(type)` 分发加载请求。
> 改进目标：注册式分发，以文件后缀匹配处理器，消除中心化 switch。

---

## 现状

`AssetManager::Load()` 中的 `switch (type)`：

```cpp
switch (type) {
case AssetType::Mesh:     /* MeshLoadTask */
case AssetType::Texture:  /* TextureLoadTask */
case AssetType::Material: /* MaterialLoadTask */
case AssetType::Terrain:  /* return false */
case AssetType::Scene:    /* return false */
}
```

### 问题

1. **新增类型必须改 switch** — 破开放闭原则
2. **`AssetType` 枚举与后缀耦合** — 调用方需知道类型枚举值
3. **`.dxmesh` / `.dds` / `.json` 天然就是类型标识** — 后缀已足够

---

## 改进方案：Loader 注册表

### 设计

```cpp
class AssetManager {
    // 注册一个后缀对应的加载器
    void RegisterLoader(std::string extension, LoaderFunc func);

    // 对外接口：只传路径，不传类型
    uint32_t Load(const std::string &path, AssetCallback onComplete, uint8_t priority = 1);
};
```

`LoaderFunc` 签名：
```cpp
using LoaderFunc = std::function<LoadTask(
    const std::string &path,
    AssetCallback onComplete,
    LoadContext ctx  // device, cmdMgr, managers...
)>;
```

### 注册示例

```cpp
// Bootstrap 或各模块初始化时注册
AssetManager::GetInstance().RegisterLoader(".dxmesh", CreateMeshLoadTask);
AssetManager::GetInstance().RegisterLoader(".dds",    CreateTextureLoadTask);
AssetManager::GetInstance().RegisterLoader(".json",   CreateSceneLoadTask);
AssetManager::GetInstance().RegisterLoader(".mat",    CreateMaterialLoadTask);
```

### Load 流程

```cpp
uint32_t AssetManager::Load(const std::string &path, AssetCallback onComplete, uint8_t priority) {
    // 1. 查缓存
    if (TryCache(path, onComplete)) return id;

    // 2. 按后缀找加载器
    auto ext = std::filesystem::path(path).extension().string();
    auto it = m_loaders.find(ext);
    if (it == m_loaders.end()) {
        // 无匹配加载器
        if (onComplete) onComplete({false, path});
        return id;
    }

    // 3. 调用加载器创建 LoadTask
    auto task = it->second(path, onComplete, m_ctx);
    m_executor->SubmitLoadTask(std::move(task));
    return id;
}
```

### Batch 加载

```cpp
// 批量加载也不需要传类型了——全由后缀决定
AssetManager::GetInstance().LoadBatch({
    "Content/Models/cube.dxmesh",
    "Content/Textures/stone.dds",
    "Content/Materials/brick.mat",
    "Content/Scenes/level1.json",
}, callback, onAllComplete);
```

---

## 对比

| 维度 | 当前 switch 模式 | 注册表模式 |
|:-----|:----------------|:-----------|
| 新增类型 | 改 AssetManager.cpp | 加一行 `RegisterLoader` |
| 调用方 | 需知道 `AssetType::Mesh` | 只传路径 |
| 第三方扩展 | 不可能 | 可（引擎外注册） |
| 缓存命中 | 按路径 | 按路径（相同） |
| 实现复杂度 | 低 | 中 |

## 注意事项

1. **重复后缀**：后注册的覆盖先注册的（允许模块覆盖默认加载器）
2. **缓存 key**：仍使用全路径，不依赖后缀
3. **`onComplete` 包装**：每个加载器负责在自己的 `onComplete` 中将结果写入 `AssetManager::m_cache`
4. **复合资源**：某些格式（`.gltf`、`.fbx`）可能产出多类型资源，此时需特殊 `LoaderFunc` 在内部自行拆分

## 迁移路线

| 步骤 | 内容 |
|:-----|:------|
| 1 | 定义 `LoaderFunc` + `LoadContext` 类型 |
| 2 | 实现 `RegisterLoader` + `Load(path)` 新入口 |
| 3 | 将现有 Mesh/Texture/Material 的 switch 分支拆为独立函数 |
| 4 | 在 Bootstrap 中注册三个加载器 |
| 5 | 保留旧 `Load(path, type, ...)` 接口过渡，逐步迁移调用方 |
| 6 | 移除 `AssetType` 枚举 |
