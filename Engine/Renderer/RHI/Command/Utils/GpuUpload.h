#pragma once

#include "Background/BackgroundExecutor.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Asset/IO/AssetLoader.h"
#include "Asset/IO/Loader/DDSLoader.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Core/GpuHandlePool.h"
#include <DirectXMath.h>
#include <d3d12.h>
#include <vector>

namespace DX12Engine::Async {

// ========================================================================
// GPU 上传工具函数
//
// 职责边界：只录制命令，不涉及分配器获取、围栏管理、提交。
// 所有 ID3D12GraphicsCommandList 在传入前必须已 Reset 且尚未 Close。
// 调用方负责分配器、命令列表、上传缓冲区的生命周期管理。
// ========================================================================

// ========================================================================
// 旧式地形辅助工具函数（仅 TerrainLoadTask 使用）
//
// 职责：从文件加载纹理 + 创建 GPU 资源，上传顶点/索引数据到 UPLOAD 堆。
// 新增代码请使用 CmdTextureUpload.h 中的标准录制函数。
// ========================================================================

// ── 纹理加载结果 ──
struct TextureLoadResult {
    Resource::GpuResourceHandle gpuHandle;
    D3D12_RESOURCE_DESC desc;
    ID3D12Resource *texResource = nullptr;
    Resource::DDSTextureInfo ddsInfo;
    bool ok = false;
};

/**
 * @brief 从文件加载 DDS 纹理并创建 GPU 资源（DEFAULT 堆，COMMON 状态）
 * @param path DDS 文件路径
 * @param device D3D12 设备
 * @return TextureLoadResult（检查 .ok 判断是否成功）
 */
inline TextureLoadResult LoadTextureFromFileAndCreateGpuResource(const std::wstring &path, ID3D12Device *device) {
    TextureLoadResult result;
    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

    if (!Resource::AssetLoader::GetInstance().LoadTextureFromFile(path, result.ddsInfo)) {
        return result;
    }

    auto gpuHandle = gpuMgr.CreateTexture2D(device, result.ddsInfo.desc, path.c_str(), D3D12_RESOURCE_STATE_COMMON);
    if (!gpuHandle.IsValid()) {
        return result;
    }

    result.gpuHandle = gpuHandle;
    result.desc = result.ddsInfo.desc;
    result.texResource = gpuMgr.GetResource(gpuHandle);
    result.ok = true;
    return result;
}

/**
 * @brief 将 CPU 顶点/索引数据上传到 GPU（创建 UPLOAD 堆 VB/IB）
 * @return true 成功，false 失败
 */
inline bool UploadGeometryToGPU(ID3D12Device *device, const void *vertexData, size_t vertexCount, uint32_t vertexStride,
                                const void *indexData, uint32_t indexCount, Resource::GpuResourceHandle &outVB,
                                Resource::GpuResourceHandle &outIB) {
    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

    size_t vbSize = vertexCount * vertexStride;
    outVB =
        gpuMgr.CreateBuffer(device, vbSize, L"Upload_VB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (auto *res = gpuMgr.GetResource(outVB)) {
        void *mapped = nullptr;
        res->Map(0, nullptr, &mapped);
        memcpy(mapped, vertexData, vbSize);
        res->Unmap(0, nullptr);
    } else {
        return false;
    }

    size_t ibSize = indexCount * sizeof(uint32_t);
    outIB =
        gpuMgr.CreateBuffer(device, ibSize, L"Upload_IB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (auto *res = gpuMgr.GetResource(outIB)) {
        void *mapped = nullptr;
        res->Map(0, nullptr, &mapped);
        memcpy(mapped, indexData, ibSize);
        res->Unmap(0, nullptr);
    } else {
        return false;
    }

    return true;
}

} // namespace DX12Engine::Async
