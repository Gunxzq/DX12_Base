#pragma once
// ========================================================================
// XFileDirectReader — 不依赖 assimp 的 DirectX X File 二进制解析器
//
// 直接解析 xof 0303bin 格式，不应用帧变换、不焊接顶点、不拆分材质，
// 确保输出与社区工具的顶点数据一致。
// ========================================================================

#include "XFileParser.h" // 复用 XFileMesh / XFileMaterial 结构
#include <string>
#include <vector>

namespace AssetTool {

/// 直接解析 .x 文件（跳过 assimp）
class XFileDirectReader {
public:
    XFileDirectReader() = default;

    /// 从文件路径解析
    bool ParseFile(const std::string &filepath);

    /// 从内存解析
    bool Parse(const uint8_t *data, size_t size);

    const std::vector<XFileMesh> &GetMeshes() const { return m_meshes; }
    const std::string &GetError() const { return m_error; }

private:
    bool ReadBinary(const uint8_t *data, size_t size);
    bool ReadText(const uint8_t *data, size_t size);

    std::vector<XFileMesh> m_meshes;
    std::string m_error;
};

} // namespace AssetTool
