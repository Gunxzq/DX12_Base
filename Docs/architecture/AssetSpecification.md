# 资产规范

## 一、核心概念

### 原子资产（三元组）

原子资产是运行时的最小可独立加载单元，每个原子资产对应一个运行时系统：

| 原子资产 | 文件格式 | 运行时系统 | 描述 |
|----------|---------|-----------|------|
| **Mesh** | `.dxmesh` | `GeometryResourceManager` | 顶点数据、索引数据、骨骼影响 |
| **Material** | `.material` | `MaterialManager` | 着色器参数、纹理引用、PSO 状态 |
| **Texture** | `.dds` / `.png` / `.jpg` | `TextureManager` + `GpuResourceManager` | 像素数据（2D、Cube、Array） |
| **Skeleton** | `.bone` | `SkeletonManager` | 骨骼树 + rest pose（HOD 解析导出） |
| **Animation** | `.anim` | `AnimationManager` | 骨骼动画剪辑（播放/调帧/循环） |

**原则**：原子资产不引用其他资产（Material 引用 Texture 的路径，但运行时通过 Handle 解耦；`.anim` 的通道骨骼名与 `.dxmesh` 的 `boneIndices` 一样只是命名/序号约定，不是资产引用）。

**三者的边界**：

```
.dxmesh  → "顶点受哪些骨骼影响"（boneIndices 序号约定）
.bone    → "骨骼树长什么样"（命名 + 层级 + rest pose）
.anim    → "骨骼怎么动"（通道名命名约定）
三者零引用，只共享命名/序号约定。详见 Docs/architecture/CharacterAsset.md
```

### 复合资产

复合资产通过 JSON 描述组合原子资产，定义运行时逻辑。复合资产不直接持有 GPU 数据，而是引用原子资产的 Handle。

| 复合资产 | 文件格式 | 运行时系统 | 描述 |
|----------|---------|-----------|------|
| **Character** | `.character` | `SceneConstructor` + 动画 System | 角色复合资产：骨架 + 网格 + 材质槽 + 动画剪辑打包（详见 `CharacterAsset.md`） |
| **Scene** | `.scene` | `SceneConstructor` + ECS | 实体层级、组件、资产引用 |
| **Terrain** | `.terrain` | `TerrainManager` | 高度图 → 程序化网格 + 材质层 |
| **ParticleSystem** | `.particle` | `ParticleManager` | 粒子发射器配置、引用纹理/材质 |
| **Prefab** | `.prefab` | `SceneConstructor` | 实体模板（实例化用） |

### 复合资产加载时序

所有复合资产遵循统一的加载时序（以粒子系统为例）：

```
1. AssetManager::Load("explosion.particle")
   └─ ParticleLoader 解析 JSON
        ├─ 识别依赖: "textures/spark.dds", "materials/particle.mat"
        ├─ 递归加载原子资产:
        │    ├─ Load("spark.dds")     → TextureHandle  ← 原子资产先就绪
        │    └─ Load("particle.mat")  → MaterialHandle  ← 原子资产先就绪
        └─ 依赖全部就绪后，创建复合资产 Handle:
             ├─ 持有 TextureHandle 引用
             ├─ 持有 MaterialHandle 引用
             └─ 持有发射器配置数据
```

**核心规则**：复合资产的依赖（原子资产）必须先于复合资产自身加载完成。复合资产自身不包含 GPU 数据，只持有对原子资产的 Handle 引用。

### 非资产数据

不是从文件加载的运行时数据：

| 数据 | 来源 | 所属系统 |
|------|------|---------|
| `ProceduralGeometry` | 运行时程序化生成 | `GeometryResourceManager` |
| `GridGeometry` | 编辑器辅助几何 | 编辑器 |
| `QuadGeometry` | 全屏四边形 | 渲染器 |
| `CapsuleGeometry` | 碰撞体 | 物理系统 |
| `LineSet` / `PointSet` | 调试可视化 / 粒子 | `GeometryResourceManager` |

## 二、三系统对应关系

```
磁盘                         运行时
┌─────────┐    DxMeshLoader    ┌──────────────────────┐
│ .dxmesh ├──────────────────► │ GeometryResource      │
│ Mesh     │   TriangleMesh     │  Manager              │
│         │   SkinCluster      │  ├─ TriangleMesh      │
│         │   PatchMesh        │  ├─ SkinnedMesh       │
└─────────┘                    │  ├─ PatchMesh         │
                               │  ├─ PointSet          │
┌─────────┐   MaterialLoader   │  └─ LineSet           │
│ .material├──────────────────► └──────────────────────┘
│ Material │                  ┌──────────────────────┐
│          │                  │ MaterialManager       │
└─────────┘                  │  ├─ MaterialHandle     │
                              │  └─ PSO 缓存          │
┌─────────┐   TextureLoader   └──────────────────────┘
│ .dds    ├──────────────────► ┌──────────────────────┐
│ Texture  │                  │ TextureManager        │
│ .png    │                  │ GpuResourceManager     │
│ .jpg    │                  │  ├─ TextureHandle      │
└─────────┘                  │  └─ SRV 描述符         │
                              └──────────────────────┘
┌─────────┐   SkeletonLoader  ┌──────────────────────┐
│ .bone   ├──────────────────► │ SkeletonManager      │
│ Skeleton│                   │  └─ SkeletonHandle    │
└─────────┘                   └──────────────────────┘
┌─────────┐   AnimLoader      ┌──────────────────────┐
│ .anim   ├──────────────────► │ AnimationManager     │
│ Animation│                  │  └─ ClipHandle        │
└─────────┘                   └──────────────────────┘

┌─────────┐   SceneLoader     ┌──────────────────────┐
│ .scene  ├──────────────────► │ SceneConstructor     │
│ (JSON)  │                   │  └─ ECS World        │
└─────────┘                   └──────────────────────┘
┌─────────┐  CharacterLoader  ┌──────────────────────┐
│ .character├─────────────────►│ SceneConstructor +   │
│ (JSON)  │                   │  动画 System          │
└─────────┘                   │  └─ Character Handle  │
                              └──────────────────────┘
```

## 三、文件格式规范

### Mesh (.dxmesh)

二进制格式，包含多个数据段。

```
[Header]
  magic: "DXMESH"
  version: uint32
  numSegments: uint32

[Segments]
  Segment 0: VertexBuffer (position, normal, uv, tangent, color)
  Segment 1: IndexBuffer (uint16 / uint32)
  Segment 2: BoneInfluences (可选) — blendIndices + blendWeights
  Segment 3: PatchControlPoints (可选) — 曲面细分控制点
```

详见 `Engine/Asset/Definitions/Mesh/DxMeshFormat.h`。

### Material (.material)

JSON 格式。

```json
{
  "shader": "shaders/standard.hlsl",
  "params": {
    "baseColor": [0.5, 0.5, 0.5, 1.0],
    "roughness": 0.8,
    "metallic": 0.0
  },
  "textures": {
    "albedo": "textures/body_albedo.dds",
    "normal": "textures/body_normal.dds",
    "roughness": "textures/body_roughness.dds"
  },
  "renderState": {
    "cullMode": "back",
    "blendMode": "opaque",
    "depthWrite": true
  }
}
```

### Scene (.scene)

JSON 格式，实体 + 组件 + 资产引用。

```json
{
  "version": 1,
  "dependencies": {
    "meshes": { "body": "Content/robo/body.dxmesh" },
    "textures": { "albedo": "Content/robo/albedo.dds" },
    "materials": { "bodyMat": "Content/robo/body.material" }
  },
  "entities": [
    {
      "name": "robot",
      "components": [
        { "type": "transform", "position": [0, 0, 0], "rotation": [0, 0, 0, 1] },
        { "type": "mesh", "mesh": "body", "material": "bodyMat" }
      ]
    }
  ]
}
```

### ParticleSystem (.particle)

JSON 格式。

```json
{
  "emission": {
    "rate": 100,
    "bursts": [{ "time": 0, "count": 50 }]
  },
  "particle": {
    "lifetime": { "min": 1.0, "max": 3.0 },
    "speed": { "min": 0.5, "max": 2.0 },
    "size": { "start": 1.0, "end": 0.1 }
  },
  "render": {
    "material": "materials/particle.mat",
    "texture": "textures/particle.dds",
    "blendMode": "additive"
  }
}
```

## 四、加载器注册表

### 接口

```cpp
class IAssetLoader {
public:
    virtual ~IAssetLoader() = default;
    virtual AssetType GetAssetType() const = 0;
    virtual std::vector<std::string> GetSupportedExtensions() const = 0;
    virtual void Load(const std::string &path, AssetCallback callback) = 0;
};
```

### 注册表

| 扩展名 | 加载器 | 输出 | 资产类型 |
|--------|--------|------|---------|
| `.dxmesh` | `DxMeshLoader` | `GeometryHandle` | Mesh |
| `.obj` | `ObjImporter` → `DxMeshLoader` | `GeometryHandle` | Mesh |
| `.dds` | `DDSLoader` | `TextureHandle` | Texture |
| `.png` / `.jpg` | `ImageLoader` → `DDSLoader` | `TextureHandle` | Texture |
| `.material` | `MaterialLoader` | `MaterialHandle` | Material |
| `.bone` | `SkeletonLoader` | `SkeletonHandle` | Skeleton |
| `.anim` | `AnimLoader` | `ClipHandle` | Animation |
| `.character` | `CharacterLoader` | `CharacterHandle` | Character |
| `.scene` | `SceneLoader` | ECS World | Scene |
| `.terrain` | `TerrainLoader` | `TerrainHandle` | Terrain |
| `.particle` | `ParticleLoader` | `ParticleSystemHandle` | ParticleSystem |

### 文件扩展名 → 资产类型（用于 UI 图标）

见 `FileIconProvider.cpp`，与加载器注册表保持同步。

## 五、`AssetType` 枚举定义

```cpp
enum class AssetType : uint8_t {
    // 原子资产
    Mesh,           // .dxmesh, .obj
    Texture,        // .dds, .png, .jpg
    Material,       // .material
    Skeleton,       // .bone     — 骨骼树 + rest pose（HOD 解析导出）
    Animation,      // .anim     — 骨骼动画剪辑（播放/调帧/循环）

    // 复合资产
    Terrain,        // .terrain — JSON 描述，引用 Mesh + Texture + Material
    Scene,          // .scene   — JSON 描述，实体 + 组件 + 资产引用
    Character,      // .character — 骨架 + 网格 + 材质槽 + 动画剪辑打包

    // 预留
    Prefab,         // .prefab  — 实体模板（实例化用）
    ParticleSystem, // .particle — JSON 描述，粒子发射器配置
    Audio           // .wav / .ogg — 音频数据
};
```

## 六、几何系统内部类型

以下类型不提升到 `AssetType`，是 `GeometryResourceManager` 的内部子类型：

| 内部类型 | 所属 | 说明 |
|----------|------|------|
| `TriangleMesh` | `AssetType::Mesh` | 标准三角形网格 |
| `SkinCluster` | `AssetType::Mesh` | 骨骼蒙皮数据（TriangleMesh 的附加数据） |
| `PatchMesh` | `AssetType::Mesh` | 曲面细分控制点网格 |
| `PointSet` | `AssetType::Mesh` / 粒子 | 点集（粒子系统使用） |
| `LineSet` | `AssetType::Mesh` / 调试 | 线段集（调试可视化） |
| `ProceduralGeometry` | 非资产 | 运行时程序化生成 |
| `GridGeometry` | 非资产 | 编辑器辅助网格 |
| `QuadGeometry` | 非资产 | 全屏四边形 |
| `CapsuleGeometry` | 非资产 | 碰撞体 |

## 七、资产预览

| 资产类型 | 预览方式 | 实现状态 |
|----------|---------|---------|
| Mesh | 纯色渲染到离屏 RT | ✅ 已完成 |
| Material | 球体 + 材质渲染 | ❌ 待实现 |
| Texture | 显示纹理图像 | ❌ 待实现 |
| Scene | 完整场景渲染 | ❌ 待实现 |
| ParticleSystem | 粒子播放 + 暂停 | ❌ 待实现 |

## 八、规范变更流程

1. 新增原子资产 → 添加到 `AssetType` → 实现 `IAssetLoader` → 注册到 `AssetManager` → 更新 `FileIconProvider`
2. 新增复合资产 → 添加到 `AssetType` → 实现 `IAssetLoader` → 定义 JSON Schema → 注册到 `AssetManager` → 更新 `FileIconProvider`
3. 新增几何子类型 → 添加到 `GeometryResourceManager` 内部，不提升到 `AssetType`