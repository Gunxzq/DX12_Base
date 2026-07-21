# 资产体系架构

## 核心原则

资产分为两层：**原子资产**（Atomic Asset）和 **复合资产**（Composite Asset）。

```
原子资产（三元组）
  ├── Mesh     — 几何数据（顶点、索引、骨骼）
  ├── Material — 着色参数、纹理引用、渲染状态
  └── Texture  — 像素数据（2D、Cube、Array）

复合资产（JSON 构建）
  ├── Scene    — 场景描述（实体 + 组件 + 资产引用）
  ├── Terrain  — 地形（分块网格 + 高度图 + 材质层）
  ├── Prefab   — 预制体（实体模板）
  └── ...      — 后续扩展
```

## 当前代码中的枚举现状

| 文件 | 枚举 | 用途 | 问题 |
|------|------|------|------|
| `Engine/Asset/Definitions/AssetType.h` | `AssetType` | 资产加载分类 | ✅ 已独立 |
| `Engine/Resource/AssetManager/ResourceType.h` | `ResourceType` | 事件分发 | ❌ 与 `AssetType` 重复，只定义了 `Terrain` |
| `Engine/Resource/Core/GpuHandlePool.h` | `GpuResourceType` | GPU 资源类型 | 不同维度（Buffer/Texture2D 等） |
| `Engine/Resource/Geometry/` | Geometry 子类型 | `TriangleMesh`, `SkinnedMesh`, `PatchMesh` 等 | 几何系统内部类型 |

## 原子资产

### Mesh

几何数据，资产格式为 `.dxmesh`，也可通过 `.obj` 导入。

```cpp
// 几何系统内部子类型（已存在）
TriangleMesh  // 静态网格（顶点 + 索引，可选骨骼）
SkinnedMesh   // 骨骼动画网格（含 BoneInfluences）
PatchMesh     // 曲面细分面片（控制点）
```

**加载器**：`DxMeshLoader` / `ObjImporter`

**输出**：`GeometryHandle`（通过 `GeometryResourceManager` 管理）

### Material

渲染参数，包含着色器引用、纹理槽位、渲染状态。

**格式**：`.material`（JSON）

**输出**：`MaterialHandle`（通过 `MaterialManager` 管理）

### Texture

像素数据，支持多种格式。

**格式**：`.dds`（原生）, `.png` / `.jpg`（导入时转 DDS）

**输出**：`TextureHandle`（通过 `TextureManager` 管理）

## 复合资产

### Scene

场景描述文件，JSON 格式，引用原子资产。

```json
{
  "dependencies": {
    "meshes": { "body": "Content/robo/body.dxmesh", ... },
    "textures": { "albedo": "Content/robo/albedo.dds", ... },
    "materials": { "bodyMat": "Content/robo/body.material", ... }
  },
  "entities": [
    { "name": "robot", "mesh": "body", "material": "bodyMat", ... }
  ]
}
```

**加载器**：`SceneLoader`

### Terrain

地形是一种特殊资产，本质是网格但有特殊处理（分块、LOD、碰撞）。

```
Terrain
  ├── 高度图纹理 → Texture
  ├── 分块网格   → Mesh（程序化生成）
  ├── 材质层     → Material（多个）
  └── 碰撞数据   → 运行时构建
```

## 加载器架构

### 目标：按扩展名注册的模式

```
AssetManager::Load(path)
  ├── loader = GetLoaderByExtension(path)
  │     ├── ".dxmesh" / ".obj"  → MeshLoader
  │     ├── ".dds" / ".png"     → TextureLoader
  │     ├── ".material"         → MaterialLoader
  │     ├── ".scene"            → SceneLoader
  │     └── 未知                 → 回退 / 错误
  └── loader->Load(path, callback)
```

### 加载器接口

```cpp
class IAssetLoader {
public:
    virtual ~IAssetLoader() = default;
    virtual AssetType GetAssetType() const = 0;
    virtual std::vector<std::string> GetSupportedExtensions() const = 0;
    virtual void Load(const std::string &path, AssetCallback callback) = 0;
};
```

## 影响范围

| 系统 | 影响 |
|------|------|
| `AssetManager` | `Load()` 的 `switch(type)` → 改为 `GetLoaderByExtension(path)` |
| `ResourceType` | 与 `AssetType` 合并，或废弃 |
| `GeometryResourceManager` | 保持 `TriangleMesh` 等子类型，不影响 `AssetType` |
| `SceneConstructor` | 依赖收集时用扩展名推断类型，不再依赖 `AssetType` 参数 |
| `Editor` | 双击回调中检查扩展名白名单，而非 `AssetType` |
| `FileIconProvider` | 扩展名 → 颜色/图标，与加载器扩展名列表保持一致 |

## 迁移步骤

1. **统一枚举**：废弃 `ResourceType`，统一使用 `AssetType`
2. **加载器注册表**：`AssetManager` 维护 `map<string, IAssetLoader*>` 按扩展名路由
3. **保留 `AssetType`**：作为标识存在，但不再用于路由
4. **几何子类型**：`TriangleMesh` 等属于 `AssetType::Mesh` 的内部变体，不提升到 `AssetType`

## 待办

- [ ] 音频资产支持（`AssetType::Audio` + `AudioLoader`）
- [ ] 加载器注册表实现
- [ ] 废弃 `ResourceType` 枚举