#pragma once

#include "Asset/IO/Loader/DDSLoader.h"
#include "Resource/Core/GpuHandlePool.h"
#include <d3d12.h>

namespace DX12Engine::Async {

// ========================================================================
// 纹理上传命令录制 — 只录制命令，不涉及分配器获取、围栏管理、提交。
// 所有 ID3D12GraphicsCommandList 在传入前必须已 Reset 且尚未 Close。
// 调用方负责分配器、命令列表、上传缓冲区的生命周期管理。
// ========================================================================

// ── 单纹理上传的录制参数 ──
struct TextureUploadRecord {
    ID3D12Resource *texResource = nullptr;
    const Resource::DDSTextureInfo *ddsInfo = nullptr;
    UINT64 uploadOffset = 0;
};

/**
 * @brief 为单张纹理录制 COPY 命令（COPY 队列）
 *
 * 录制内容：
 *   COMMON → COPY_DEST
 *   UpdateSubresources
 *   COPY_DEST → COMMON
 */
inline void RecordCopyTexture(ID3D12GraphicsCommandList *copyCmdList, ID3D12Resource *texResource,
                              ID3D12Resource *uploadBuffer, UINT64 uploadOffset,
                              const Resource::DDSTextureInfo &ddsInfo) {
    auto toCopy =
        CD3DX12_RESOURCE_BARRIER::Transition(texResource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    copyCmdList->ResourceBarrier(1, &toCopy);

    UpdateSubresources(copyCmdList, texResource, uploadBuffer, uploadOffset, 0,
                       static_cast<UINT>(ddsInfo.subresources.size()),
                       const_cast<D3D12_SUBRESOURCE_DATA *>(ddsInfo.subresources.data()));

    auto back =
        CD3DX12_RESOURCE_BARRIER::Transition(texResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    copyCmdList->ResourceBarrier(1, &back);
}

/**
 * @brief 为单张纹理录制状态转换命令（DIRECT 队列）
 *
 * 录制内容：
 *   COMMON → PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE
 */
inline void RecordTransitionToSRV(ID3D12GraphicsCommandList *directCmdList, ID3D12Resource *texResource) {
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        texResource, D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    directCmdList->ResourceBarrier(1, &barrier);
}

/**
 * @brief 录制完整纹理上传（COPY + DIRECT 两段）
 */
inline void RecordTextureUploadBatch(ID3D12GraphicsCommandList *copyCmdList, ID3D12GraphicsCommandList *directCmdList,
                                     ID3D12Resource *uploadBuffer, const TextureUploadRecord *textures, uint32_t count,
                                     D3D12_RESOURCE_BARRIER *barriers, uint32_t maxBarriers) {
    if (copyCmdList) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto &tex = textures[i];
            if (!tex.texResource || !tex.ddsInfo)
                continue;
            RecordCopyTexture(copyCmdList, tex.texResource, uploadBuffer, tex.uploadOffset, *tex.ddsInfo);
        }
    }

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
inline void RecordTextureUpload(ID3D12GraphicsCommandList *copyCmdList, ID3D12GraphicsCommandList *directCmdList,
                                ID3D12Resource *texResource, ID3D12Resource *uploadBuffer, UINT64 uploadOffset,
                                const Resource::DDSTextureInfo &ddsInfo) {
    TextureUploadRecord record;
    record.texResource = texResource;
    record.ddsInfo = &ddsInfo;
    record.uploadOffset = uploadOffset;

    D3D12_RESOURCE_BARRIER singleBarrier[1];
    RecordTextureUploadBatch(copyCmdList, directCmdList, uploadBuffer, &record, 1, singleBarrier, 1);
}

} // namespace DX12Engine::Async
