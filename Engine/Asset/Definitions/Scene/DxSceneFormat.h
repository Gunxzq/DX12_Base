#pragma once
#include <cstdint>

// ============================================================================
// DxScene — 运行时零解析 Scene 二进制格式（.scene 后缀）
//
// 对齐 DxMesh 规范（DxMeshFormat.h）——**完整覆盖 JSON 架构内容**（用户定案：
// 场景二进制存储化不能丢失 JSON 架构所定义的内容——环境/材质/材质引用/实体/水块全保留）
//
// 磁盘布局（分节，Header 后依次连续）：
//   Header + Mesh 名表 + 材质表 + SOA 实体数组 + 水块数组 + 环境段
//
// 迭代策略（用户定案）：JSON（.scene.json）用于快速迭代/测试字段，二进制（.scene）用于固化
// ============================================================================

#pragma pack(push, 1)

// ── 文件魔数 ──
static constexpr char DX_SCENE_MAGIC[8] = {'D', 'X', 'S', 'C', 'E', 'N', 'E', '\0'};
static constexpr uint32_t DX_SCENE_VERSION = 2;

// ── Header（40 字节，固定大小） ──
struct DxSceneHeader {
    char magic[8];            // "DXSCENE\0"
    uint32_t version;         // 当前 = 2
    uint32_t entityCount;     // 实体总数
    uint32_t meshCount;       // Mesh 名表数量
    uint32_t materialCount;   // 材质表数量
    uint32_t waterBlockCount; // 水块数量（邻接 Sea 合并）
    uint32_t flags;           // 标志位（DxSceneFlags——环境段是否存在等）
};

// ── 标志位 ──
enum DxSceneFlags : uint32_t {
    DxSceneFlag_HasEnvironment = 1 << 0, // 含环境段（sceneEnvironment：ambient/skybox 等）
    DxSceneFlag_HasSkybox = 1 << 1,      // 含天空盒
};

// ── 材质引用（每实体 mesh 的材质槽——与 JSON comp.mesh.materials 对齐） ──
// 简化：实体 mesh 的材质槽 = materialCount 索引（单槽——多槽扩展按版本递增）
// 每实体：materialIdx（u32——索引材质表，UINT32_MAX = 无材质）

// ── 水块（每块 40 字节——10 float；旋转恒等（水面 Y=0 平面）省略，引擎重建世界矩阵） ──
struct DxSceneWaterBlock {
    float minX, minZ, maxX, maxZ; // 世界四边形角点（min/max）
    float posX, posY, posZ;       // 世界矩阵位置（Y=0 水面）
    float scaleX, scaleY, scaleZ; // 世界矩阵缩放（宽高比补全——scaleY=1 固定）
    float tilingX, tilingZ;       // 纹理平铺（每 30 单位重复一次）
};

// ── 磁盘布局（Header 后依次连续） ──
// [Mesh 名表]   meshCount ×（u16 len + len 字节 UTF-8 名称）——soaMeshIdx 索引引用
// [材质表]      materialCount ×（u16 len + len 字节 UTF-8 材质名）——soaMaterialIdx 索引引用
//              （材质参数/引用由 MaterialManager 按名解析——与 JSON materials 对齐）
// [SOA 实体数组]（每字段连续存储，entityCount 对齐——memcpy 直接读）：
//   persistentId[entityCount]  （u64——索引作标识，引擎按序构造实体）
//   meshIdx[entityCount]       （u32——索引 Mesh 名表）
//   materialIdx[entityCount]   （u32——材质引用，索引材质表；UINT32_MAX = 无）
//   position[entityCount * 3]  （f32 XYZ）
//   rotation[entityCount * 4]  （f32 XYZW 四元数）
//   scale[entityCount * 3]     （f32 XYZ）
//   cullDistance[entityCount]  （f32——MPD @CullFar，0 = 不限制）
// [水块数组]   waterBlockCount × DxSceneWaterBlock（40 字节/块——程序化水面四边形）
// [环境段]     仅 DxSceneFlag_HasEnvironment 时存在：
//              ambient（4 × f32：RGBA）+ entityMotionPolicy（u16 len + 字节）
//              + skybox（DxSceneFlag_HasSkybox 时：u16 len + 字节 天空盒纹理名）
//
// 说明：二进制 = JSON 架构的等价存储化（环境/材质/材质引用/实体/水块全保留——
//       不丢失 JSON 架构所定义的内容）；JSON 用于迭代，二进制用于固化
// ============================================================================

#pragma pack(pop)
