# 资产加载器改进方案（定案）

> 状态：📋 设计定案（2026-08-02，依据 remaining_issues.md #8 推进）
> 关联：`Docs/todos/archived/remaining_issues.md` #8、`Docs/architecture/assets/AssetArchitecture.md`、`Docs/snapshots/AnimationSystem_Snapshot_20260801.md` 阶段 3（Character 聚合独立化）
> 结论：采用 **Godot ResourceFormatLoader 注册表模式**：`LoaderFunc` 按文件后缀注册，类型由 Loader 声明，`Load(path)` 只传路径不传类型。

---

## 一、现状问题

`AssetManager::Load()` 中的 `switch (type)`（约 265 行，5 个真实分支）：

```cpp
switch (type) {
case AssetType::Mesh:     /* MeshLoadTask */
case AssetType::Texture:  /* TextureLoadTask */
case AssetType::Material: /* MaterialLoadTask */
case AssetType::Skeleton: /* SkeletonLoadTask */
case AssetType::Character:/* 复合资产聚合：骨架同步 + 依赖计数 */
}
```

### 问题

1. **新增类型必须改 switch** — 破开放闭原则
2. **`AssetType` 枚举与后缀耦合** — 调用方需知道类型枚举值
3. **`.dxmesh` / `.dds` / `.bone` / `.character` 天然就是类型标识** — 后缀已足够
4. **Character 复合资产聚合逻辑（骨架同步 + lambda/原子计数）堆在 switch 里** — 90 行逻辑不可测、错误处理缺失（快照阶段 3）

---

## 二、大型引擎参考（2026-08-02 调研）

### Godot（与本项目场景最接近）

核心是 **ResourceFormatLoader 注册表**（`ResourceLoader::add_resource_format_loader`）：

| 机制 | 做法 |
|:-----|:-----|
| 注册 | 每个格式一个 Loader 类，实现 `get_recognized_extensions()` 声明支持的后缀，`add_resource_format_loader()` 全局注册 |
| 匹配 | `recognize_path()` 用**路径结尾比较**（`p_path.right(ext.length())`，大小写不敏感），**不是** `extension()` 取最后一级 |
| 分发 | `load(path, type_hint)` 顺序遍历所有 Loader，第一个匹配后缀的尝试加载，失败则继续下一个 |
| 类型 | `type_hint` 可选；Loader 通过 `get_resource_type()` / `handles_type()` 声明自己产出什么类型——**类型由 Loader 提供，不是调用方传入** |
| 缓存 | 同路径二次加载直接返回缓存（`has_cached`） |

### Unreal（编辑器导入侧）

**UFactory 自动扫描**：`Formats.Add("ext;Description")` 声明扩展名 + `SupportedClass` 声明产物类，引擎启动时自动扫描所有 UFactory 子类完成注册，导入时按扩展名路由。

### 对项目的启示

1. 注册表按后缀分发是标准做法——方向正确。
2. **类型由 Loader 声明**：注册表条目自带 `AssetType`，`InferType(path)` 从路径派生类型，调用方不用再传。
3. **后缀匹配用"路径结尾比较"而非 `extension()`**——这是处理 `.json` 双后缀的关键。

---

## 三、定案设计（2026-08-02）

### 3.1 核心接口

```cpp
// AssetManager.h 新增
using LoaderFunc = std::function<uint32_t(const std::string &path, AssetCallback onComplete, uint8_t priority)>;

struct LoaderEntry {
    LoaderFunc func;   // 创建并提交 LoadTask 的闭包（捕获 managers/deviceContext）
    AssetType type;    // 该后缀产出的类型（供 InferType + AssetResult.type）
};

// 注册：ext 统一小写带点（".dxmesh" / ".bone" / ".scene"）
void RegisterLoader(std::string ext, AssetType type, LoaderFunc func);
// 推断：后缀匹配（剥 .json 再试）→ AssetType，找不到返回 AssetType::Prefab（哨兵）
AssetType InferType(const std::string &path) const;

// 新入口（只传路径，不传 type）——旧 Load(path, type) 签名移除，无过渡期
uint32_t Load(const std::string &path, AssetCallback onComplete, uint8_t priority = 1);
```

### 3.2 后缀匹配算法（处理 `.json` 双后缀）

`std::filesystem::path("kd-03.scene.json").extension()` 返回 `".json"`，直接匹配会漏掉语义后缀 `.scene`。定案采用**剥 json 再匹配**（B 方案）：

```
ext = lowercase(path.extension())
if m_loaders 含 ext → 命中
else if ext == ".json" → 对 path.stem() 再取 extension() 查表  ← 自动覆盖 .scene.json / .hod.json
else 未命中 → 回调失败
```

### 3.3 类型与后缀映射（Initialize 注册）

| 后缀 | AssetType | Loader |
|:-----|:----------|:-------|
| `.dxmesh` / `.obj` | Mesh | MeshLoader（MeshLoadTask） |
| `.dds` / `.png` / `.jpg` / `.jpeg` / `.bmp` / `.tga` | Texture | TextureLoader（TextureLoadTask） |
| `.mat` | Material | MaterialLoader（MaterialLoadTask） |
| `.bone` | Skeleton | SkeletonLoader（SkeletonLoadTask） |
| `.character` | Character | CharacterLoader（依赖聚合，独立函数） |
| `.scene` | Scene | 场景加载由 SceneLoader/SceneManager 负责，AssetManager 不注册（保持现状返回失败） |

### 3.4 anim 特殊性（保留 LoadAnimation 专用入口）

**`.anim` 不注册到注册表**。原因：

- `.anim` 通道按**骨骼名匹配**（`AnimLoader` 一次性 hash 建表），加载必须携带骨架 `BoneNames` 上下文
- **anim 与 bone 是紧密联系的资产**：`.bone` 定义骨骼树，`.anim` 定义骨骼通道，二者以骨骼名耦合，无法脱离骨架独立加载
- 因此保持专用入口：`LoadAnimation(path, boneNames, onComplete, priority)`
- `.character` 复合资产内部对 `.anim` 的依赖走 `LoadAnimation`（先同步加载骨架 → 取 BoneNames → 再异步加载剪辑）

### 3.5 Character 聚合逻辑拆出 switch（快照阶段 3 联动）

Character 分支（骨架同步 + 依赖计数聚合）从 `Load()` 的 switch 中**整体拆出**，独立为 `.character` Loader 函数。聚合逻辑仍在 Loader 内部（骨架同步 → 其余依赖异步计数 → 组装 CharacterData），但不再堆在 `Load()` 主函数里。错误处理（依赖失败 → 立即失败回调 + 释放已加载 handle）在本次一并补齐。

### 3.6 AssetType 枚举保留

`AssetType` **不移除**：`AssetResult.type` 字段、`FileIconProvider` 扩展名映射、`SceneConstructor` 依赖收集分组都依赖它。注册表只是把"分发依据"从枚举换成后缀，枚举降级为 Loader 声明的结果类型标识 + 外部查询用。

### 3.7 按资产类型拆文件（2026-08-02 追加，参照属性卡注册制）

注册表模式具备**声明式拆分**能力（与 `ComponentEditorRegistry::Register<T>(drawFn)` 同构）：每个资产类型一个 Loader 文件，各自提供 `Register*Loader()` 自注册，`AssetManager` 主体不再包含任何具体资产的加载逻辑。

```
Engine/Resource/AssetManager/
  ├─ AssetManager.h/.cpp            ← 注册表核心：RegisterLoader/FindLoader/InferType/Load/LoadBatch/LoadAnimation/缓存
  └─ Loaders/                        ← 按资产类型拆文件
       ├─ MeshLoader.cpp             RegisterMeshLoader()      → .dxmesh/.obj
       ├─ TextureLoader.cpp          RegisterTextureLoader()   → .dds/.png/.jpg/.jpeg/.bmp/.tga
       ├─ MaterialLoader.cpp         RegisterMaterialLoader()  → .mat
       ├─ SkeletonLoader.cpp         RegisterSkeletonLoader()  → .bone
       └─ CharacterLoader.cpp        RegisterCharacterLoader() → .character（复合聚合）
```

- **注册时机灵活**：`AssetManager::Initialize()` 末尾统一调用完成默认注册；编辑器/Game 端也可在**使用 AssetManager 之前**自行调用 `Register*Loader()` 覆盖默认（后注册覆盖先注册）。只需满足"注册早于 `Load()`"。
- **新增资产类型** = 新增 `Loaders/xxx.cpp` + 一行 `Register*Loader` 声明 + 一行 Initialize 调用，**不改 AssetManager 主体**（开放封闭，与新增 ECS 组件 = 新增 Editor 文件同模式）。
- 与属性卡注册制的唯一差异：Loader 需要捕获运行时 managers（`m_geoMgr` 等），走"Initialize 统一调用注册函数"的运行时注册（同 `Bootstrap::Register`/`SystemRegistry::Register`），不做静态初始化注册。
- `AssetManager.cpp` 从 491 行瘦身至 ~223 行；各 Loader 可独立演进、可测试；Game 端/编辑器端可按需选择性注册（如 Game 不注册 `.character`）。

---

## 四、迁移范围（2026-08-02 定案）

### 4.1 调用方改动（无过渡接口，一次性迁移）

| 调用方 | 现状 | 迁移后 |
|:-------|:-----|:-------|
| `SceneConstructor.cpp:113` | `LoadBatch(pair<string,AssetType>)` | `LoadBatch(vector<string>)`（新重载，按后缀推断类型） |
| `AssetBrowser.cpp`（3 处） | `Load(path, AssetType::Mesh/Texture/Material)` | `Load(path)` 不传类型 |
| `AnimationViewportPanel.cpp:619` | `Load(path, AssetType::Mesh)` | `Load(path)` 不传类型 |

### 4.2 Game 端（暂时搁置，仅记录变更点）

**现状**：Game 目录（29 个文件）**无任何直接 AssetManager 调用**——`GameWorld` 通过 `SceneConstructor`（引擎侧）间接加载场景依赖，动画/角色功能尚未接入 Game 端。

**后续 Game 端接入时需要做的变更**（记录，不在本次实施）：

1. `SceneConstructor` 已被迁移为新 `LoadBatch(vector<string>)` 接口——Game 端若直接构造资产列表，用**后缀**而非 `AssetType` 枚举声明类型
2. 场景依赖收集（`desc.dependencies`）中的类型信息改由扩展名推断，不再依赖 `AssetType` 参数
3. 动画资产接入：Game 端必须先加载 `.bone`（`Load(path)`）拿到 `SkeletonHandle` 及 `BoneNames`，再调 `LoadAnimation(path, boneNames, ...)` 加载 `.anim`；`.anim` 无独立注册表入口
4. 角色场景化（NPC，P2）：`SceneConstructor` 需为 `.character` 依赖递归收集（骨架/网格/材质/剪辑），参考 `CharacterAsset.md` §九 阶段 C/D

### 4.3 不移除的内容

- `AssetType` 枚举（见 3.6）
- `.anim` 专用入口 `LoadAnimation`（见 3.4）
- `TryCache` / 缓存 key（仍为全路径）

---

## 五、实施步骤

| 步骤 | 内容 |
|:-----|:-----|
| 1 | `AssetManager.h`：定义 `LoaderFunc` + `LoaderEntry`，新增 `RegisterLoader` / `InferType` / 新 `Load(path)` / `LoadBatch(vector<string>)`，移除旧 `Load(path, type)` 签名 |
| 2 | `AssetManager.cpp`：将 Mesh/Texture/Material/Skeleton 分支拆为独立静态 Loader 函数；Character 聚合拆为独立函数；`Initialize()` 末尾注册全部默认 Loader；`Load()` 改为后缀查表分发 |
| 3 | `SceneConstructor.cpp`：`LoadBatch` 调用改为新重载（构造 `vector<string>`） |
| 4 | `AssetBrowser.cpp`：3 处 `Load` 调用去掉类型参数 |
| 5 | `AnimationViewportPanel.cpp`：`Load` 调用去掉类型参数 |
| 6 | 编译验证（人工） |

---

## 六、对比

| 维度 | 当前 switch 模式 | 注册表模式（定案） |
|:-----|:----------------|:-------------------|
| 新增类型 | 改 AssetManager.cpp | 加一行 `RegisterLoader` |
| 调用方 | 需知道 `AssetType::Mesh` | 只传路径（`Load(path)`） |
| 第三方扩展 | 不可能 | 可（引擎外注册） |
| 缓存命中 | 按路径 | 按路径（相同） |
| 类型来源 | 调用方传枚举 | Loader 声明 + `InferType(path)` |
| 双后缀（.scene.json） | 不支持 | 支持（剥 .json 再匹配） |
| 实现复杂度 | 低 | 中 |

## 七、注意事项

1. **重复后缀**：后注册的覆盖先注册的（允许模块覆盖默认加载器）
2. **缓存 key**：仍使用全路径，不依赖后缀
3. **`onComplete` 包装**：每个加载器负责在自己的 `onComplete` 中将结果写入 `AssetManager::m_cache`
4. **复合资源**：某些格式可能产出多类型资源（如 `.character`），由特殊 `LoaderFunc` 在内部自行拆分（复用 `Load` / `LoadAnimation`）
5. **`.anim` 特殊性**：不注册注册表，保留 `LoadAnimation(path, boneNames)` 专用入口（anim 与 bone 以骨骼名紧密耦合）
