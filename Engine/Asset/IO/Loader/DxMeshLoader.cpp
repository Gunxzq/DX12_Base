#include "DxMeshLoader.h"
#include "Asset/Definitions/Mesh/DxMeshFormat.h"
#include "Resource/Core/GpuHandlePool.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/GpuResourceManager.h"
#include <filesystem>
#include <fstream>
#include <vector>

namespace DX12Engine::Asset {

using namespace DX12Engine::Resource;

bool DxMeshLoader::LoadFromFile(const std::wstring &filePath, ID3D12Device *device, const std::wstring &meshName,
                                TriangleMesh &outMesh) {
    // 1. 读取文件
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }
    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> fileData(fileSize);
    file.read(reinterpret_cast<char *>(fileData.data()), fileSize);
    file.close();

    if (fileData.size() < sizeof(DxMeshHeader)) {
        return false;
    }

    // 2. 解析 Header
    const auto *header = reinterpret_cast<const DxMeshHeader *>(fileData.data());

    // 校验魔数和版本
    if (memcmp(header->magic, DX_MESH_MAGIC, 8) != 0) {
        return false;
    }
    if (header->version != DX_MESH_VERSION) {
        return false;
    }

    uint32_t vertexDataSize = header->vertexCount * header->vertexStride;
    uint32_t indexDataSize = header->indexCount * header->indexSize;

    if (sizeof(DxMeshHeader) + vertexDataSize + indexDataSize > fileSize) {
        return false;
    }

    // 3. 获取数据指针
    const void *vertexData = DxMesh_GetVertexData(header);
    const void *indexData = DxMesh_GetIndexData(header);

    // 4. 创建 GPU VB
    auto &gpuMgr = GpuResourceManager::GetInstance();
    auto vbHandle = gpuMgr.CreateBuffer(device, vertexDataSize, (meshName + L"_VB").c_str(), D3D12_HEAP_TYPE_UPLOAD,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!vbHandle.IsValid()) {
        return false;
    }
    ID3D12Resource *vbResource = gpuMgr.GetResource(vbHandle);
    if (vbResource) {
        void *mapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        vbResource->Map(0, &readRange, &mapped);
        memcpy(mapped, vertexData, vertexDataSize);
        vbResource->Unmap(0, nullptr);
    }

    // 5. 创建 GPU IB
    auto ibHandle = gpuMgr.CreateBuffer(device, indexDataSize, (meshName + L"_IB").c_str(), D3D12_HEAP_TYPE_UPLOAD,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!ibHandle.IsValid()) {
        gpuMgr.Release(vbHandle, ~0ull);
        return false;
    }
    ID3D12Resource *ibResource = gpuMgr.GetResource(ibHandle);
    if (ibResource) {
        void *mapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ibResource->Map(0, &readRange, &mapped);
        memcpy(mapped, indexData, indexDataSize);
        ibResource->Unmap(0, nullptr);
    }

    // 6. 填充 TriangleMesh
    outMesh = {};
    outMesh.vertexBufferHandle = vbHandle;
    outMesh.indexBufferHandle = ibHandle;
    outMesh.vertexCount = header->vertexCount;
    outMesh.indexCount = header->indexCount;
    outMesh.vertexStride = header->vertexStride;
    outMesh.indexFormat = (header->indexSize == 2) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    outMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    outMesh.flags = header->flags;
    outMesh.isGpuReady = true;

    // 设置包围盒
    Math::BoundingAABB aabb;
    aabb.min = DirectX::XMFLOAT3(header->boundsMin[0], header->boundsMin[1], header->boundsMin[2]);
    aabb.max = DirectX::XMFLOAT3(header->boundsMax[0], header->boundsMax[1], header->boundsMax[2]);
    outMesh.localBounds = aabb;

    // 读取 SubMesh 表
    if (header->subMeshCount > 0) {
        const auto *subTable = DxMesh_GetSubMeshTable(header);
        outMesh.subMeshes.reserve(header->subMeshCount);
        for (uint32_t i = 0; i < header->subMeshCount; ++i) {
            SubMeshInfo info;
            info.startIndex = subTable[i].indexOffset;
            info.indexCount = subTable[i].indexCount;
            info.startVertex = static_cast<int32_t>(subTable[i].vertexOffset);
            outMesh.subMeshes.push_back(info);
        }
    }

    return true;
}

} // namespace DX12Engine::Asset
