#include "ThumbnailArray.h"
#include "Common/Common.h"
#include "DebugUI/DebugUIManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include <cassert>
#include <cstring>

using namespace DX12Engine;
using namespace DX12Engine::Renderer;



bool ThumbnailArray::Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descriptorHeaps) {
    if (m_initialized)
        Shutdown();

    if (!device || !descriptorHeaps)
        return false;

    m_device = device;
    m_descriptorHeaps = descriptorHeaps;

    m_capacity = DEFAULT_CAPACITY;
    m_allocatedCount = 0;
    m_baseRtvSlot = UINT32_MAX;

    // ── 创建 TEX2D_ARRAY 资源 ──
    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = SLICE_SIZE;
    resourceDesc.Height = SLICE_SIZE;
    resourceDesc.DepthOrArraySize = m_capacity;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    resourceDesc.SampleDesc = {1, 0};
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clearValue.Color[0] = 0.12f;
    clearValue.Color[1] = 0.12f;
    clearValue.Color[2] = 0.14f;
    clearValue.Color[3] = 1.0f;

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON, &clearValue, IID_PPV_ARGS(&m_textureArray));

    if (FAILED(hr)) {
        m_device = nullptr;
        m_descriptorHeaps = nullptr;
        return false;
    }
    m_textureArray->SetName(L"ThumbnailArray");

    // ── 为所有 slice 分配连续的 RTV 槽位 ──
    m_baseRtvSlot = m_descriptorHeaps->AllocateConsecutive(Resource::PartitionType::Rtv, m_capacity);
    if (m_baseRtvSlot == UINT32_MAX) {
        m_textureArray.Reset();
        m_device = nullptr;
        m_descriptorHeaps = nullptr;
        return false;
    }

    m_rtvHandles.resize(m_capacity);
    for (uint32_t i = 0; i < m_capacity; ++i) {
        m_rtvHandles[i] = m_descriptorHeaps->GetCpuHandle(Resource::PartitionType::Rtv, m_baseRtvSlot + i);

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.ArraySize = 1;
        rtvDesc.Texture2DArray.FirstArraySlice = i;
        rtvDesc.Texture2DArray.MipSlice = 0;
        rtvDesc.Texture2DArray.PlaneSlice = 0;
        m_device->CreateRenderTargetView(m_textureArray.Get(), &rtvDesc, m_rtvHandles[i]);
    }

    // ── 初始化空闲列表（倒序，LIFO） ──
    m_freeSlices.reserve(m_capacity);
    for (uint32_t i = m_capacity; i > 0; --i) {
        m_freeSlices.push_back(i - 1);
    }

    m_initialized = true;
    return true;
}

void ThumbnailArray::Shutdown() {
    if (!m_initialized)
        return;

    m_freeSlices.clear();
    m_rtvHandles.clear();

    // 释放连续分配的 RTV 槽位
    if (m_baseRtvSlot != UINT32_MAX && m_descriptorHeaps) {
        for (uint32_t i = 0; i < m_capacity; ++i) {
            m_descriptorHeaps->Free(Resource::PartitionType::Rtv, m_baseRtvSlot + i, UINT64_MAX);
        }
        m_baseRtvSlot = UINT32_MAX;
    }

    // 释放纹理数组资源
    m_textureArray.Reset();

    m_device = nullptr;
    m_descriptorHeaps = nullptr;
    m_capacity = 0;
    m_allocatedCount = 0;
    m_initialized = false;
}

uint32_t ThumbnailArray::AllocSlice() {
    if (!m_initialized || m_freeSlices.empty())
        return UINT32_MAX;

    uint32_t slice = m_freeSlices.back();
    m_freeSlices.pop_back();
    ++m_allocatedCount;
    return slice;
}

void ThumbnailArray::FreeSlice(uint32_t slice) {
    if (!m_initialized || slice >= m_capacity)
        return;

    m_freeSlices.push_back(slice);
    --m_allocatedCount;
}

D3D12_CPU_DESCRIPTOR_HANDLE ThumbnailArray::GetRtvHandle(uint32_t slice) const {
    if (!m_initialized || slice >= m_rtvHandles.size())
        return {};
    return m_rtvHandles[slice];
}

bool ThumbnailArray::UploadToSlice(uint32_t slice, uint32_t width, uint32_t height, const void *pixels,
                                   D3D12DeviceContext *deviceCtx) {
    if (!m_initialized || slice >= m_capacity || !pixels || !deviceCtx)
        return false;

    auto &cmdMgr = deviceCtx->GetCommandManager();

    // 计算上传所需大小
    UINT rowPitch = width * 4;
    UINT totalSize = rowPitch * height;

    D3D12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(totalSize);
    D3D12_HEAP_PROPERTIES uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
    HRESULT hr = m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&uploadBuffer));
    if (FAILED(hr)) return false;

    // 复制像素数据到上传缓冲区
    void *mapped;
    uploadBuffer->Map(0, nullptr, &mapped);
    uint8_t *dst = static_cast<uint8_t *>(mapped);
    const uint8_t *src = static_cast<const uint8_t *>(pixels);
    for (UINT y = 0; y < height; ++y) {
        memcpy(dst + y * rowPitch, src + y * width * 4, width * 4);
    }
    uploadBuffer->Unmap(0, nullptr);

    // 使用 COPY 队列执行上传
    uint64_t completedFence = cmdMgr.GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_COPY);
    auto allocHandle = cmdMgr.AcquireAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(completedFence);
    auto *alloc = cmdMgr.GetAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(allocHandle);
    auto cmdHandle = cmdMgr.AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_COPY>(alloc);
    auto cmdList = cmdMgr.GetCommandList<D3D12_COMMAND_LIST_TYPE_COPY>(cmdHandle);
    auto *d = cmdList.Get();

    // Transition: COMMON → COPY_DEST
    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_textureArray.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST,
        slice);
    d->ResourceBarrier(1, &barrier);

    // Copy upload buffer → texture array slice
    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = m_textureArray.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = slice;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    footprint.Offset = 0;
    footprint.Footprint.Width = width;
    footprint.Footprint.Height = height;
    footprint.Footprint.Depth = 1;
    footprint.Footprint.RowPitch = rowPitch;
    footprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = uploadBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    d->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition back: COPY_DEST → COMMON (use raw barrier for StateBefore/StateAfter modification)
    D3D12_RESOURCE_BARRIER backBarrier = {};
    backBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    backBarrier.Transition.pResource = m_textureArray.Get();
    backBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    backBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    backBarrier.Transition.Subresource = slice;
    d->ResourceBarrier(1, &backBarrier);

    cmdList.Close();
    cmdMgr.Submit(D3D12_COMMAND_LIST_TYPE_COPY, cmdList);

    // Signal + Wait 确保上传完成才释放 upload buffer
    auto &fenceMgr = cmdMgr.GetFenceManager();
    uint64_t uploadSignal = fenceMgr.GetNextSequence();
    auto *copyQueue = cmdMgr.GetCopyQueue();
    if (copyQueue) {
        fenceMgr.Signal(D3D12_COMMAND_LIST_TYPE_COPY, copyQueue->Get(), uploadSignal);
    }
    fenceMgr.WaitForSequence(D3D12_COMMAND_LIST_TYPE_COPY, uploadSignal);

    cmdMgr.ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(allocHandle, UINT64_MAX);

    return true;
}

ThumbnailArray::ReadbackResult ThumbnailArray::ReadbackSlice(uint32_t slice, D3D12DeviceContext *deviceCtx) {
    ReadbackResult result = {};
    if (!m_initialized || slice >= m_capacity || !deviceCtx)
        return result;

    auto &cmdMgr = deviceCtx->GetCommandManager();
    UINT rowPitch = SLICE_SIZE * 4;
    UINT totalSize = rowPitch * SLICE_SIZE;

    // 创建 readback 缓冲区
    D3D12_RESOURCE_DESC readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(totalSize);
    D3D12_HEAP_PROPERTIES readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);

    Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;
    HRESULT hr = m_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&readbackBuffer));
    if (FAILED(hr)) return result;

    // 使用 COPY 队列执行回读
    uint64_t completedFence = cmdMgr.GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_COPY);
    auto allocHandle = cmdMgr.AcquireAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(completedFence);
    auto *alloc = cmdMgr.GetAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(allocHandle);
    auto cmdHandle = cmdMgr.AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_COPY>(alloc);
    auto cmdList = cmdMgr.GetCommandList<D3D12_COMMAND_LIST_TYPE_COPY>(cmdHandle);
    auto *d = cmdList.Get();

    // Transition: COMMON → COPY_SOURCE
    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_textureArray.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE,
        slice);
    d->ResourceBarrier(1, &barrier);

    // Copy texture array slice → readback buffer
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = m_textureArray.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = slice;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    footprint.Offset = 0;
    footprint.Footprint.Width = SLICE_SIZE;
    footprint.Footprint.Height = SLICE_SIZE;
    footprint.Footprint.Depth = 1;
    footprint.Footprint.RowPitch = rowPitch;
    footprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = readbackBuffer.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = footprint;

    d->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition back: COPY_SOURCE → COMMON
    D3D12_RESOURCE_BARRIER backBarrier = {};
    backBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    backBarrier.Transition.pResource = m_textureArray.Get();
    backBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    backBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    backBarrier.Transition.Subresource = slice;
    d->ResourceBarrier(1, &backBarrier);

    cmdList.Close();
    cmdMgr.Submit(D3D12_COMMAND_LIST_TYPE_COPY, cmdList);

    // Signal + Wait 确保读回完成才映射
    auto &fenceMgr = cmdMgr.GetFenceManager();
    uint64_t copySignal = fenceMgr.GetNextSequence();
    auto *copyQueue = cmdMgr.GetCopyQueue();
    if (copyQueue) {
        fenceMgr.Signal(D3D12_COMMAND_LIST_TYPE_COPY, copyQueue->Get(), copySignal);
    }
    fenceMgr.WaitForSequence(D3D12_COMMAND_LIST_TYPE_COPY, copySignal);

    cmdMgr.ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(allocHandle, UINT64_MAX);

    // 映射 readback 缓冲区读取像素数据
    void *mapped;
    hr = readbackBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) return result;

    auto *pixels = new uint8_t[totalSize];
    uint8_t *srcRow = static_cast<uint8_t *>(mapped);
    for (UINT y = 0; y < SLICE_SIZE; ++y) {
        memcpy(pixels + y * SLICE_SIZE * 4, srcRow + y * rowPitch, SLICE_SIZE * 4);
    }
    readbackBuffer->Unmap(0, nullptr);

    result.width = SLICE_SIZE;
    result.height = SLICE_SIZE;
    result.pixels = pixels;
    return result;
}

void ThumbnailArray::CreateSliceSRV(uint32_t slice, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) {
    if (!m_initialized || slice >= m_capacity || cpuHandle.ptr == 0)
        return;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.ArraySize = 1;
    srvDesc.Texture2DArray.FirstArraySlice = slice;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.PlaneSlice = 0;

    m_device->CreateShaderResourceView(m_textureArray.Get(), &srvDesc, cpuHandle);
}


