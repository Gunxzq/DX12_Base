# Mesh 二进制格式 — `.dxmesh`

> 日期：2026-07-06
> 关联：`M3dLoader.h`、`GeometryGenerator.h`、`AssetFormatStrategy.md`

---

## 设计目标

| 目标 | 说明 |
|:-----|:------|
| 运行时零解析 | `ReadFile` → 解析 Header → `CreateVertexBuffer`(数据指针) → `CreateIndexBuffer`(数据指针)，全程 memcpy |
| D3D12 原生对齐 | 顶点数据在磁盘上的布局 = GPU 输入布局，无字段重排 |
| 支持静态/蒙皮 | 两种顶点格式，由 Header 的 `Skinned` 标志区分 |
| 支持 LOD | 多级细节，每级独立的顶点/索引偏移 |
| 可 mmap | 格式设计允许直接内存映射加载（未来优化） |

---

## 文件布局

```
┌──────────────────────────────────────────────────┐
│  Header (128 bytes, 固定大小)                      │
├──────────────────────────────────────────────────┤
│  Vertex Data (VertexCount × VertexStride bytes)    │
├──────────────────────────────────────────────────┤
│  Index Data (IndexCount × IndexSize bytes)         │
├──────────────────────────────────────────────────┤
│  (可选) LOD 层级表                                  │
└──────────────────────────────────────────────────┘
```

---

## Header 结构

```cpp
#pragma pack(push, 1)
struct DxMeshHeader {
    // ── 标识 ──
    char     magic[8];        // "DXMESH\0"
    uint32_t version;         // 当前 = 1

    // ── 数据计数 ──
    uint32_t vertexCount;     // 顶点总数
    uint32_t indexCount;      // 索引总数
    uint32_t vertexStride;    // 单个顶点字节数（Static=40, Skinned=64）

    // ── 格式标志 ──
    uint32_t flags;           // bit 0: Skinned, bit 1: 16-bit Index
    uint32_t indexSize;       // 2 or 4

    // ── 包围盒 ──
    float    boundsMin[3];
    float    boundsMax[3];

    // ── LOD ──
    uint32_t lodCount;        // 层级数（至少 1）
    uint32_t lodOffset;       // LOD 表在文件中的字节偏移（相对文件头）

    // ── 保留 ──
    uint64_t reserved[7];     // 未来扩展（骨架哈希等）
};
static_assert(sizeof(DxMeshHeader) == 128, "DxMeshHeader must be 128 bytes");
#pragma pack(pop)
```

### flags 定义

```cpp
enum DxMeshFlags : uint32_t {
    DxMesh_Skinned    = 1 << 0,  // 顶点含骨骼数据
    DxMesh_Index16    = 1 << 1,  // 索引为 uint16_t（默认 uint32_t）
};
```

---

## 顶点格式

### StaticVertex（非蒙皮网格）

```cpp
#pragma pack(push, 1)
struct DxMeshStaticVertex {
    DirectX::XMFLOAT3 Position;    // offset  0, 12 bytes
    DirectX::XMFLOAT3 Normal;      // offset 12, 12 bytes
    DirectX::XMFLOAT3 TangentU;    // offset 24, 12 bytes
    DirectX::XMFLOAT2 TexC;        // offset 36,  8 bytes
    // total = 44 bytes
};
#pragma pack(pop)
```

对应 D3D12 输入布局：

| 槽 | 语义 | 格式 | 对齐偏移 |
|:---|:-----|:-----|:---------|
| 0 | `POSITION` | `R32G32B32_FLOAT` | 0 |
| 0 | `NORMAL` | `R32G32B32_FLOAT` | 12 |
| 0 | `TANGENT` | `R32G32B32_FLOAT` | 24 |
| 0 | `TEXCOORD` | `R32G32_FLOAT` | 36 |

### SkinnedVertex（蒙皮网格）

```cpp
#pragma pack(push, 1)
struct DxMeshSkinnedVertex {
    DirectX::XMFLOAT3 Position;     // offset  0, 12 bytes
    DirectX::XMFLOAT3 TangentU;     // offset 12, 12 bytes
    DirectX::XMFLOAT3 Normal;       // offset 24, 12 bytes
    DirectX::XMFLOAT2 TexC;         // offset 36,  8 bytes
    DirectX::XMFLOAT4 BoneWeights;  // offset 44, 16 bytes
    uint8_t          BoneIndices[4]; // offset 60,  4 bytes
    // total = 64 bytes, 16-byte aligned
};
#pragma pack(pop)
```

> 与现有的 `M3dVertex`（64 字节）完全兼容，无需重排。

对应 D3D12 输入布局：

| 槽 | 语义 | 格式 | 对齐偏移 |
|:---|:-----|:-----|:---------|
| 0 | `POSITION` | `R32G32B32_FLOAT` | 0 |
| 0 | `TANGENT` | `R32G32B32_FLOAT` | 12 |
| 0 | `NORMAL` | `R32G32B32_FLOAT` | 24 |
| 0 | `TEXCOORD` | `R32G32_FLOAT` | 36 |
| 1 | `BLENDWEIGHT` | `R32G32B32A32_FLOAT` | 44 |
| 1 | `BLENDINDICES` | `R8G8B8A8_UINT` | 60 |

---

## 索引格式

| flag | 索引类型 | 大小 | 说明 |
|:-----|:---------|:-----|:------|
| 无 | `uint32_t` | 4 字节 | 默认，支持大网格 |
| `DxMesh_Index16` | `uint16_t` | 2 字节 | 顶点数 < 65536 时节省带宽 |

---

## LOD 层级表

紧跟在索引数据之后（由 `lodOffset` 指定偏移），结构如下：

```cpp
#pragma pack(push, 1)
struct DxMeshLOD {
    uint32_t vertexOffset;   // 相对顶点数据起始的偏移（顶点数）
    uint32_t vertexCount;    // 本 LOD 顶点数
    uint32_t indexOffset;    // 相对索引数据起始的偏移（索引数）
    uint32_t indexCount;     // 本 LOD 索引数
    float    errorMetric;    // 简化误差（仅用于 LOD 切换阈值计算）
};
#pragma pack(pop)
```

`lodCount` 个 `DxMeshLOD` 连续排列。LOD 0 为最高细节（原始网格），LOD N-1 为最低细节。

---

## 文件写入/读取流程

### 导入工具写入

```
assimp 解析 FBX → M3dMeshData（现有流程）
  ↓
转换到 DxMeshVertex 数组（保持布局不变）
  ↓
填充 Header
  ↓
写入文件：
  Header → Vertex Data → Index Data → LOD Table
```

### 运行时加载

```
ReadFile(完整文件到内存)
  ├─ reinterpret_cast<DxMeshHeader*>(base_ptr)     ← 零解析
  ├─ 顶点指针 = base_ptr + sizeof(DxMeshHeader)
  ├─ 索引指针 = 顶点指针 + header.vertexCount * header.vertexStride
  ├─ CreateVertexBuffer(vptr, vertexCount, stride)
  ├─ CreateIndexBuffer(iptr, indexCount, indexSize)
  └─ 释放文件内存（或在亚稳态帧后释放）
```

---

## 与现有格式的关系

| 现有格式 | 关系 |
|:---------|:-----|
| `.m3d` 文本 | 导入工具读取 `.m3d`，输出 `.dxmesh`。`M3dLoader` 保留做导入工具使用，运行时不再依赖 |
| `GeometryGenerator::Vertex` | 与 `DxMeshStaticVertex` 布局完全一致，可直接 reinterpret_cast |
| `M3dVertex` | 与 `DxMeshSkinnedVertex` 布局完全一致（64 字节） |
