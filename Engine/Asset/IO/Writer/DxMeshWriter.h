#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// DxMeshWriter — 将顶点+索引数据写入 .dxmesh 二进制文件
//
// 用于将程序化几何体（GeometryGenerator）导为标准 .dxmesh 格式，
// 后续可由 DxMeshLoader 加载。GameWorld 在创建程序化 Mesh 时调用一次，
// 后续运行直接从文件加载。
//
// 用法：
//   DxMeshWriter::Write(vertices, vertexStride, indices, indexSize, path);
// ============================================================================

namespace DX12Engine::Asset {

class DxMeshWriter {
public:
    /// 写入 .dxmesh 文件
    /// @param vertices     顶点数据指针
    /// @param vertexCount  顶点数
    /// @param vertexStride 单顶点字节数（Static=44, Skinned=64）
    /// @param indices      索引数据指针
    /// @param indexCount   索引数
    /// @param indexSize    单索引字节数（2 或 4）
    /// @param outputPath   输出文件路径
    /// @param boundsMin    AABB 最小值（可选，nullptr 则从顶点计算）
    /// @param boundsMax    AABB 最大值（可选，nullptr 则从顶点计算）
    /// @param skinned      是否蒙皮顶点
    /// @return true 成功，false 失败
    static bool Write(const void *vertices, size_t vertexCount, size_t vertexStride, const void *indices,
                      size_t indexCount, uint32_t indexSize, const std::wstring &outputPath,
                      const float *boundsMin = nullptr, const float *boundsMax = nullptr, bool skinned = false);
};

} // namespace DX12Engine::Asset
