#pragma once

#include "Renderer/Utils/GeometryGenerator.h"
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

namespace DX12Engine::Resource {

// 地形网格数据（输出结构）
struct TerrainMeshData {
    std::vector<GeometryGenerator::Vertex> vertices;
    std::vector<uint32_t> indices; // 索引

    // 地形参数
    uint32_t widthSegments = 0;  // 宽度分段数
    uint32_t heightSegments = 0; // 高度分段数
    float width = 0.0f;          // 地形实际宽度（X 轴）
    float depth = 0.0f;          // 地形实际深度（Z 轴）
    float maxHeight = 0.0f;      // 最大高度（Y 轴）
};

// 地形加载器
class TerrainLoader {
public:
    static bool LoadFromPNG(const uint8_t *heightmapData, size_t dataSize, float width, float depth, float maxHeight,
                            TerrainMeshData &outMesh);

    // 便捷方法：指定分段数（默认 257x257）
    static bool LoadFromPNG(const uint8_t *heightmapData, size_t dataSize, float width, float depth, float maxHeight,
                            uint32_t segments, // 分段数（如 257）
                            TerrainMeshData &outMesh);
};

} // namespace DX12Engine::Resource