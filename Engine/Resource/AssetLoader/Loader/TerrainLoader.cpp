#include "TerrainLoader.h"
#include <DirectXMath.h>
#include <cmath>
#include <cstring>

using namespace DirectX;

#define STB_IMAGE_IMPLEMENTATION
#include "ThirdParty/stb/stb_image.h"

namespace DX12Engine::Resource {

static float SampleHeight(const uint8_t *data, int x, int z, int width, int height, float maxHeight) {
    if (!data || x < 0 || x >= width || z < 0 || z >= height) {
        return 0.0f;
    }

    // 灰度图：只有一个通道
    uint8_t gray = data[z * width + x];
    return (gray / 255.0f) * maxHeight;
}

static void ComputeNormals(TerrainMeshData &meshData) {
    uint32_t numVertices = (uint32_t)meshData.vertices.size();
    if (numVertices == 0)
        return;

    // 初始化法线数组
    std::vector<DirectX::XMFLOAT3> normals(numVertices, DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f));

    // 遍历每个四边形面片（每 4 个索引一组），用两个三角形累加法线
    // 索引顺序: i0=左上(0,0), i1=右上(1,0), i2=左下(0,1), i3=右下(1,1)
    for (size_t i = 0; i + 3 < meshData.indices.size(); i += 4) {
        uint32_t i0 = meshData.indices[i];      // 左上 (0,0)
        uint32_t i1 = meshData.indices[i + 1];  // 右上 (1,0)
        uint32_t i2 = meshData.indices[i + 2];  // 左下 (0,1)
        uint32_t i3 = meshData.indices[i + 3];  // 右下 (1,1)

        DirectX::XMVECTOR p0 = XMLoadFloat3(&meshData.vertices[i0].Position);
        DirectX::XMVECTOR p1 = XMLoadFloat3(&meshData.vertices[i1].Position);
        DirectX::XMVECTOR p2 = XMLoadFloat3(&meshData.vertices[i2].Position);
        DirectX::XMVECTOR p3 = XMLoadFloat3(&meshData.vertices[i3].Position);

        // 三角形 1: i0-i1-i3 (左上-右上-右下)
        DirectX::XMVECTOR e01 = XMVectorSubtract(p1, p0);
        DirectX::XMVECTOR e03 = XMVectorSubtract(p3, p0);
        DirectX::XMVECTOR fn1 = XMVector3Normalize(XMVector3Cross(e01, e03));

        // 三角形 2: i0-i3-i2 (左上-右下-左下)
        DirectX::XMVECTOR e02 = XMVectorSubtract(p2, p0);
        DirectX::XMVECTOR fn2 = XMVector3Normalize(XMVector3Cross(e03, e02));

        DirectX::XMFLOAT3 f1, f2;
        XMStoreFloat3(&f1, fn1);
        XMStoreFloat3(&f2, fn2);

        auto addNormal = [&](uint32_t idx, const DirectX::XMFLOAT3 &fn) {
            normals[idx].x += fn.x;
            normals[idx].y += fn.y;
            normals[idx].z += fn.z;
        };

        // 三角形 1: i0-i1-i3 的顶点法线累加
        addNormal(i0, f1);
        addNormal(i1, f1);
        addNormal(i3, f1);
        // 三角形 2: i0-i3-i2 的顶点法线累加
        addNormal(i0, f2);
        addNormal(i3, f2);
        addNormal(i2, f2);
    }

    // 归一化顶点法线
    for (uint32_t i = 0; i < numVertices; ++i) {
        DirectX::XMVECTOR n = XMLoadFloat3(&normals[i]);
        n = XMVector3Normalize(n);
        XMStoreFloat3(&meshData.vertices[i].Normal, n);
    }
}

bool TerrainLoader::LoadFromPNG(const uint8_t *heightmapData, size_t dataSize, float width, float depth,
                                float maxHeight, TerrainMeshData &outMesh) {
    // 默认使用 257x257 分段（256x256 网格）
    return LoadFromPNG(heightmapData, dataSize, width, depth, maxHeight, 257, outMesh);
}

bool TerrainLoader::LoadFromPNG(const uint8_t *heightmapData, size_t dataSize, float width, float depth,
                                float maxHeight, uint32_t segments, TerrainMeshData &outMesh) {
    if (!heightmapData || dataSize == 0 || segments < 2) {
        return false;
    }

    // 1. 解码 PNG
    int imgWidth, imgHeight, channels;
    stbi_uc *imageData = stbi_load_from_memory(heightmapData, (int)dataSize, &imgWidth, &imgHeight, &channels, 1);
    if (!imageData) {
        return false;
    }

    // 2. 计算网格参数
    uint32_t numCols = segments; // X 方向顶点数
    uint32_t numRows = segments; // Z 方向顶点数
    uint32_t vertexCount = numCols * numRows;

    // 四边形面片：每个 quad 4 个索引
    uint32_t quadCount = (numCols - 1) * (numRows - 1);

    outMesh.vertices.resize(vertexCount);
    outMesh.indices.resize(quadCount * 4);
    outMesh.widthSegments = numCols - 1; // 分段数 = 顶点数 - 1
    outMesh.heightSegments = numRows - 1;
    outMesh.width = width;
    outMesh.depth = depth;
    outMesh.maxHeight = maxHeight;

    // 3. 生成顶点位置和纹理坐标
    float halfWidth = width * 0.5f;
    float halfDepth = depth * 0.5f;
    float dx = width / (numCols - 1);
    float dz = depth / (numRows - 1);
    float du = 1.0f / (numCols - 1);
    float dv = 1.0f / (numRows - 1);

    for (uint32_t row = 0; row < numRows; ++row) {
        float z = halfDepth - row * dz; // Z 从正到负
        int imgY = (int)((float)row / (numRows - 1) * (imgHeight - 1));
        imgY = std::max(0, std::min(imgY, imgHeight - 1));

        for (uint32_t col = 0; col < numCols; ++col) {
            float x = -halfWidth + col * dx;
            int imgX = (int)((float)col / (numCols - 1) * (imgWidth - 1));
            imgX = std::max(0, std::min(imgX, imgWidth - 1));

            uint32_t idx = row * numCols + col;

            // 高度采样
            float y = SampleHeight(imageData, imgX, imgY, imgWidth, imgHeight, maxHeight);

            outMesh.vertices[idx].Position = DirectX::XMFLOAT3(x, y, z);
            outMesh.vertices[idx].TexC = DirectX::XMFLOAT2(col * du, row * dv);
            // 法线和切线稍后计算
            outMesh.vertices[idx].Normal = DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f);
            outMesh.vertices[idx].TangentU = DirectX::XMFLOAT3(1.0f, 0.0f, 0.0f);
        }
    }

    // 4. 生成四边形面片索引（D3D quad domain 期望的顺序: 左上→右上→左下→右下）
    //   i0(u=0,v=0)  i1(u=1,v=0)
    //        +-----------+
    //        |           |
    //        |           |
    //        +-----------+
    //   i2(u=0,v=1)  i3(u=1,v=1)
    uint32_t idx = 0;
    for (uint32_t row = 0; row < numRows - 1; ++row) {
        for (uint32_t col = 0; col < numCols - 1; ++col) {
            uint32_t i0 = row * numCols + col;               // 左上
            uint32_t i1 = row * numCols + col + 1;           // 右上
            uint32_t i2 = (row + 1) * numCols + col;         // 左下
            uint32_t i3 = (row + 1) * numCols + col + 1;     // 右下

            outMesh.indices[idx++] = i0; // 左上 (u=0, v=0)
            outMesh.indices[idx++] = i1; // 右上 (u=1, v=0)
            outMesh.indices[idx++] = i2; // 左下 (u=0, v=1)
            outMesh.indices[idx++] = i3; // 右下 (u=1, v=1)
        }
    }

    // 5. 计算法线（四边形面片：每 4 个索引一组）
    ComputeNormals(outMesh);

    // 6. 清理 stb 图像数据
    stbi_image_free(imageData);

    return true;
}

} // namespace DX12Engine::Resource