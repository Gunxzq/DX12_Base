#pragma once
#include <cstdint>
#include <string>

namespace DX12Engine::Resource {
struct TriangleMesh;
} // namespace DX12Engine::Resource

struct ID3D12Device;

namespace DX12Engine::Asset {

// ============================================================================
// DxMeshLoader — 从 .dxmesh 二进制文件创建 GPU VB/IB
//
// 用法：
//   TriangleMesh mesh;
//   if (DxMeshLoader::LoadFromFile(path, device, L"Cube", mesh)) {
//       GeometryHandle handle = geoMgr->RegisterGeometry<TriangleMesh>(mesh);
//   }
// ============================================================================
class DxMeshLoader {
public:
    /// 从 .dxmesh 文件加载网格，创建 GPU VB/IB
    /// @param filePath   .dxmesh 文件路径
    /// @param device     D3D12 设备
    /// @param meshName   调试名称（如 L"Cube_VB"）
    /// @param outMesh    输出的 TriangleMesh（含 VB/IB handle、顶点/索引计数等）
    /// @return true 成功
    static bool LoadFromFile(const std::wstring &filePath, ID3D12Device *device, const std::wstring &meshName,
                             Resource::TriangleMesh &outMesh);
};

} // namespace DX12Engine::Asset
