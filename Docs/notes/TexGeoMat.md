好的，让我具体分析这三个系统在运行时是否会动态创建 GPU 资源。

## 先确认当前设计的实际情况

根据你提供的代码：

| 管理器 | 实际持有的内容 | 是否管理 GPU 资源 |
|--------|---------------|------------------|
| `MaterialManager` | `MaterialData`（CPU 参数） + `refCount` | ❌ 不持有 GPU 资源，只提供常量数据供上传 |
| `GeometryResourceManager` | `TriangleMesh`（CPU 顶点/索引数据） | ❌ 不持有 GPU 资源，只存 CPU 端几何数据 |
| `TextureManager` | `GpuResourceHandle`（引用 GPU 资源）+ `srvIndex` | ✅ 持有 GPU 资源句柄 + 描述符槽位 |

**结论：只有 TextureManager 真正管理 GPU 资源，Material 和 Geometry 只是 CPU 端数据容器。**

## 运行时会动态创建 GPU 资源的场景

### 场景 1：TextureManager ✅ 运行时确实需要

| 场景 | 是否运行时 | 说明 |
|------|-----------|------|
| 异步加载纹理 | 是（加载完成时） | 文件 IO 完成后，主线程创建 GPU 纹理并注册 |
| 渲染到纹理（RTT） | 是（每帧或按需） | 反射探针、小地图、后处理特效 |
| 动态生成纹理（Procedural） | 是 | 运行时算法生成噪波图、高度图等 |
| 纹理图集动态扩容 | 是 | 运行时添加新 Sprite 需要扩容或创建新纹理 |
| 跨帧纹理复用（池） | 是 | 临时纹理池（如临时渲染目标） |

**TextureManager 封装 GPU 资源管理是有价值的** — 因为它需要处理异步加载完成时的资源注册。

---

### 场景 2：MaterialManager ❌ 运行时几乎不创建 GPU 资源

| 场景 | 是否运行时 | 说明 |
|------|-----------|------|
| 材质参数修改 | 否 | 只是修改 CPU 端参数，下一帧上传到 CBV |
| 动态材质实例化 | 否 | 新建 `MaterialData` 结构，不涉及 GPU 资源 |
| 材质切换 | 否 | 只是引用不同句柄 |
| 运行时编译材质 Shader | 否 | 通常离线完成，除非有运行时 Shader 编译系统 |

**MaterialManager 不管理 GPU 资源** — 它只管理 CPU 端的材质参数结构体。GPU 侧是通过 `GetGPUMaterialList()` 批量上传到常量缓冲区，不是每个材质独立分配 GPU 资源。

**潜在运行时场景（罕见）：**
- **运行时生成材质**：比如根据玩家涂装动态生成材质参数 → 仍只是 CPU 数据，无新 GPU 资源
- **材质实例化数量爆炸**：可能需要 GPU 端的材质参数缓冲区动态扩容 → 涉及 `RingBuffer` 或 `GpuResourceManager` 重新分配缓冲区，但这是 `MaterialManager` 的间接依赖，不是它直接创建

---

### 场景 3：GeometryResourceManager ❌ 运行时几乎不创建 GPU 资源

| 场景 | 是否运行时 | 说明 |
|------|-----------|------|
| 异步加载网格 | 是（加载完成时） | 但 GeometryManager 只存 CPU 数据，不创建 GPU VB/IB |
| 程序化生成网格 | 是 | 运行时生成顶点数据（地形、水面），但仍需上传到 GPU |
| LOD 切换 | 是 | 加载不同精度的网格数据 |
| 蒙皮骨骼矩阵 | 是（每帧） | 通过 `RingBuffer` 上传，不是 GeometryManager 职责 |

**关键问题：谁来创建 GPU 顶点/索引缓冲区？**

当前 `GeometryResourceManager` 只存 CPU 数据，实际 GPU 资源创建应该在：
- 渲染系统遍历可见物体时，检查是否已上传到 GPU
- 如果没有，通过 `GpuResourceManager` 创建 VB/IB
- 或者有一个专门的 `VertexBufferPool` / `MeshGpuCache`

**这是当前架构可能存在的问题** — CPU 端网格数据与 GPU 端资源的关联丢失了。

---

## 总结：三个系统的运行时 GPU 资源创建需求

| 系统 | 是否需要运行时创建 GPU 资源 | 说明 |
|------|--------------------------|------|
| **TextureManager** | ✅ **经常需要** | 异步加载完成、RTT、过程化纹理 |
| **MaterialManager** | ❌ **几乎不需要** | 只是 CPU 参数容器，GPU 侧是批量上传 |
| **GeometryResourceManager** | ⚠️ **需要但当前缺失** | CPU 数据存在，但缺少 GPU 缓冲区创建/管理 |

## 对阴影贴图的启示

阴影贴图与这三个系统的情况不同：

| 维度 | 材质/几何/纹理（资产） | 阴影贴图（运行时） |
|------|---------------------|-------------------|
| GPU 资源创建频率 | 加载时一次性 | 每帧/每光源可能变化 |
| 引用计数 | 需要（多物体共享） | 不需要（光源独占） |
| 异步加载 | 需要 | 不需要 |
| 语义归属 | 独立资产 | 光源的属性 |

**结论：** 阴影贴图不应该放入 TextureManager，因为：
1. 它不需要引用计数
2. 它不需要异步加载路径
3. 它的生命周期完全由 LightManager 控制
4. 它使用 DSV 而非 SRV

**建议：** LightManager 直接持有：
```cpp
struct LightShadowData {
    GpuResourceHandle depthTexture;  // 来自 GpuResourceManager
    uint32_t dsvSlot;                // 来自 DescriptorSlotAllocator（DSV堆）
    uint32_t resolution;
    bool needsRedraw;                // 光源移动/参数变化时重新渲染
};
```

这样不经过 TextureManager，语义清晰，职责分离。