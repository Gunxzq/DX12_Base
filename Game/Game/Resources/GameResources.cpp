#include "GameResources.h"
#include "Boot/GameContext.h"
#include "ECS/Core/Registry.h"
#include "Scene/SceneManager.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/RHI/Command/Fence/FenceManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Texture/TextureManager.h"
#include <DirectXMath.h>

using namespace DX12Engine;
using namespace DX12Engine::Resource;

// ========================================================================
// 初始化
// ========================================================================

void GameResources::Initialize(Boot::GameContext *context) {
    m_context = context;

    // ====================================================================
    // 创建 1x1 纯白纹理（反射测试立方体用）
    // ====================================================================
    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Device *device = m_context->DeviceContext->GetDevice();

    D3D12_RESOURCE_DESC whiteDesc = {};
    whiteDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    whiteDesc.Width = 1;
    whiteDesc.Height = 1;
    whiteDesc.DepthOrArraySize = 1;
    whiteDesc.MipLevels = 1;
    whiteDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    whiteDesc.SampleDesc.Count = 1;
    whiteDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    whiteDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    GpuResourceHandle whiteTexHandle =
        gpuMgr.CreateTexture2D(device, whiteDesc, L"WhiteTexture", D3D12_RESOURCE_STATE_COMMON);
    if (whiteTexHandle.IsValid()) {
        uint32_t whiteSrvSlot = m_context->DescriptorHeaps->Allocate(PartitionType::Texture);
        if (whiteSrvSlot != UINT32_MAX) {
            uint32_t whitePixel = 0xFFFFFFFFu;
            D3D12_SUBRESOURCE_DATA subData = {};
            subData.pData = &whitePixel;
            subData.RowPitch = 4;
            subData.SlicePitch = 4;

            uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
            auto allocHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
            auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle);
            auto cmdHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
            auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdHandle);

            UINT64 uploadSize = GetRequiredIntermediateSize(gpuMgr.GetResource(whiteTexHandle), 0, 1);
            GpuResourceHandle uploadBuf =
                gpuMgr.CreateBuffer(device, uploadSize, L"WhiteTexture_Upload", D3D12_HEAP_TYPE_UPLOAD,
                                    D3D12_RESOURCE_STATE_GENERIC_READ);

            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                gpuMgr.GetResource(whiteTexHandle), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList.Get()->ResourceBarrier(1, &barrier);

            UpdateSubresources(cmdList.Get(), gpuMgr.GetResource(whiteTexHandle), gpuMgr.GetResource(uploadBuf), 0,
                               0, 1, &subData);

            auto barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(
                gpuMgr.GetResource(whiteTexHandle), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList.Get()->ResourceBarrier(1, &barrier2);

            cmdList.Close();
            m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList);
            m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);

            uint64_t seq = m_context->GetNextSequence();
            gpuMgr.Release(uploadBuf, seq);
            m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdHandle);
            m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle, seq);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            srvDesc.Texture2D.MostDetailedMip = 0;

            D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
                m_context->DescriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, whiteSrvSlot);
            device->CreateShaderResourceView(gpuMgr.GetResource(whiteTexHandle), &srvDesc, cpuHandle);

            m_whiteTextureHandle = m_context->TextureMgr->RegisterTexture(whiteTexHandle, whiteSrvSlot);
            m_whiteTextureSrvSlot = whiteSrvSlot;
        }
    }

    // ====================================================================
    // 预触 entt 组件存储池——确保 Worker 线程首次 view<> 时不与主线程竞态
    // ====================================================================
    auto *registry = m_context->SceneMgr->GetRegistry();
    if (registry) {
        registry->view<ECS::MeshComponent>();
        registry->view<ECS::TransformComponent>();
        registry->view<ECS::OpaqueTag>();
        registry->view<ECS::TransparentTag>();
        registry->view<ECS::SkinnedTag>();
        registry->view<ECS::SkinnedComponent>();
        registry->view<ECS::TerrainComponent>();
        registry->view<ECS::BillboardComponent>();
        registry->view<ECS::WaterComponent>();
    }
}