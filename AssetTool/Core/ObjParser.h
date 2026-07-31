#pragma once
// ========================================================================
// ObjParser — 轻量 OBJ 格式解析器
// 只读 v/vt/vn/f，不焊接顶点、不优化索引，确保数据原始
// ========================================================================

#include <string>
#include <vector>

namespace AssetTool {

/// OBJ 文件解析结果，与 XFileMesh 兼容
struct ObjMesh {
    std::vector<float> positions;   // float3
    std::vector<float> normals;     // float3
    std::vector<float> texcoords;   // float2
    std::vector<uint32_t> indices;  // triangle list

    /// 是否包含法线
    bool HasNormals() const { return !normals.empty(); }

    /// 是否包含纹理坐标
    bool HasTexcoords() const { return !texcoords.empty(); }

    /// 顶点数
    uint32_t VertexCount() const { return static_cast<uint32_t>(positions.size() / 3); }
};

/// OBJ 解析器
class ObjParser {
public:
    ObjParser() = default;

    /// 从文件解析 OBJ
    bool ParseFile(const std::string &filepath);

    /// 从内存解析 OBJ
    bool Parse(const std::string &content);

    const ObjMesh &GetMesh() const { return m_mesh; }
    const std::string &GetError() const { return m_error; }

private:
    ObjMesh m_mesh;
    std::string m_error;

    // 临时顶点索引映射（v/t/n → 唯一顶点）
    bool m_hasTex = false;
    bool m_hasNorm = false;
};

} // namespace AssetTool
