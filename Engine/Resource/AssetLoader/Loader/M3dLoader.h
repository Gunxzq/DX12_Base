#pragma once

#include "Resource/Skeleton/SkeletonData.h"
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace DX12Engine::Resource {

// ============================================================================
// M3d 顶点格式（与 .m3d 文本格式对齐，可直接上传 GPU）
// .m3d 存储顺序: Pos → TangentU → Normal → TexC → BoneWeights → BoneIndices
// ============================================================================
#pragma pack(push, 1)
struct M3dVertex {
    DirectX::XMFLOAT3 Pos;         // offset  0 (12 bytes)
    DirectX::XMFLOAT3 TangentU;    // offset 12 (12 bytes)
    DirectX::XMFLOAT3 Normal;      // offset 24 (12 bytes)
    DirectX::XMFLOAT2 TexC;        // offset 36 ( 8 bytes)
    DirectX::XMFLOAT4 BoneWeights; // offset 44 (16 bytes)
    uint8_t BoneIndices[4];        // offset 60 ( 4 bytes)
    // total = 64 bytes, 16-byte aligned
};
#pragma pack(pop)

static_assert(sizeof(M3dVertex) == 64, "M3dVertex must be 64 bytes for GPU upload");

// ============================================================================
// 子集定义（一个子集 = 一组相同材质的三角形）
// ============================================================================
struct M3dSubset {
    uint32_t materialIndex;
    uint32_t vertexStart;
    uint32_t vertexCount;
    uint32_t faceStart;
    uint32_t faceCount;
};

// ============================================================================
// 材质定义
// ============================================================================
struct M3dMaterial {
    DirectX::XMFLOAT4 DiffuseAlbedo = {1.0f, 1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT3 FresnelR0 = {0.05f, 0.05f, 0.05f};
    float Roughness = 0.5f;
    bool AlphaClip = false;
    std::string EffectTypeName;
    std::string DiffuseMapName;
    std::string NormalMapName;
};

// ============================================================================
// M3d 加载输出（一个完整的 .m3d 解析结果）
// ============================================================================
struct M3dMeshData {
    // 网格数据
    std::vector<M3dVertex> vertices;
    std::vector<uint32_t> indices; // 三角形列表，每 3 个一组
    std::vector<M3dSubset> subsets;
    std::vector<M3dMaterial> materials;

    // 骨骼数据（可用于构造 SkeletonData）
    std::vector<int> boneHierarchy;               // 父骨骼索引，根为 -1
    std::vector<DirectX::XMFLOAT4X4> boneOffsets; // 绑定姿势逆矩阵
    std::vector<std::string> boneNames;

    // 动画数据（ClipName → AnimationClip）
    std::unordered_map<std::string, AnimationClip> animations;

    bool HasSkeleton() const { return !boneHierarchy.empty(); }
    bool IsValid() const { return !vertices.empty() && !indices.empty(); }
};

// ============================================================================
// M3d 加载器
// ============================================================================
class M3dLoader {
public:
    M3dLoader() = delete;

    /// 从 .m3d 文件加载完整数据
    static bool LoadFromFile(const std::string &filepath, M3dMeshData &outData);
};

} // namespace DX12Engine::Resource
