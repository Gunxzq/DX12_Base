# 资产格式策略：内部格式 vs 源格式兼容

> 日期：2026-07-06
> 关联：`SceneFileAndLoading.md`、`todo.md` 方向 C/D

---

## 问题

引擎需要加载模型、纹理、材质等资产。这些资产的源头可能是 FBX/glTF/OBJ（模型）、PSD/TGA/PNG（纹理），引擎需要决定：

> 运行时直接解析源格式，还是先导入为内部格式再加载？

---

## 两个方向对比

### 方案 A：强制导入为内部格式（类 Unreal / Unity）

```
源文件 (.fbx / .png / .psd)
  │  导入工具（离线或编辑器内）
  └─→ 内部格式 (.ddsmesh / .dds / .mat)
        │  运行时加载器
        └─→ 引擎
```

| 优势 | 代价 |
|:-----|:-----|
| 运行时加载路径统一，代码简单 | 需要导入工具或编辑器支持 |
| 顶点格式可在导入时转换（如 `BLENDINDICES` → `R8G8B8A8_UINT`） | 源文件修改后需重新导入 |
| 纹理可在导入时压缩为 BCn（DDS），运行时零开销 | 开发迭代多一步"导入"操作 |
| 数据可做预处理（焊接顶点、计算 LOD、合并子 mesh） | 内部格式需要版本管理 |

### 方案 B：运行时直接解析源格式（类 Godot）

```
源文件 (.fbx / .png)
  │  运行时解析器（assimp / stb_image）
  └─→ 引擎
```

| 优势 | 代价 |
|:-----|:-----|
| 无导入步骤，修改即所见 | 运行时解析开销（FBX 解析可达 100ms+） |
| 文件路径即资产引用，无需依赖表 | 顶点格式不确定，需运行时适配 |
| 对内容创作者更友好 | 大场景加载时可能卡顿（除非异步流送） |

---

## 项目现状

现有代码和依赖已经明确倾向**方案 A（内部格式）**：

| 依据 | 说明 |
|:-----|:------|
| **纹理** | 已使用 DDS（BCn 压缩），而非 PNG/TGA 运行时解析。当前 `TerrainLoader` 的纹理来自 DDS |
| **顶点格式** | `BLENDINDICES` 使用 `R8G8B8A8_UINT` 而非 FBX/glTF 导出的 `R32G32B32A32_UINT`，需要导入时转换 |
| **`SceneDescription`** | 已假设 `.ddsmesh` / `.mat` / `.dds` 引用结构 |
| **CMake 依赖** | `assimp`（模型导入）、`stb_image`（纹理导入）、`nlohmann/json`（序列化）|
| **`TerrainLoader`** | 已使用 `stb_image` 读取 PNG 高度图，运行时转换 → DDS 格式上传 GPU |

依赖情况：
- **assimp** ✅ 已配置（`CMakeLists.txt:34`、`:136`）
- **stb_image** ✅ 已集成（`ThirdParty/stb/stb_image.h`、`TerrainLoader.cpp` 使用）
- **nlohmann/json** ✅ 已配置，`SceneLoader` / `SceneDescription` 使用
- **DDS 工具** ✅ 已有 `DDSLoader` / `DDSUtils`

---

## 推荐架构

### 分层：导入层 → 运行时加载层

```
                      开发期 / 编辑器              运行时
                      ────────────────            ────────

源文件 (.fbx)  ──→  导入工具 (assimp)  ──→  .ddsmesh  ──→  MeshLoader
源文件 (.png)  ──→  导入工具 (stb_image) ──→  .dds     ──→  TextureLoader / TerrainLoader
源文件 (.psd)  ──→                            .mat     ──→  MaterialLoader
                 ↓ 导入时处理
                 顶点格式转换 / 纹理压缩 / 数据优化
```

### 开发期的折中

在正式导入工具完成前，通过**代码生成 + 轻量 JSON 描述**过渡：

```
阶段 1（现状）：C++ 代码内程序化生成几何体 + 硬编码纹理路径
  ↓
阶段 2：定义轻量 JSON 格式（TerraiDesc / MeshDesc / MaterialDesc）
        运行时 Loader 解析 JSON + 创建资源
        路径从 SceneLoader 统一
  ↓
阶段 3：离线导入工具
        FBX → .ddsmesh（assimp 处理顶点格式、焊接、LOD）
        PNG/TGA → .dds（stb_image 读取 + BCn 压缩）
        场景编辑器中拖拽导入
```

### 各资产类型格式规划

| 资产类型 | 内部格式 | 加载器 | 导入工具链 |
|:---------|:---------|:-------|:----------|
| **Mesh** | `.ddsmesh` | MeshLoader | assimp (FBX/glTF/OBJ) → 顶点格式转换 → 写入 |
| **Texture** | `.dds` | TextureLoader / DDSLoader | stb_image (PNG/TGA) → BCn 压缩 → 写入 |
| **Material** | `.mat` (JSON) | MaterialLoader | 场景编辑器编辑 / JSON 手写 |
| **Terrain** | `.hmap` (JSON + 纹理) | TerrainLoader | 高度图 PNG → stb_image 读取 → GPU 资源 |
| **Scene** | `.scene.json` | SceneLoader | 场景编辑器编辑 / JSON 手写 |

---

## 当前路线图对照

| 阶段 | 内容 | 当前状态 |
|:----:|:------|:---------|
| **P0** | 定义 `.ddsmesh` / `.mat` 等内部格式 | ❌ 未定义 |
| **P0** | 完善 `TerrainDesc` JSON 格式（地形纹理列表等） | ❌ 未定义 |
| **P0** | `SceneLoader` + `SceneDescription` 骨架 | ✅ 已有 |
| **P1** | `TerrainLoader` 从地形 JSON 读取纹理列表（替代硬编码路径） | ❌ 硬编码中 |
| **P1** | `TextureUploadTask` 复用（方向 C） | ❌ 待实施 |
| **P2** | `AssetManager` 统一加载管线（方向 D） | ◐ 骨架 |
| **P3** | 离线导入工具 (assimp → .ddsmesh / stb_image → .dds) | ❌ |

---

## 关键决策记录

1. **纹理不走运行时格式解析**：DDS 是唯一运行时纹理格式。PNG/TGA 等通过导入工具转换为 DDS。
2. **模型不走运行时 assimp**：assimp 只在导入工具中使用，运行时 MeshLoader 只解析 `.ddsmesh`。
3. **Material 使用 JSON**：材质是数据驱动的描述文件，JSON 可读写，编辑器友好。
4. **Terrain 使用 JSON + DDS 纹理**：地形描述 JSON 包含高度图路径、纹理列表、尺寸参数；纹理文件为 DDS。
5. **SceneDescription 的 dependencies 节是单次加载的全部资产清单**，加载器无需遍历 entity 树。

---

## 核心收益分析

### 运行时性能

内部格式使运行时加载路径变成纯"读取 + 创建 GPU 资源"，不存在格式转换开销：

```
运行时（内部格式）：
  .ddsmesh → CreateVertexBuffer / CreateIndexBuffer           ← 零转换
  .dds     → CreateTexture2D + CopyResource                    ← 零转换
  .mat     → 解析 JSON 字段 → 填充 Material 结构体             ← 轻量 JSON 解析

运行时（源格式兼容）：
  .fbx     → assimp 解析 → 顶点格式转换 → CreateVertexBuffer  ← 10-100ms
  .png     → stb_image 解码 → CPU 像素操作 → CreateTexture2D   ← 1-10ms
  .gltf    → 解析 JSON + buffer 解码 → 多资源拆分              ← 复杂
```

### 资产分离与复用

内部格式天然支持资产分离存储，每个资产文件只包含单一职责的数据：

```
.ddsmesh（几何体）
  ├─ 顶点数据（位置/法线/UV/切线/BLENDINDICES/BLENDWEIGHT）
  ├─ 索引数据
  └─ 包围盒

.dds（纹理）
  ├─ BCn 压缩像素数据
  └─ mip 链

.mat（材质，JSON）
  ├─ baseColor     → "Textures/brick_diffuse.dds"
  ├─ normal        → "Textures/brick_normal.dds"
  ├─ metallic      → "Textures/brick_metallic.dds"
  ├─ roughness     → "Textures/brick_roughness.dds"
  └─ shaderModel   → "PBR/Standard"

.scene.json（场景，JSON）
  ├─ dependencies（资产引用表，key → 路径）
  └─ entities（组件引用 key）
```

复用性体现：

| 资产 | 可被多少场景/材质引用 | 实际价值 |
|:-----|:---------------------|:---------|
| `cube.ddsmesh` | N 个材质 × M 个场景 | ✅ 高，基础几何体通用 |
| `brick_diffuse.dds` | N 个材质 | ✅ 高，同纹理不同材质参数 |
| `stone.mat` | M 个场景中的实体 | ◐ 中，PBR 参数可调 |
| `test_scene.json` | 1 次加载 | ❌ 场景本身不跨场景 |

即使 PBR 材质因光照/色调不同难以直接复用，**mesh 和 texture 的跨材质复用**已经覆盖了大部分存储和加载的收益。
