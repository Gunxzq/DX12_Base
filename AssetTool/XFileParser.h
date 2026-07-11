#pragma once
// ========================================================================
// XFileParser — DirectX XFile (assimp 实现)
//
// 底层使用 assimp 解析 .x 文件（xof 0303bin/txt），
// 输出为引擎可用的 DxMeshFormat + 材质描述。
//
// assimp 自动处理：
//   - 二进制/文本 .x 格式检测
//   - 三角形化（MeshFace → 三角）
//   - 顶点焊接
//   - 法线生成（降级）
// ========================================================================

#include "Asset/Definitions/Mesh/DxMeshFormat.h"
#include "Asset/Definitions/Material/MaterialDesc.h"
#include <cstdint>
#include <string>
#include <vector>

// 前置声明 assimp 类型（避免暴露 assimp 头文件给外部）
struct aiScene;
struct aiMaterial;

namespace AssetTool {

/// 从 .x Material 块提取的材质数据
struct XFileMaterial {
    float faceColor[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    float power = 0.0f;
    float specularColor[3] = {0.0f, 0.0f, 0.0f};
    float emissiveColor[3] = {0.0f, 0.0f, 0.0f};
    std::string textureFilename;

    /// 转换为引擎 MaterialDesc（PBR 映射）
    DX12Engine::Resource::MaterialDesc ToMaterialDesc() const;
};

/// 单个网格数据（一个 assimp aiMesh → 一个 XFileMesh）
struct XFileMesh {
    std::string name;

    std::vector<float> positions;   // float3
    std::vector<float> normals;     // float3
    std::vector<float> texcoords;   // float2
    std::vector<uint32_t> indices;  // 三角形索引

    XFileMaterial material;

    float boundsMin[3];
    float boundsMax[3];

    bool HasNormals() const { return !normals.empty(); }
    bool HasTexcoords() const { return !texcoords.empty(); }
    size_t VertexCount() const { return positions.size() / 3; }

    bool WriteDxMesh(const std::string &outputPath) const;
    void ComputeBounds();
};

/// 解析器（assimp 后端，支持自动 XOR 解密检测）
class XFileParser {
public:
    XFileParser() = default;

    /// 从内存加载并解析 .x 文件
    bool Parse(const uint8_t *data, size_t size);

    /// 从文件路径加载并解析（自动检测 XOR 加密）
    bool ParseFile(const std::string &filepath);

    const std::vector<XFileMesh> &GetMeshes() const { return m_meshes; }
    const std::string &GetError() const { return m_error; }

    /// 导入标志（默认 Triangulate|GenNormals|JoinIdentical）
    unsigned int m_importFlags = 0;

    /// 是否启用自动 XOR 解密重试（默认 true）
    bool m_autoDecrypt = true;

    /// 当 autoDecrypt 启用时尝试的 XOR key 列表（默认仅 PowerUpKit key）
    std::vector<uint32_t> m_decryptKeys = { 0x0B7E7759 };

private:
    /// 尝试一次导入（不重试）
    bool TryImport(const uint8_t *data, size_t size, std::string &outError);

    bool Import(const aiScene *scene);

    std::vector<XFileMesh> m_meshes;
    std::string m_error;
};

} // namespace AssetTool
