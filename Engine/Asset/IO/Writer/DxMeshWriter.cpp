#include "DxMeshWriter.h"
#include "Asset/Definitions/Mesh/DxMeshFormat.h"
#include <filesystem>
#include <fstream>

namespace DX12Engine::Asset {

bool DxMeshWriter::Write(const void *vertices, size_t vertexCount, size_t vertexStride, const void *indices,
                         size_t indexCount, uint32_t indexSize, const std::wstring &outputPath, const float *boundsMin,
                         const float *boundsMax, bool skinned, const DxMeshSubMesh *subMeshes, uint32_t subMeshCount) {
    if (!vertices || vertexCount == 0 || !indices || indexCount == 0) {
        return false;
    }
    if (indexSize != 2 && indexSize != 4) {
        return false;
    }

    // 确保输出目录存在
    std::filesystem::path outPath(outputPath);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);

    // 计算 AABB
    float minBounds[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float maxBounds[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    if (boundsMin && boundsMax) {
        memcpy(minBounds, boundsMin, sizeof(minBounds));
        memcpy(maxBounds, boundsMax, sizeof(maxBounds));
    } else if (vertexStride >= 12) {
        const auto *verts = static_cast<const uint8_t *>(vertices);
        for (size_t i = 0; i < vertexCount; ++i) {
            const float *pos = reinterpret_cast<const float *>(verts + i * vertexStride);
            minBounds[0] = (std::min)(minBounds[0], pos[0]);
            minBounds[1] = (std::min)(minBounds[1], pos[1]);
            minBounds[2] = (std::min)(minBounds[2], pos[2]);
            maxBounds[0] = (std::max)(maxBounds[0], pos[0]);
            maxBounds[1] = (std::max)(maxBounds[1], pos[1]);
            maxBounds[2] = (std::max)(maxBounds[2], pos[2]);
        }
    }

    // 计算文件偏移
    uint32_t flags = skinned ? DxMeshFlag_Skinned : 0;
    if (indexSize == 2)
        flags |= DxMeshFlag_Index16;

    uint32_t vertexDataSize = static_cast<uint32_t>(vertexCount * vertexStride);
    uint32_t indexDataSize = static_cast<uint32_t>(indexCount * indexSize);
    uint32_t lodTableSize = sizeof(DxMeshLOD) * 1; // 当前固定单 LOD
    uint32_t lodTableOffset = static_cast<uint32_t>(sizeof(DxMeshHeader) + vertexDataSize + indexDataSize);
    uint32_t subMeshTableOffset = lodTableOffset + lodTableSize;

    // 填充 Header
    DxMeshHeader header = {};
    memcpy(header.magic, DX_MESH_MAGIC, 8);
    header.version = DX_MESH_VERSION;
    header.vertexCount = static_cast<uint32_t>(vertexCount);
    header.indexCount = static_cast<uint32_t>(indexCount);
    header.vertexStride = static_cast<uint32_t>(vertexStride);
    header.flags = flags;
    header.indexSize = indexSize;
    memcpy(header.boundsMin, minBounds, sizeof(minBounds));
    memcpy(header.boundsMax, maxBounds, sizeof(maxBounds));
    header.lodCount = 1;
    header.lodOffset = lodTableOffset;
    header.subMeshOffset = subMeshTableOffset;
    header.subMeshCount = (subMeshes != nullptr) ? subMeshCount : 1;

    // 写文件
    std::ofstream ofs(outPath, std::ios::binary);
    if (!ofs.is_open()) {
        return false;
    }

    // 1. Header
    ofs.write(reinterpret_cast<const char *>(&header), sizeof(header));

    // 2. 顶点数据
    ofs.write(static_cast<const char *>(vertices), vertexDataSize);

    // 3. 索引数据
    ofs.write(static_cast<const char *>(indices), indexDataSize);

    // 4. LOD 表（单 LOD）
    DxMeshLOD lod = {};
    lod.vertexOffset = 0;
    lod.vertexCount = header.vertexCount;
    lod.indexOffset = 0;
    lod.indexCount = header.indexCount;
    lod.errorMetric = 0.0f;
    ofs.write(reinterpret_cast<const char *>(&lod), sizeof(lod));

    // 5. SubMesh 表
    if (subMeshes != nullptr && subMeshCount > 0) {
        ofs.write(reinterpret_cast<const char *>(subMeshes), sizeof(DxMeshSubMesh) * subMeshCount);
    } else {
        // 默认：整个网格为一个 SubMesh
        DxMeshSubMesh defaultSub = {};
        defaultSub.indexOffset = 0;
        defaultSub.indexCount = header.indexCount;
        defaultSub.vertexOffset = 0;
        ofs.write(reinterpret_cast<const char *>(&defaultSub), sizeof(defaultSub));
    }

    return true;
}

} // namespace DX12Engine::Asset
