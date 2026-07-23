# 场景快照（SceneSnapshot）架构

> 日期：2026-07-22
> 状态：📋 新设计
> 关联：`SceneManager.md`、`EditorSceneManager`、`EditorStateFile`（即将废弃）

---

## 1. 核心概念分离

### 1.1 三个角色

```
┌─────────────────────────────────────────────────────────────────────┐
│                         EditorSceneManager                          │
│  角色：管理者（Manager）                                               │
│  职责：ECS 实体管理、Tab 生命周期、场景构造编排、渲染管线驱动、ApplySnapshot │
│  不包含：序列化、快照存储                                              │
└──────────────────────────┬──────────────────────────────────────────┘
                           │ 持有 vector<SceneSnapshot>
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│                           SceneSnapshot                              │
│  角色：快照（Snapshot）                                                │
│  职责：per-tab 完整状态聚合，可序列化/反序列化，Tab 切换时整体恢复      │
│  包含：场景内容（skybox/environment/entity） + 编辑器UX状态（camera/hierarchy/selection） │
│  不包含：管理逻辑                                                     │
└──────────────────────────┬──────────────────────────────────────────┘
                           │ SaveTo / LoadFrom
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      SnapshotSerializer                              │
│  角色：持久化层（Serializer）                                          │
│  职责：读写磁盘 JSON，管理缓存文件生命周期                              │
│  取代：EditorStateFile（旧，仅 UX 状态）                               │
│  不包含：任何场景逻辑                                                  │
└─────────────────────────────────────────────────────────────────────┘
```

### 1.2 角色对比

| 角色 | 类名 | 数据持有 | 序列化 | 示例方法 |
|:-----|:-----|:---------|:-------|:---------|
| **管理者** | `EditorSceneManager` | `vector<SceneSnapshot>` | ❌ 委托给 Snapshot | `SwitchScene()`, `ApplySnapshot()`, `CreateEntity()` |
| **快照** | `SceneSnapshot` | 自身字段 | ✅ `SaveTo()` / `LoadFrom()` | `HasSkybox()`, `IsValid()` |
| **持久化层** | `SnapshotSerializer` | 磁盘文件 | ✅ 文件 I/O | `SaveSnapshot()`, `LoadSnapshot()`, `ListCachedScenes()` |

---

## 2. 问题背景

### 2.1 当前状态管理分散

编辑器多 Tab 模式下，每个场景的运行时状态分散在多个位置：

```
EditorStateFile（磁盘缓存）          ← 编辑器 UX 状态
  ├─ CameraState (position + forward)
  ├─ HierarchyState (parent 映射)
  └─ SelectionState (persistentId)

EditorSceneManager（内存 per-tab 缓存） ← 运行时数据
  ├─ m_tabEntities[i]      (vector<uint64_t>)
  ├─ m_tabEntityDescs[i]   (vector<EntityDesc>)
  ├─ m_tabGeoMaps[i]       (map<string, GeometryHandle>)
  ├─ m_tabMatMaps[i]       (map<string, MaterialHandle>)
  └─ (缺失) SkyboxDesc / EnvironmentDesc

全局单例管理器（仅活跃场景）          ← 被覆盖就丢失
  ├─ SceneManager::m_skybox
  ├─ SceneManager::m_environment
  ├─ SkyboxManager（单例）
  ├─ LightManager（单例）
  └─ WaterManager（单例）
```

### 2.2 问题清单

| 问题 | 表现 | 后果 |
|:-----|:------|:------|
| **状态散落** | 新增 per-tab 数据需加一个 `vector<T>` 成员 | 横向膨胀，易遗漏（当前天空盒已遗漏） |
| **切换不原子** | `ProcessPendingTabSwitch` 需手动同步多个 vector | 漏掉某个状态就产生 Bug |
| **序列化分散** | UX 状态在 EditorStateFile，场景数据在 EntityDesc | 非持久化/可持久化边界模糊 |
| **角色混叠** | EditorSceneManager 既管场景又管序列化 | 违反单一职责 |

---

## 3. SceneSnapshot — 场景快照

### 3.1 结构体定义

```cpp
/// 单场景的完整快照
///
/// 设计原则：
///   - 可序列化字段：可写入磁盘缓存，跨会话恢复
///   - 运行时字段：仅存于内存，跨会话需重新加载
///   - 快照 = 场景内容（skybox/environment/entity）
///           + 编辑器 UX 状态（camera/hierarchy/selection）
///           + 运行时 GPU 句柄（geoMap/matMap）
struct SceneSnapshot {
    // ====================================================================
    // 可序列化字段（SnapshotSerializer 读写）
    // ====================================================================

    /// 场景环境数据
    Resource::SkyboxDesc skybox;
    Resource::EnvironmentDesc environment;

    /// 实体描述列表（用于导出 JSON 和 Tab 切换时重建）
    std::vector<Resource::EntityDesc> entityDescs;

    /// 相机状态（位置 + 朝向）
    struct CameraState {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 forward;
    };
    CameraState camera;

    /// 层级展开状态（parent 映射）
    std::vector<HierarchyEntry> hierarchy;

    /// 选中实体列表（persistentId）
    std::vector<uint64_t> selection;

    // ====================================================================
    // 运行时字段（仅内存，不可序列化）
    // ====================================================================

    /// GPU 几何体句柄映射
    std::unordered_map<std::string, Resource::GeometryHandle> geoMap;

    /// GPU 材质句柄映射
    std::unordered_map<std::string, Resource::MaterialHandle> matMap;

    /// 天空盒 GPU 资源句柄（跨 Tab 切换重建天空盒用）
    Resource::GpuResourceHandle skyboxTextureHandle;
    Resource::GeometryHandle skyboxGeometryHandle;

    /// ECS 实体 Handle 列表（与 entityDescs 一一对应）
    std::vector<uint64_t> entities;

    // ====================================================================
    // 序列化能力
    // ====================================================================

    /// 写入磁盘缓存（仅序列化可序列化字段）
    bool SaveTo(const std::filesystem::path& path) const;

    /// 从磁盘缓存读取（仅恢复可序列化字段）
    bool LoadFrom(const std::filesystem::path& path);

    // ====================================================================
    // 辅助方法
    // ====================================================================

    bool HasSkybox() const { return !skybox.texture.empty(); }
    bool HasEnvironment() const { return !environment.ambientLight.empty(); }
    bool IsValid() const { return !entityDescs.empty() || HasSkybox(); }
};
```

### 3.2 与旧结构的映射

```
旧（6 个 vector + EditorStateFile）       新（SceneSnapshot）
─────────────────────────────────       ─────────────────────
m_tabEntities[i]              ───────→  entities
m_tabEntityDescs[i]           ───────→  entityDescs
m_tabGeoMaps[i]               ───────→  geoMap
m_tabMatMaps[i]               ───────→  matMap
（缺失）                       ───────→  skybox
（缺失）                       ───────→  environment
（缺失）                       ───────→  skyboxTextureHandle
（缺失）                       ───────→  skyboxGeometryHandle
EditorStateFile::CameraState  ───────→  camera
EditorStateFile::Hierarchy    ───────→  hierarchy
EditorStateFile::Selection    ───────→  selection
```

---

## 4. EditorSceneManager — 管理者角色

### 4.1 职责边界

```
EditorSceneManager 的职责：
  ✅ 管理 ECS 实体（CreateEntity / RegisterEntity / RemoveAllEntities）
  ✅ 管理 Tab 生命周期（SwitchScene / CloseTab / ProcessPendingTabSwitch）
  ✅ 编排场景构造（RegisterSceneConstructSystem → OnSceneConstructReady）
  ✅ 驱动渲染管线（通过 ApplySnapshot 恢复全局管理器状态）
  ✅ 应用快照（ApplySnapshot → Clear + Rebuild）

EditorSceneManager 不包含：
  ❌ 序列化逻辑（委托给 SceneSnapshot::SaveTo / LoadFrom）
  ❌ 磁盘文件管理（委托给 SnapshotSerializer）
  ❌ 编辑器 UX 状态的直接持有（通过 SceneSnapshot 间接持有）
```

### 4.2 核心接口

```cpp
class EditorSceneManager {
public:
    // ===== 管理者接口 =====
    uint64_t CreateEntity();
    void RegisterEntity(uint64_t entity);
    void RemoveAllEntities();

    void SwitchScene(const std::string& name, const std::filesystem::path& path);
    void CloseTab(size_t index);
    void ProcessPendingTabSwitch();

    void RegisterSceneConstructSystem();
    void ApplySnapshot(size_t index);  // Clear + Rebuild 全局管理器

    // ===== 快照访问 =====
    const SceneSnapshot& GetSnapshot(size_t index) const;
    SceneSnapshot& GetMutableSnapshot(size_t index);

    // ===== 查询 =====
    uint64_t GetActiveSceneId() const;
    const std::vector<uint64_t>& GetActiveEntities() const;

private:
    // 持有快照，不持有序列化逻辑
    std::vector<SceneTab> m_openTabs;
    std::vector<SceneSnapshot> m_snapshots;
};
```

### 4.3 与 SceneManager（引擎 CORE）的关系

回顾 `Docs/architecture/SceneManager.md §5`，EditorSceneManager 的核心职责是 **ECS 实体管理**：

| 职责 | 说明 |
|:-----|:------|
| **RegisterSceneConstructSystem** | 响应 GeneratorTaskCompleteEvent，构造 ECS 实体 |
| **EntityDesc 缓存** | 维护 `m_entityDescs` 映射，供 Outliner 编辑和 JSON 导出 |
| **场景文件管理** | NewScene / SaveScene / SaveSceneAs |
| **多 Tab 管理** | Tab 创建/切换/关闭 |
| **ECS 实体入口** | CreateEntity / RegisterEntity（包装 SceneManager） |

**新加入的职责**：`ApplySnapshot` — 在 Tab 切换时将快照中的环境数据恢复到全局管理器。

---

## 5. SnapshotSerializer — 持久化层

### 5.1 职责

```
SnapshotSerializer 取代 EditorStateFile：
  ┌─ EditorStateFile（旧）              SnapshotSerializer（新）
  │  仅管理 UX 状态（camera/hierarchy/selection）   管理完整快照的持久化
  │  File-per-scene 模式                File-per-scene 模式（不变）
  │  无场景内容感知                     感知 SceneSnapshot 结构
  └─ 硬编码的 SaveCameraState 等        泛化的 SaveSnapshot / LoadSnapshot
```

### 5.2 接口设计

```cpp
/// 快照持久化层
///
/// 职责：
///   - 将 SceneSnapshot 的可序列化字段写入磁盘 JSON
///   - 从磁盘 JSON 恢复 SceneSnapshot 的可序列化字段
///   - 管理缓存文件生命周期（目录结构、清理）
///
/// 不包含：
///   - 任何场景管理逻辑
///   - 运行时 GPU 句柄的恢复
class SnapshotSerializer {
public:
    void Initialize(const std::filesystem::path& cacheRoot);

    /// 保存快照到磁盘（仅序列化可序列化字段）
    bool SaveSnapshot(const std::filesystem::path& scenePath,
                      const SceneSnapshot& snapshot);

    /// 从磁盘加载快照（仅恢复可序列化字段，GPU 句柄需重新加载）
    bool LoadSnapshot(const std::filesystem::path& scenePath,
                      SceneSnapshot& snapshot) const;

    /// 检查是否有缓存
    bool HasCache(const std::filesystem::path& scenePath) const;

    /// 删除缓存
    bool RemoveCache(const std::filesystem::path& scenePath);

private:
    std::filesystem::path m_cacheRoot;
    // Content/Cache/Editor/xxx.snapshot.json
};
```

### 5.3 与 EditorStateFile 的兼容

```
旧 EditorStateFile 文件：
  Content/Cache/Editor/forest.scene.json

新 SnapshotSerializer 文件：
  Content/Cache/Editor/forest.snapshot.json

迁移：旧文件可被 SnapshotSerializer 读取兼容，
      新文件写入 .snapshot.json 后缀。
```

---

## 6. 数据流

### 6.1 场景加载完成（OnSceneConstructReady）

```
OnSceneConstructReady(sceneData)
  │
  ├─ SceneSnapshot &snap = m_snapshots[m_activeTabIndex]
  │
  ├─ snap.skybox = sceneData.skybox          ← 场景内容
  ├─ snap.environment = sceneData.environment
  ├─ snap.entityDescs = sceneData.entities
  ├─ snap.geoMap = sceneData.geoMap          ← 运行时句柄
  ├─ snap.matMap = sceneData.matMap
  ├─ snap.skyboxTextureHandle = sceneData.skyboxTextureHandle
  ├─ snap.skyboxGeometryHandle = sceneData.skyboxGeometryHandle
  │
  ├─ 创建实体，添加到 snap.entities
  │
  └─ ApplySnapshot(m_activeTabIndex)         ← 恢复全局管理器
        └─ 管理器 Clear + Rebuild 逻辑
```

### 6.2 Tab 切换（ProcessPendingTabSwitch）

```
ProcessPendingTabSwitch()
  │
  ├─ 1. 保存当前 Tab 的 UX 状态到 snap
  │     ├─ snap.camera = current camera
  │     ├─ snap.hierarchy = current hierarchy
  │     └─ snap.selection = current selection
  │
  ├─ 2. 更新 m_activeTabIndex
  │
  ├─ 3. ApplySnapshot(m_activeTabIndex)      ← Clear + Rebuild
  │
  └─ 4. 从 snap 恢复 UX 状态
        ├─ camera ← snap.camera
        ├─ hierarchy ← snap.hierarchy
        └─ selection ← snap.selection
```

### 6.3 关闭 Tab（CloseTab）

```
CloseTab(index)
  │
  ├─ 1. 保存快照到磁盘
  │     └─ m_snapshots[index].SaveTo(cachePath)
  │
  ├─ 2. 从 m_snapshots 移除
  │
  └─ 3. 如果是最后一个 Tab
        └─ 清除所有实体 + Clear 管理器
```

### 6.4 编辑器启动恢复

```
Editor::Initialize()
  │
  ├─ ... 初始化各管理器 ...
  │
  └─ 从 SnapshotSerializer 恢复上次会话
        ├─ 遍历缓存文件列表
        ├─ 为每个缓存创建 Tab + SceneSnapshot
        └─ 激活第一个 Tab 的 ApplySnapshot
```

---

## 7. 长期愿景：缓存层（Cache Layer）

### 7.1 架构定位

```
┌──────────────────────────────────────────────────────────────┐
│                        Editor                                 │
│  ┌────────────────────────────────────────────────────────┐  │
│  │                    Cache Layer                          │  │
│  │                                                        │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐ │  │
│  │  │ SceneSnapshot│  │  AnimState   │  │  AudioState  │ │  │
│  │  │  (场景模块)    │  │  (动画模块)   │  │  (音频模块)   │ │  │
│  │  └──────┬───────┘  └──────────────┘  └──────────────┘ │  │
│  │         │                                               │  │
│  │         ▼                                               │  │
│  │  ┌──────────────┐                                       │  │
│  │  │Snapshot      │  ← JSON / Binary 序列化               │  │
│  │  │Serializer    │                                       │  │
│  │  └──────┬───────┘                                       │  │
│  └─────────┼────────────────────────────────────────────────┘  │
│            ▼                                                   │
│     Content/Cache/Editor/xxx.snapshot.json                     │
└────────────────────────────────────────────────────────────────┘
```

### 7.2 设计原则

| 原则 | 说明 |
|:-----|:------|
| **模块自治** | 每个模块（Scene、Animation、Audio、Physics...）独立定义自己的 Snapshot 结构体，互不依赖 |
| **统一容器** | EditorSceneManager 持有 `vector<SceneSnapshot>`，Tab 切换时调用 `ApplySnapshot` 整体恢复 |
| **序列化能力** | 每个 Snapshot 可选择实现 `SaveTo` / `LoadFrom`，由 SnapshotSerializer 统一调度 |
| **运行时隔离** | 可序列化字段与运行时字段（GPU 句柄、指针）显式分离 |
| **增量演进** | 优先实现 SceneSnapshot，后续模块按需接入，不阻塞现有功能 |

### 7.3 远期扩展示例

```cpp
// 未来 Editor 中：
struct PerTabSnapshot {
    SceneSnapshot scene;
    AnimSnapshot anim;
    PhysicsSnapshot physics;
    AudioSnapshot audio;
    // ...
};

// EditorSceneManager 持有：
std::vector<PerTabSnapshot> m_snapshots;
```

每个模块独立演进，新增模块只需加一个字段，不影响现有逻辑。

---

## 8. 迁移路径

| 阶段 | 步骤 | 内容 |
|:----:|:----:|:------|
| **P0** | 1 | 定义 `SceneSnapshot` 结构体（含序列化能力） |
| | 2 | EditorSceneManager 用 `vector<SceneSnapshot>` 替换旧成员 |
| | 3 | 实现 `ApplySnapshot`（Clear + Rebuild） |
| | 4 | 串联到 SwitchScene / ProcessPendingTabSwitch / CloseTab |
| **P1** | 5 | 实现 `SnapshotSerializer`，取代 `EditorStateFile` |
| | 6 | 将 camera/hierarchy/selection 归入 SceneSnapshot |
| | 7 | 编辑器启动时恢复上次会话的快照 |
| **P2** | 8 | 按需扩展 AnimSnapshot / PhysicsSnapshot 等 |

---

## 10. 当前行为与已知缺陷

### 10.1 Tab 切换交互

```
Tab 切换仅支持用户手动点击：
  ┌─ 用户点击 Tab → ImGui 选中 → isVisible=true → m_pendingSwitchTab = i
  │     └─ ProcessPendingTabSwitch 在帧结束后执行
  │           ├─ SaveCurrentSnapshotToDisk()
  │           ├─ 更新 m_activeTabIndex
  │           ├─ ApplyTabState()     ← Clear + Rebuild 全局管理器
  │           ├─ RestoreSnapshotCamera()
  │           └─ PostEvent(TabSwitchedEvent)

  └─ 双击场景文件（AssetBrowser）
        ├─ 文件未打开 → 累加新 Tab，不自动切换（首个 Tab 除外）
        └─ 文件已打开 → 忽略，不重复加载，不切换 Tab
```

### 10.2 已知缺陷

| # | 缺陷 | 影响 | 原因 |
|:-:|:-----|:-----|:------|
| 1 | **程序化 Tab 切换频闪** | 双击场景文件时自动切换 Tab 会导致一帧闪烁 | ImGui 的 `SetSelected` 仅生效一帧，下一帧被 ImGui 内部状态覆盖。`m_activeTabIndex` 与 ImGui 选中状态不同步 |
| 2 | **`OnFileDoubleClick` 重复加载** | 即使 Tab 已存在，`SwitchScene` 提前返回后 `OnFileDoubleClick` 仍会调用 `LoadScene` 启动异步加载，浪费资源 | `OnFileDoubleClick` 无法访问 `EditorSceneManager` 的 Tab 列表，无法判断 Tab 是否已存在 |
| 3 | **`SnapshotSerializer` 未实现** | `EditorStateFile` 已删除，但替代的 `SnapshotSerializer` 尚未实现 | 当前快照通过 `SceneSnapshot::SaveTo/LoadFrom` 直接读写磁盘，缺少统一的持久化层 |
| 4 | **编辑器启动恢复** | 重启后不恢复上次会话的快照 | 需要 `SnapshotSerializer` 实现后统一处理 |

### 10.3 后续优化方向

```
P1:
  ┌─ 实现 SnapshotSerializer 取代 EditorStateFile
  ├─ 编辑器启动时恢复上次会话的快照
  └─ OnFileDoubleClick 跳过已存在 Tab 的异步加载

P2:
  └─ 寻找 ImGui 标准程序化 Tab 切换方案（SetTabItemSelected 或自定义 TabBar）
```

```
场景文件 (.scene.json)          SceneSnapshot（运行时）          SnapshotSerializer（磁盘）
       │                              │                               │
  SceneLoader                    OnSceneConstructReady              CloseTab / 定时保存
  JSON → SceneDescription        sceneData → SceneSnapshot          SceneSnapshot → JSON
       │                              │                               │
       └──────────┬───────────────────┴───────────────┬───────────────┘
                  │                                   │
            只读数据（场景内容）                   读写数据（快照缓存）
            SkyboxDesc                             CameraState
            EnvironmentDesc                        HierarchyState
            EntityDesc                             SelectionState
            GeoHandle/MatHandle                    SkyboxDesc（缓存）
```