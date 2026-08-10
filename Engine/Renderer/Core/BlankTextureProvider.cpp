#include "BlankTextureProvider.h"

#include "Common/d3dUtil.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Core/GpuHandlePool.h" // GpuResourceHandle（定义于此；Struct/GpuResourceHandle.h 不存在）
#include "Resource/GpuResourceManager.h"
#include <DirectXMath.h>
#include <vector>

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

// ========================================================================
// 单例实现
// ========================================================================

BlankTextureProvider &BlankTextureProvider::GetInstance() {
    static BlankTextureProvider instance;
    return instance;
}

// ========================================================================
// 生命周期
// ========================================================================

void BlankTextureProvider::Initialize(ID3D12Device *device, DescriptorHeapCollection *descHeaps, CommandManager *cmdMgr,
                                      Resource::HeapTag heapTag) {
    if (m_initialized)
        return;
    m_device = device;
    if (!device || !descHeaps || !cmdMgr) {
        m_initialized = false;
        return;
    }

    CreateWhite2D(device, descHeaps, cmdMgr, heapTag);
    CreateBlackCube(device, descHeaps, cmdMgr, heapTag);

    m_initialized = (m_white2DSRV.ptr != 0 && m_blackCubeSRV.ptr != 0);
}

void BlankTextureProvider::Shutdown() {
    auto &gpuMgr = GpuResourceManager::GetInstance();
    if (m_whiteTexHandle.IsValid()) {
        gpuMgr.Release(m_whiteTexHandle, 0);
        m_whiteTexHandle = {};
    }
    if (m_blackCubeHandle.IsValid()) {
        gpuMgr.Release(m_blackCubeHandle, 0);
        m_blackCubeHandle = {};
    }
    m_whiteSrvSlot = UINT32_MAX;
    m_blackCubeSrvSlot = UINT32_MAX;
    m_white2DSRV = {};
    m_blackCubeSRV = {};
    m_device = nullptr;
    m_initialized = false;
}

// ========================================================================
// 白色 2D 纹理（1x1 R8G8B8A8_UNORM 0xFFFFFFFF）
// ========================================================================

void BlankTextureProvider::CreateWhite2D(ID3D12Device *device, DescriptorHeapCollection *descHeaps,
                                         CommandManager *cmdMgr, Resource::HeapTag heapTag) {
    auto &gpuMgr = GpuResourceManager::GetInstance();

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    m_whiteTexHandle =
        gpuMgr.CreateTexture2D(device, desc, L"BlankTextureProvider_White2D", D3D12_RESOURCE_STATE_COPY_DEST);
    if (!m_whiteTexHandle.IsValid())
        return;

    ID3D12Resource *whiteRes = gpuMgr.GetResource(m_whiteTexHandle);
    if (!whiteRes) {
        gpuMgr.Release(m_whiteTexHandle, 0);
        m_whiteTexHandle = {};
        return;
    }

    // 上传 1x1 白色像素（同步阻塞至 GPU 完成）
    uint32_t whitePixel = 0xFFFFFFFFu;
    D3D12_SUBRESOURCE_DATA subData = {};
    subData.pData = &whitePixel;
    subData.RowPitch = 4;
    subData.SlicePitch = 4;

    UINT64 uploadSize = GetRequiredIntermediateSize(whiteRes, 0, 1);
    GpuResourceHandle uploadBuf = gpuMgr.CreateBuffer(device, (uint32_t)uploadSize, L"BlankTextureProvider_White2D_Up",
                                                      D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!uploadBuf.IsValid()) {
        gpuMgr.Release(m_whiteTexHandle, 0);
        m_whiteTexHandle = {};
        return;
    }

    {
        const uint64_t completedFence = cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
        auto allocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
        auto *alloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
        auto cmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
        auto cmdList = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);

        UpdateSubresources(cmdList.Get(), whiteRes, gpuMgr.GetResource(uploadBuf), 0, 0, 1, &subData);

        // COPY_DEST → PIXEL_SHADER_RESOURCE（SRV 采样）
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            whiteRes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList.Get()->ResourceBarrier(1, &barrier);
        cmdList.Close();

        cmdMgr->Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList);
        cmdMgr->Flush(D3D12_COMMAND_LIST_TYPE_DIRECT); // 同步阻塞

        const uint64_t seq = cmdMgr->GetNextSequence();
        cmdMgr->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
        gpuMgr.Release(uploadBuf, seq);
    }

    // 分配 SRV 槽位（Texture 分区，使用调用方 heapTag——编辑器 EditorViewport 堆）
    m_whiteSrvSlot = descHeaps->Allocate(heapTag, PartitionType::Texture);
    if (m_whiteSrvSlot == UINT32_MAX)
        return;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuH =
        descHeaps->GetPartitionCpuHandle(PartitionType::Texture, m_whiteSrvSlot, heapTag);
    device->CreateShaderResourceView(whiteRes, &srvDesc, cpuH);

    m_white2DSRV = descHeaps->GetPartitionGpuHandle(PartitionType::Texture, m_whiteSrvSlot, heapTag);
}

// ========================================================================
// 黑色 Cubemap（1x1x6 R8G8B8A8_UNORM 全 0x00）
// ========================================================================

void BlankTextureProvider::CreateBlackCube(ID3D12Device *device, DescriptorHeapCollection *descHeaps,
                                           CommandManager *cmdMgr, Resource::HeapTag heapTag) {
    auto &gpuMgr = GpuResourceManager::GetInstance();

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1;
    desc.Height = 1;
    desc.DepthOrArraySize = 6; // Cubemap 六面
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    m_blackCubeHandle =
        gpuMgr.CreateTexture2D(device, desc, L"BlankTextureProvider_BlackCube", D3D12_RESOURCE_STATE_COPY_DEST);
    if (!m_blackCubeHandle.IsValid())
        return;

    ID3D12Resource *cubeRes = gpuMgr.GetResource(m_blackCubeHandle);
    if (!cubeRes) {
        gpuMgr.Release(m_blackCubeHandle, 0);
        m_blackCubeHandle = {};
        return;
    }

    // 六面黑色像素
    uint32_t blackPixels[6] = {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u};
    std::vector<D3D12_SUBRESOURCE_DATA> subDatas(6);
    for (int i = 0; i < 6; ++i) {
        subDatas[i].pData = &blackPixels[i];
        subDatas[i].RowPitch = 4;
        subDatas[i].SlicePitch = 4;
    }

    UINT64 uploadSize = GetRequiredIntermediateSize(cubeRes, 0, 6);
    GpuResourceHandle uploadBuf =
        gpuMgr.CreateBuffer(device, (uint32_t)uploadSize, L"BlankTextureProvider_BlackCube_Up", D3D12_HEAP_TYPE_UPLOAD,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!uploadBuf.IsValid()) {
        gpuMgr.Release(m_blackCubeHandle, 0);
        m_blackCubeHandle = {};
        return;
    }

    {
        const uint64_t completedFence = cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
        auto allocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
        auto *alloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
        auto cmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
        auto cmdList = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);

        UpdateSubresources(cmdList.Get(), cubeRes, gpuMgr.GetResource(uploadBuf), 0, 0, 6, subDatas.data());

        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            cubeRes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList.Get()->ResourceBarrier(1, &barrier);
        cmdList.Close();

        cmdMgr->Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList);
        cmdMgr->Flush(D3D12_COMMAND_LIST_TYPE_DIRECT); // 同步阻塞

        const uint64_t seq = cmdMgr->GetNextSequence();
        cmdMgr->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
        gpuMgr.Release(uploadBuf, seq);
    }

    m_blackCubeSrvSlot = descHeaps->Allocate(heapTag, PartitionType::Texture);
    if (m_blackCubeSrvSlot == UINT32_MAX)
        return;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.TextureCube.MipLevels = 1;
    srvDesc.TextureCube.MostDetailedMip = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuH =
        descHeaps->GetPartitionCpuHandle(PartitionType::Texture, m_blackCubeSrvSlot, heapTag);
    device->CreateShaderResourceView(cubeRes, &srvDesc, cpuH);

    m_blackCubeSRV = descHeaps->GetPartitionGpuHandle(PartitionType::Texture, m_blackCubeSrvSlot, heapTag);
}

} // namespace DX12Engine::Renderer
