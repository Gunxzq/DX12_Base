#pragma once

#include "Background/BackgroundExecutor.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Resource/AssetLoader/AssetLoader.h"
#include "Resource/AssetLoader/Loader/DDSLoader.h"
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
// 纹理加载结果 — 从文件加载 DDS + 创建 GPU 资源后的结果
// ========================================================================
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

// ========================================================================
// 纹理上传录制 — 输入已准备就绪的纹理资源和命令列表，只录制命令
// ========================================================================

/**
 * @brief 单纹理上传的录制参数
 */
struct TextureUploadRecord {
    ID3D12Resource *texResource = nullptr;          // 当前处于 COMMON 状态的 GPU 纹理
    const Resource::DDSTextureInfo *ddsInfo = nullptr; // DDS subresource 数据
    UINT64 uploadOffset = 0;                        // uploadBuffer 中的偏移
};

/**
 * @brief 为单张纹理录制 COPY 命令（COPY 队列）
 *
 * 录制内容：
 *   COMMON → COPY_DEST
 *   UpdateSubresources
 *   COPY_DEST → COMMON
 *
 * @param copyCmdList 已 Reset 的 COPY 命令列表
 * @param copyCmdList 已 Reset 的 COPY 命令列表
 * @param texResource 纹理资源（当前 COMMON 状态）
 * @param uploadBuffer 上传缓冲区
 * @param uploadOffset uploadBuffer 中的起始偏移
 * @param ddsInfo DDS subresource 数据
 */
inline void RecordCopyTexture(ID3D12GraphicsCommandList *copyCmdList,
                              ID3D12Resource *texResource,
                              ID3D12Resource *uploadBuffer,
                              UINT64 uploadOffset,
                              const Resource::DDSTextureInfo &ddsInfo) {
    // COMMON → COPY_DEST
    auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
        texResource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    copyCmdList->ResourceBarrier(1, &toCopy);

    UpdateSubresources(copyCmdList, texResource, uploadBuffer, uploadOffset, 0,
                       static_cast<UINT>(ddsInfo.subresources.size()),
                       const_cast<D3D12_SUBRESOURCE_DATA *>(ddsInfo.subresources.data()));

    // COPY_DEST → COMMON
    auto back = CD3DX12_RESOURCE_BARRIER::Transition(
        texResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    copyCmdList->ResourceBarrier(1, &back);
}

/**
 * @brief 为单张纹理录制状态转换命令（DIRECT 队列）
 *
 * 录制内容：
 *   COMMON → PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE
 *
 * @param directCmdList 已 Reset 的 DIRECT 命令列表
 * @param texResource 纹理资源（当前 COMMON 状态）
 */
inline void RecordTransitionToSRV(ID3D12GraphicsCommandList *directCmdList,
                                  ID3D12Resource *texResource) {
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        texResource, D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    directCmdList->ResourceBarrier(1, &barrier);
}

/**
 * @brief 录制完整纹理上传（COPY + DIRECT 两段）
 *
 * COPY 队列：
 *   对每张纹理: COMMON → COPY_DEST → UpdateSubresources → COPY_DEST → COMMON
 * DIRECT 队列：
 *   对所有纹理统一: COMMON → PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE
 *
 * @param copyCmdList  已 Reset 的 COPY 命令列表（录制后不 Close）
 * @param directCmdList 已 Reset 的 DIRECT 命令列表（录制后不 Close）
 * @param uploadBuffer  上传缓冲区
 * @param textures      纹理录制参数数组
 * @param count         纹理数量
 * @param barriers      用于 DIRECT 屏障的临时数组（外部提供避免栈分配）
 * @param maxBarriers   barriers 数组容量
 */
inline void RecordTextureUploadBatch(ID3D12GraphicsCommandList *copyCmdList,
                                     ID3D12GraphicsCommandList *directCmdList,
                                     ID3D12Resource *uploadBuffer,
                                     const TextureUploadRecord *textures,
                                     uint32_t count,
                                     D3D12_RESOURCE_BARRIER *barriers,
                                     uint32_t maxBarriers) {
    // ── COPY 队列：逐纹理录制（copyCmdList 为 nullptr 时跳过） ──
    if (copyCmdList) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto &tex = textures[i];
            if (!tex.texResource || !tex.ddsInfo)
                continue;

            RecordCopyTexture(copyCmdList, tex.texResource, uploadBuffer,
                              tex.uploadOffset, *tex.ddsInfo);
        }
    }

    // ── DIRECT 队列：批量状态转换（directCmdList 为 nullptr 时跳过） ──
    if (directCmdList) {
        uint32_t barrierCount = 0;
        for (uint32_t i = 0; i < count && barrierCount < maxBarriers; ++i) {
            const auto &tex = textures[i];
            if (!tex.texResource)
                continue;
            barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
                tex.texResource, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (barrierCount > 0)
            directCmdList->ResourceBarrier(barrierCount, barriers);
    }
}

/**
 * @brief 单纹理版本（实际调用批量版，count=1）
 */
inline void RecordTextureUpload(ID3D12GraphicsCommandList *copyCmdList,
                                ID3D12GraphicsCommandList *directCmdList,
                                ID3D12Resource *texResource,
                                ID3D12Resource *uploadBuffer,
                                UINT64 uploadOffset,
                                const Resource::DDSTextureInfo &ddsInfo) {
    TextureUploadRecord record;
    record.texResource = texResource;
    record.ddsInfo = &ddsInfo;
    record.uploadOffset = uploadOffset;

    D3D12_RESOURCE_BARRIER singleBarrier[1];
    RecordTextureUploadBatch(copyCmdList, directCmdList, uploadBuffer, &record, 1,
                             singleBarrier, 1);
}

} // namespace DX12Engine::Async
