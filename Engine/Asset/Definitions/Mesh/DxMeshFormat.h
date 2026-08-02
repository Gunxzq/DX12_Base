#pragma once
#include <cstdint>

// ============================================================================
// DxMesh — 运行时零解析 Mesh 二进制格式
//
// 磁盘布局：
//   Header (128 bytes) + Vertex Data + Index Data + LOD Table
//
// 运行时加载：
//   ReadFile → 检查 Magic → CreateVertexBuffer/CreateIndexBuffer（memcpy）
//   无需解析每顶点数据。
//
// 与现有顶点格式的兼容性：
//   DxMeshStaticVertex ↔ GeometryGenerator::Vertex（布局相同，44 字节）
//   DxMeshSkinnedVertex = DxMeshStaticVertex 头部（44B）+ 骨骼尾部（20B）= 64 字节
//   头部 position/normal/tangentU/texC 与静态一致，换渲染器前 44 字节可正确读出（见 SubMeshMaterialSlots.md §2.3a）
// ============================================================================

#pragma pack(push, 1)

// ── 文件魔数 ──
static constexpr char DX_MESH_MAGIC[8] = {'D', 'X', 'M', 'E', 'S', 'H', '\0', '\0'};
static constexpr uint32_t DX_MESH_VERSION = 1;

// ── 标志位 ──
enum DxMeshFlags : uint32_t {
    DxMeshFlag_Skinned = 1 << 0, // 顶点包含骨骼数据（BoneWeights + BoneIndices）
    DxMeshFlag_Index16 = 1 << 1, // 索引为 uint16_t（默认 uint32_t）
};

// ── Header（128 字节，固定大小） ──
struct DxMeshHeader {
    char magic[8];         // "DXMESH\0\0"
    uint32_t version;      // 当前 = 1
    uint32_t vertexCount;  // 顶点总数
    uint32_t indexCount;   // 索引总数
    uint32_t vertexStride; // 单顶点字节数（Static=44, Skinned=64）

    uint32_t flags;     // DxMeshFlags
    uint32_t indexSize; // 2 或 4

    float boundsMin[3]; // AABB 最小值
    float boundsMax[3]; // AABB 最大值

    uint32_t lodCount;  // LOD 层级数（至少 1）
    uint32_t lodOffset; // LOD 表在文件中的字节偏移

    uint32_t subMeshCount;  // SubMesh 数量（0 表示整个网格为一个 SubMesh）
    uint32_t subMeshOffset; // SubMesh 表在文件中的字节偏移

    uint64_t padding;     // 对齐填充
    uint64_t reserved[6]; // 未来扩展
};
static_assert(sizeof(DxMeshHeader) == 128, "DxMeshHeader must be 128 bytes");

// ── 静态顶点（非蒙皮） ──
// 对应 GeometryGenerator::Vertex 布局
struct DxMeshStaticVertex {
    float position[3]; // offset  0, 12 bytes
    float normal[3];   // offset 12, 12 bytes
    float tangentU[3]; // offset 24, 12 bytes
    float texC[2];     // offset 36,  8 bytes
    // total = 44 bytes
};
static_assert(sizeof(DxMeshStaticVertex) == 44, "DxMeshStaticVertex must be 44 bytes");

// ── 蒙皮顶点 ──
// 头部与 DxMeshStaticVertex 一致（position/normal/tangentU/texC），仅尾部追加骨骼字段
// 布局统一：头部 44B 静态/蒙皮共用，换渲染器前 44 字节可正确读出（见 SubMeshMaterialSlots.md §2.3a）
struct DxMeshSkinnedVertex {
    float position[3];      // offset  0, 12 bytes
    float normal[3];        // offset 12, 12 bytes
    float tangentU[3];      // offset 24, 12 bytes
    float texC[2];          // offset 36,  8 bytes
    float boneWeights[4];   // offset 44, 16 bytes
    uint8_t boneIndices[4]; // offset 60,  4 bytes
    // total = 64 bytes, 16-byte aligned
};
static_assert(sizeof(DxMeshSkinnedVertex) == 64, "DxMeshSkinnedVertex must be 64 bytes");

// ── LOD 层级 ──
struct DxMeshLOD {
    uint32_t vertexOffset; // 顶点偏移（相对顶点数据起始，按顶点数计）
    uint32_t vertexCount;  // 本 LOD 顶点数
    uint32_t indexOffset;  // 索引偏移（相对索引数据起始，按索引数计）
    uint32_t indexCount;   // 本 LOD 索引数
    float errorMetric;     // 简化误差
};
static_assert(sizeof(DxMeshLOD) == 20, "DxMeshLOD must be 20 bytes");

// ── SubMesh 层级 ──
// 紧跟在 LOD 表之后，由 subMeshOffset 指定偏移
struct DxMeshSubMesh {
    uint32_t indexOffset;  // 索引偏移（相对索引数据起始，按索引数计）
    uint32_t indexCount;   // 本 SubMesh 索引数
    uint32_t vertexOffset; // 顶点偏移（相对顶点数据起始，按顶点数计）
};

// ── 文件内偏移计算辅助 ──
inline const uint8_t *DxMesh_GetVertexData(const DxMeshHeader *header) {
    return reinterpret_cast<const uint8_t *>(header) + sizeof(DxMeshHeader);
}
inline const uint8_t *DxMesh_GetIndexData(const DxMeshHeader *header) {
    return DxMesh_GetVertexData(header) + header->vertexCount * header->vertexStride;
}
inline const DxMeshLOD *DxMesh_GetLODTable(const DxMeshHeader *header) {
    return reinterpret_cast<const DxMeshLOD *>(reinterpret_cast<const uint8_t *>(header) + header->lodOffset);
}
inline const DxMeshSubMesh *DxMesh_GetSubMeshTable(const DxMeshHeader *header) {
    return reinterpret_cast<const DxMeshSubMesh *>(reinterpret_cast<const uint8_t *>(header) + header->subMeshOffset);
}

#pragma pack(pop)
