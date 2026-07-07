#include "Background/TerrainLoadTask.h"
#include "Boot/GameContext.h"
#include "Core/SharedDataStore/SharedDataStore.h"
#include "ECS/Core/Components.h"
#include "ECS/Core/Registry.h"
#include "Event/EventRegistry.h"
#include "Event/EventTypes.h"
#include "Framework/SystemRegistry.h"
#include "GameWorld.h"
#include "Renderer/Effects/AO/AmbientOcclusionManager.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "Renderer/Material/MaterialManager.h"
#include "Renderer/Pipeline/BillboardRenderer.h"
#include "Renderer/Pipeline/OpaqueRenderer.h"
#include "Renderer/Pipeline/ReflectionProbeRenderer.h"
#include "Renderer/Pipeline/ShadowRenderer.h"
#include "Renderer/Pipeline/SkyRenderer.h"
#include "Renderer/Pipeline/TerrainRenderer.h"
#include "Renderer/Pipeline/WaterRenderer.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/LightManager/LightManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Pool/RenderTargetPool.h"
#include "Resource/Texture/TextureManager.h"
#include "Scheduler/FrameDriver.h"
#include <DirectXMath.h>
#include <d3dcompiler.h>

using namespace DirectX;
using namespace DX12Engine;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::Scheduler;

// ========================================================================
// GameWorld — 渲染 System 注册
// ========================================================================

void GameWorld::RegisterLightingPass() {
    SystemRegistry::Register(
        {.name = "LightingPass",
         .func =
             [this](Registry &, const MessageContext &) {
                 if (!m_lightingRenderer)
                     return;

                 uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                 auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                 auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
                 auto backBuffer = m_context->GetBackBuffer();

                 // 屏障：G-buffer RTs COMMON → PIXEL_SHADER_RESOURCE
                 auto &rtPool = RenderTargetPool::GetInstance();
                 auto barrierToSRV = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_COMMON,
                                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                     cmd.Get()->ResourceBarrier(1, &b);
                 };
                 barrierToSRV(m_appRTs->GetGBufferAlbedoResource());
                 barrierToSRV(m_appRTs->GetGBufferNormalResource());
                 barrierToSRV(m_appRTs->GetGBufferMaterialResource());
                 barrierToSRV(m_appRTs->GetGBufferWorldPosResource());

                 // 屏障：交换链 PRESENT → RENDER_TARGET
                 D3D12_RESOURCE_BARRIER bbBarrier = {};
                 bbBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 bbBarrier.Transition.pResource = backBuffer;
                 bbBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                 bbBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 bbBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmd.Get()->ResourceBarrier(1, &bbBarrier);

                 // 视口和 RT
                 const auto &vp = m_context->DeviceContext->GetViewport();
                 const auto &sr = m_context->DeviceContext->GetScissorRect();
                 cmd.Get()->RSSetViewports(1, &vp);
                 cmd.Get()->RSSetScissorRects(1, &sr);
                 auto rtv = m_context->DeviceContext->GetCurrentBackBufferView();
                 cmd.Get()->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

                 // 设置描述符堆
                 ID3D12DescriptorHeap *heaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 cmd.Get()->SetDescriptorHeaps(1, heaps);

                 // 渲染
                 D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV = {};
                 if (m_skyboxTextureHandle.IsValid()) {
                     envMapSRV = m_context->TextureMgr->GetSRV(m_skyboxTextureHandle);
                 }
                 D3D12_GPU_DESCRIPTOR_HANDLE cubemapArraySRV = m_context->ReflectionProbeMgr->GetProbeCubemapArraySRV();

                 auto &lightMgr = LightManager::GetInstance();
                 D3D12_GPU_DESCRIPTOR_HANDLE shadowDataSRV = lightMgr.GetShadowDataSRV();
                 D3D12_GPU_DESCRIPTOR_HANDLE shadowMapSRV = lightMgr.GetShadowMapSRV();

                 m_lightingRenderer->BeginFrame(cmd, m_context->FrameResourceManager->GetPassCBAddress(),
                                                LightManager::GetInstance().GetLightCBAddress(),
                                                m_appRTs->GetGBufferAlbedoSRV(), m_appRTs->GetGBufferNormalSRV(),
                                                m_appRTs->GetGBufferMaterialSRV(), m_appRTs->GetGBufferWorldPosSRV(),
                                                AmbientOcclusionManager::GetInstance().GetAmbientMapSRV(), envMapSRV,
                                                cubemapArraySRV, shadowDataSRV, shadowMapSRV);
                 m_lightingRenderer->Draw(cmd);
                 m_lightingRenderer->EndFrame();

                 // 屏障：G-buffer RTs PIXEL_SHADER_RESOURCE → COMMON
                 auto barrierToCommon = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                                   D3D12_RESOURCE_STATE_COMMON);
                     cmd.Get()->ResourceBarrier(1, &b);
                 };
                 barrierToCommon(m_appRTs->GetGBufferAlbedoResource());
                 barrierToCommon(m_appRTs->GetGBufferNormalResource());
                 barrierToCommon(m_appRTs->GetGBufferMaterialResource());
                 barrierToCommon(m_appRTs->GetGBufferWorldPosResource());

                 // 屏障：交换链 RENDER_TARGET → PRESENT
                 bbBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 bbBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 cmd.Get()->ResourceBarrier(1, &bbBarrier);

                 cmd.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Lighting, cmdH);
                 uint64_t seq = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Lighting,
         .alwaysRun = true});
}

void GameWorld::RegisterOpaqueRenderSystem() {
    SystemRegistry::Register(
        {.name = "OpaqueRenderSystem",
         .func =
             [this](Registry &, const MessageContext &) {
                 if (!m_renderer || m_opaqueQueue.Empty())
                     return;

                 // G-buffer RT 必须已分配
                 if (!m_appRTs || !m_appRTs->IsInitialized())
                     return;

                 auto &rtPool = RenderTargetPool::GetInstance();

                 uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                 auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                 auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);

                 D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferAlbedo()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferNormal()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferMaterial()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferWorldPos()),
                 };

                 // 资源屏障：COMMON → RENDER_TARGET
                 auto barrierRT = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_COMMON,
                                                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                     cmd.Get()->ResourceBarrier(1, &b);
                 };
                 barrierRT(m_appRTs->GetGBufferAlbedoResource());
                 barrierRT(m_appRTs->GetGBufferNormalResource());
                 barrierRT(m_appRTs->GetGBufferMaterialResource());
                 barrierRT(m_appRTs->GetGBufferWorldPosResource());

                 // 设置视口
                 const auto &vp = m_context->DeviceContext->GetViewport();
                 const auto &sr = m_context->DeviceContext->GetScissorRect();
                 cmd.Get()->RSSetViewports(1, &vp);
                 cmd.Get()->RSSetScissorRects(1, &sr);

                 // 设置描述符堆
                 ID3D12DescriptorHeap *heaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 cmd.Get()->SetDescriptorHeaps(1, heaps);

                 // 绑定 4 个 G-buffer RT + 主深度缓冲
                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();
                 cmd.Get()->OMSetRenderTargets(4, rtvs, FALSE, &dsvHandle);

                 // 清除 RT 和深度缓冲
                 const float clearColor[4] = {0, 0, 0, 0};
                 cmd.Get()->ClearRenderTargetView(rtvs[0], clearColor, 0, nullptr);
                 cmd.Get()->ClearRenderTargetView(rtvs[1], clearColor, 0, nullptr);
                 cmd.Get()->ClearRenderTargetView(rtvs[2], clearColor, 0, nullptr);
                 cmd.Get()->ClearRenderTargetView(rtvs[3], clearColor, 0, nullptr);
                 cmd.Get()->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

                 // G-buffer 渲染（使用 OpaqueRenderer 的 G-buffer 通道）
                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 D3D12_GPU_DESCRIPTOR_HANDLE matSRV = m_context->MaterialMgr->GetMaterialBufferSRV();
                 D3D12_GPU_DESCRIPTOR_HANDLE texHeapStart =
                     m_context->DescriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, 0);

                 m_renderer->BeginFrameGBuffer(cmd, passCBAddr, matSRV, texHeapStart);

                 for (const auto &item : m_opaqueQueue) {
                     if (!item.IsValid())
                         continue;
                     m_renderer->DrawInstancedGBuffer(cmd, item.geometryHandle, item.instanceBuffer, item.instanceCount,
                                                      item.startIndex, item.startVertex, item.indexCount);
                 }

                 m_renderer->EndFrameGBuffer();

                 // 屏障：RENDER_TARGET → COMMON（为下帧准备）
                 auto barrierBack = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                   D3D12_RESOURCE_STATE_COMMON);
                     cmd.Get()->ResourceBarrier(1, &b);
                 };
                 barrierBack(m_appRTs->GetGBufferAlbedoResource());
                 barrierBack(m_appRTs->GetGBufferNormalResource());
                 barrierBack(m_appRTs->GetGBufferMaterialResource());
                 barrierBack(m_appRTs->GetGBufferWorldPosResource());

                 cmd.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Opaque, cmdH);

                 uint64_t seq = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Opaque,
         .alwaysRun = true});
}

void GameWorld::RegisterSkinnedOpaqueRenderSystem() {
    SystemRegistry::Register(
        {.name = "SkinnedOpaqueRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 if (!m_skinnedRenderer || m_skinnedQueue.Empty() || !m_appRTs)
                     return;

                 uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                 auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                 auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
                 auto *nativeCL = cmd.Get();

                 // 屏障：G-buffer RTs COMMON → RENDER_TARGET
                 auto &rtPool = RenderTargetPool::GetInstance();
                 auto barrierRT = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_COMMON,
                                                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                     nativeCL->ResourceBarrier(1, &b);
                 };
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferAlbedo()));
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferNormal()));
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferMaterial()));
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferWorldPos()));

                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();

                 // 设置视口 + 描述符堆
                 const auto &vp = m_context->DeviceContext->GetViewport();
                 const auto &sr = m_context->DeviceContext->GetScissorRect();
                 nativeCL->RSSetViewports(1, &vp);
                 nativeCL->RSSetScissorRects(1, &sr);
                 ID3D12DescriptorHeap *heaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 nativeCL->SetDescriptorHeaps(1, heaps);

                 // 绑定 G-buffer RTs
                 D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferAlbedo()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferNormal()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferMaterial()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferWorldPos()),
                 };
                 nativeCL->OMSetRenderTargets(4, rtvs, FALSE, &dsvHandle);

                 // G-buffer 渲染
                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 D3D12_GPU_DESCRIPTOR_HANDLE matSRV = m_context->MaterialMgr->GetMaterialBufferSRV();
                 D3D12_GPU_DESCRIPTOR_HANDLE texHeapStart =
                     m_context->DescriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, 0);

                 m_skinnedRenderer->BeginFrameGBuffer(cmd, passCBAddr, matSRV, texHeapStart);
                 m_skinnedRenderer->DrawGBuffer(cmd, m_skinnedQueue);
                 m_skinnedRenderer->EndFrameGBuffer();

                 // 屏障：G-buffer RTs RENDER_TARGET → COMMON（为下帧/后续 Pass 准备）
                 auto barrierBack = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                   D3D12_RESOURCE_STATE_COMMON);
                     nativeCL->ResourceBarrier(1, &b);
                 };
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferAlbedo()));
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferNormal()));
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferMaterial()));
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferWorldPos()));

                 cmd.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Opaque, cmdH);

                 uint64_t seq = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Opaque,
         .alwaysRun = true});
}

void GameWorld::RegisterSkyboxSystem() {
    SystemRegistry::Register(
        {.name = "SkyboxRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 if (!m_skyRenderer || !m_skyboxGeometryHandle.IsValid()) {
                     return;
                 }

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 auto backBuffer = m_context->GetBackBuffer();
                 auto rtvHandle = m_context->DeviceContext->GetCurrentBackBufferView();
                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();
                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();

                 D3D12_GPU_DESCRIPTOR_HANDLE skySRV = {0};
                 if (m_skyboxTextureHandle.IsValid()) {
                     skySRV = m_context->TextureMgr->GetSRV(m_skyboxTextureHandle);
                 }

                 D3D12_RESOURCE_BARRIER barrier = {};
                 barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 barrier.Transition.pResource = backBuffer;
                 barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                 barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 const auto &viewport = m_context->DeviceContext->GetViewport();
                 const auto &scissorRect = m_context->DeviceContext->GetScissorRect();
                 cmdList.Get()->RSSetViewports(1, &viewport);
                 cmdList.Get()->RSSetScissorRects(1, &scissorRect);
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 ID3D12DescriptorHeap *descriptorHeaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 cmdList.Get()->SetDescriptorHeaps(1, descriptorHeaps);

                 m_skyRenderer->BeginFrame(cmdList, passCBAddr, skySRV);
                 m_skyRenderer->DrawSky(cmdList, m_skyboxGeometryHandle, m_skyboxObjectCBAddress);
                 m_skyRenderer->EndFrame();

                 barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::PostProcess, cmdListHandle);

                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::PostProcess,
         .alwaysRun = true});
}

void GameWorld::RegisterWaterRenderSystem() {
    SystemRegistry::Register(
        {.name = "WaterRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 if (m_transparentQueue.Empty()) {
                     return;
                 }

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 auto backBuffer = m_context->GetBackBuffer();
                 auto rtvHandle = m_context->DeviceContext->GetCurrentBackBufferView();
                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();

                 D3D12_RESOURCE_BARRIER barrier = {};
                 barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 barrier.Transition.pResource = backBuffer;
                 barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                 barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 const auto &viewport = m_context->DeviceContext->GetViewport();
                 const auto &scissorRect = m_context->DeviceContext->GetScissorRect();
                 cmdList.Get()->RSSetViewports(1, &viewport);
                 cmdList.Get()->RSSetScissorRects(1, &scissorRect);
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 ID3D12DescriptorHeap *descriptorHeaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 cmdList.Get()->SetDescriptorHeaps(1, descriptorHeaps);

                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 D3D12_GPU_VIRTUAL_ADDRESS lightCBAddr = LightManager::GetInstance().GetLightCBAddress();
                 D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV = m_context->MaterialMgr->GetMaterialBufferSRV();

                 m_waterRenderer->BeginFrame(cmdList, passCBAddr, lightCBAddr, materialBufferSRV, m_waterCBAddress);

                 // 遍历透明物体队列（使用新的 TRenderQueue<TransparentRenderItem>）
                 for (const auto &item : m_transparentQueue.GetItems()) {
                     if (!item.IsValid())
                         continue;
                     D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV = {};
                     if (m_skyboxTextureHandle.IsValid()) {
                         envMapSRV = m_context->TextureMgr->GetSRV(m_skyboxTextureHandle);
                     }
                     m_waterRenderer->DrawWater(cmdList, item.geometryHandle, item.worldMatrix, item.objectCBAddress,
                                                envMapSRV);
                 }
                 m_waterRenderer->EndFrame();

                 barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Transparent, cmdListHandle);

                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Transparent,
         .alwaysRun = true});
}

void GameWorld::RegisterTerrainSystems() {
    // 合并 System：从 SharedDataStore 读取 TerrainGPUResult → 注册 GPU 资源 → 创建 ECS 实体
    //
    // 数据流：
    //   事件 payload (低位 32 bits) = DataSlotHandle → SharedDataStore::GetData()
    //   → TerrainGPUResult* → 注册 GeometryHandle/TextureHandle → 创建 Entity
    //   高位 32 bits = 资源类型标识（0=地形），用于区分不同资源类型
    SystemRegistry::Register(
        {.name = "TerrainGPUCreateSystem",
         .func =
             [this](Registry &reg, const MessageContext &ctx) {
                 // 从 payload 低位解码 DataSlotHandle
                 auto cpuHandle = Core::DataSlotHandle::FromUint32(static_cast<uint32_t>(ctx.payload & 0xFFFFFFFF));
                 uint32_t flags = static_cast<uint32_t>(ctx.payload >> 32);
                 m_context->Logging->Info("[TerrainGPUCreate] Triggered (handle={}, flags={})",
                                          static_cast<uint32_t>(cpuHandle), flags);

                 auto &assetMgr = Core::SharedDataStore::GetInstance();
                 auto *result = static_cast<const Async::TerrainGPUResult *>(assetMgr.GetData(cpuHandle));
                 if (!result) {
                     m_context->Logging->Error("[TerrainGPUCreate] Invalid handle from AssetDataManager");
                     return;
                 }

                 // 注册几何体（PatchMesh）
                 Resource::PatchMesh mesh;
                 mesh.vertexBufferHandle = result->vbHandle;
                 mesh.indexBufferHandle = result->ibHandle;
                 mesh.vertexCount = result->vertexCount;
                 mesh.indexCount = result->indexCount;
                 mesh.patchCount = result->indexCount / 4;
                 mesh.vertexStride = sizeof(GeometryGenerator::Vertex);
                 mesh.indexFormat = DXGI_FORMAT_R32_UINT;
                 mesh.patchType = Resource::PatchType::Quad;
                 mesh.isGpuReady = true;
                 mesh.localBounds = result->bounds;
                 auto geoHandle = m_context->GeometryResourceManager->RegisterGeometry<PatchMesh>(mesh);

                 // 创建纹理 SRV
                 TextureHandle heightMapHandle = TextureHandle::Invalid();
                 TextureHandle albedoHandle = TextureHandle::Invalid();
                 TextureHandle normalHandle = TextureHandle::Invalid();
                 if (result->heightMapGpuHandle.IsValid() || result->albedoGpuHandle.IsValid() ||
                     result->normalMapGpuHandle.IsValid()) {
                     uint32_t baseSrvIdx = m_context->DescriptorHeaps->AllocateConsecutive(PartitionType::Texture, 3);
                     if (baseSrvIdx != UINT32_MAX) {
                         auto device = m_context->DeviceContext->GetDevice();
                         auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

                         if (result->heightMapGpuHandle.IsValid()) {
                             D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                             srvDesc.Format = result->heightMapDesc.Format;
                             srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                             srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                             srvDesc.Texture2D.MostDetailedMip = 0;
                             srvDesc.Texture2D.MipLevels = result->heightMapDesc.MipLevels;
                             auto cpuHandleDesc = m_context->DescriptorHeaps->GetCpuHandle(
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, baseSrvIdx);
                             device->CreateShaderResourceView(gpuMgr.GetResource(result->heightMapGpuHandle), &srvDesc,
                                                              cpuHandleDesc);
                             heightMapHandle =
                                 m_context->TextureMgr->RegisterTexture(result->heightMapGpuHandle, baseSrvIdx);
                         }
                         if (result->albedoGpuHandle.IsValid()) {
                             D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                             srvDesc.Format = result->albedoDesc.Format;
                             srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                             srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                             srvDesc.Texture2D.MostDetailedMip = 0;
                             srvDesc.Texture2D.MipLevels = result->albedoDesc.MipLevels;
                             auto cpuHandleDesc = m_context->DescriptorHeaps->GetCpuHandle(
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, baseSrvIdx + 1);
                             device->CreateShaderResourceView(gpuMgr.GetResource(result->albedoGpuHandle), &srvDesc,
                                                              cpuHandleDesc);
                             albedoHandle =
                                 m_context->TextureMgr->RegisterTexture(result->albedoGpuHandle, baseSrvIdx + 1);
                         }
                         if (result->normalMapGpuHandle.IsValid()) {
                             D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                             srvDesc.Format = result->normalMapDesc.Format;
                             srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                             srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                             srvDesc.Texture2D.MostDetailedMip = 0;
                             srvDesc.Texture2D.MipLevels = result->normalMapDesc.MipLevels;
                             auto cpuHandleDesc = m_context->DescriptorHeaps->GetCpuHandle(
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, baseSrvIdx + 2);
                             device->CreateShaderResourceView(gpuMgr.GetResource(result->normalMapGpuHandle), &srvDesc,
                                                              cpuHandleDesc);
                             normalHandle =
                                 m_context->TextureMgr->RegisterTexture(result->normalMapGpuHandle, baseSrvIdx + 2);
                         }
                     }
                 }

                 // 创建 ECS 实体
                 auto entity = reg.CreateEntity();
                 reg.AddComponent<ECS::TransformComponent>(entity, XMFLOAT3(0, -50, 0), XMFLOAT3(0, 0, 0),
                                                           XMFLOAT3(1, 1, 1));
                 ECS::TerrainComponent terrainComp;
                 terrainComp.geometryHandle = geoHandle;
                 terrainComp.heightMapHandle = heightMapHandle;
                 terrainComp.albedoHandle = albedoHandle;
                 terrainComp.normalHandle = normalHandle;
                 terrainComp.heightScale = result->maxHeight;
                 reg.AddComponent<ECS::TerrainComponent>(entity, std::move(terrainComp));

                 // 释放 AssetDataManager 中的数据
                 assetMgr.ScheduleRelease(cpuHandle, 0);

                 m_context->Logging->Info("[TerrainGPUCreate] Terrain entity created (handle={})",
                                          static_cast<uint32_t>(cpuHandle));
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Main,
         .priority = TaskPriority::Normal,
         .interestedMessages = {static_cast<uint32_t>(Event::EventType::ResourceReadyEvent)}});
}

void GameWorld::RegisterTerrainRenderSystem() {
    SystemRegistry::Register(
        {.name = "TerrainRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 if (m_terrainQueue.Empty() || !m_appRTs) {
                     return;
                 }

                 uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                 auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                 auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
                 auto *nativeCL = cmd.Get();

                 // 屏障：G-buffer RTs COMMON → RENDER_TARGET
                 auto &rtPool = RenderTargetPool::GetInstance();
                 auto barrierRT = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_COMMON,
                                                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                     nativeCL->ResourceBarrier(1, &b);
                 };
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferAlbedo()));
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferNormal()));
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferMaterial()));
                 barrierRT(rtPool.GetResource(m_appRTs->GetGBufferWorldPos()));

                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();
                 const auto &vp = m_context->DeviceContext->GetViewport();
                 const auto &sr = m_context->DeviceContext->GetScissorRect();
                 nativeCL->RSSetViewports(1, &vp);
                 nativeCL->RSSetScissorRects(1, &sr);

                 ID3D12DescriptorHeap *heaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 nativeCL->SetDescriptorHeaps(1, heaps);

                 // 绑定 G-buffer RTs
                 D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferAlbedo()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferNormal()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferMaterial()),
                     rtPool.GetRtvHandle(m_appRTs->GetGBufferWorldPos()),
                 };
                 nativeCL->OMSetRenderTargets(4, rtvs, FALSE, &dsvHandle);

                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 m_terrainRenderer->BeginFrameGBuffer(cmd, passCBAddr);

                 for (const auto &item : m_terrainQueue.GetItems()) {
                     if (!item.IsValid())
                         continue;
                     m_terrainRenderer->DrawTerrainGBuffer(cmd, item);
                 }

                 // 屏障：G-buffer RTs RENDER_TARGET → COMMON（为下帧/后续 Pass 准备）
                 auto barrierBack = [&](ID3D12Resource *res) {
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                   D3D12_RESOURCE_STATE_COMMON);
                     nativeCL->ResourceBarrier(1, &b);
                 };
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferAlbedo()));
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferNormal()));
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferMaterial()));
                 barrierBack(rtPool.GetResource(m_appRTs->GetGBufferWorldPos()));

                 cmd.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Opaque, cmdH);
                 uint64_t seq = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Opaque,
         .alwaysRun = true});
}

void GameWorld::RegisterClearSystem() {
    SystemRegistry::Register(
        {.name = "ClearRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 auto backBuffer = m_context->GetBackBuffer();

                 D3D12_RESOURCE_BARRIER beginBarrier = {};
                 beginBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 beginBarrier.Transition.pResource = backBuffer;
                 beginBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                 beginBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 beginBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &beginBarrier);

                 const auto &viewport = m_context->DeviceContext->GetViewport();
                 const auto &scissorRect = m_context->DeviceContext->GetScissorRect();
                 cmdList.Get()->RSSetViewports(1, &viewport);
                 cmdList.Get()->RSSetScissorRects(1, &scissorRect);

                 auto rtvHandle = m_context->DeviceContext->GetCurrentBackBufferView();
                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 const float clearColor[] = {0.0f, 0.2f, 0.4f, 1.0f};
                 cmdList.Get()->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
                 cmdList.Get()->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                                      1.0f, 0, 0, nullptr);

                 D3D12_RESOURCE_BARRIER endBarrier = {};
                 endBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 endBarrier.Transition.pResource = backBuffer;
                 endBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 endBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 endBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &endBarrier);

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::PrePass, cmdListHandle);

                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::PrePass,
         .alwaysRun = true});
}

void GameWorld::RegisterShadowRenderSystem() {
    SystemRegistry::Register(
        {.name = "ShadowRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 auto &lightMgr = LightManager::GetInstance();
                 if (!lightMgr.HasShadow(LightType::Directional) || m_opaqueQueue.Empty())
                     return;

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 uint32_t dirCount = lightMgr.GetLightCount(LightType::Directional);
                 for (uint32_t i = 0; i < dirCount; ++i) {
                     const Light *light = lightMgr.GetLight(LightType::Directional, i);
                     if (!light || light->CastShadow <= 0.5f)
                         continue;

                     const auto &shadowRes = lightMgr.GetDirShadowResource();
                     ID3D12Resource *depthTexture = DepthStencilPool::GetInstance().GetResource(shadowRes.handle);
                     if (!depthTexture || shadowRes.cbvAddress == 0)
                         continue;

                     D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
                         m_context->DescriptorHeaps->GetCpuHandle(PartitionType::Dsv, shadowRes.dsvSlot);

                     m_shadowRenderer->BeginOffscreen(cmdList, shadowRes.cbvAddress, dsvHandle, shadowRes.resolution,
                                                      shadowRes.resolution);

                     for (const auto &item : m_opaqueQueue) {
                         if (!item.IsValid())
                             continue;
                         m_shadowRenderer->DrawInstanced(cmdList, item.geometryHandle, item.instanceBuffer,
                                                         item.instanceCount);
                     }

                     m_shadowRenderer->EndOffscreen(cmdList);
                 }

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::PrePass, cmdListHandle);

                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::PrePass,
         .alwaysRun = true});
}

void GameWorld::RegisterPointShadowRenderSystem() {
    SystemRegistry::Register(
        {.name = "PointShadowRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 auto &lightMgr = LightManager::GetInstance();
                 if (!lightMgr.HasShadow(LightType::Point) || m_opaqueQueue.Empty())
                     return;

                 uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                 auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                 auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);

                 cmd.Get()->SetGraphicsRootSignature(m_shadowRenderer->GetRootSignature());

                 m_shadowRenderer->SetInPass(true);

                 uint32_t ptCount = lightMgr.GetLightCount(LightType::Point);
                 for (uint32_t lightIdx = 0; lightIdx < ptCount; ++lightIdx) {
                     const Light *light = lightMgr.GetLight(LightType::Point, lightIdx);
                     if (!light || light->CastShadow <= 0.5f)
                         continue;

                     const auto &shadowRes = lightMgr.GetPointShadowResource(lightIdx);
                     if (!shadowRes.isValid)
                         continue;

                     auto &dsPool = DepthStencilPool::GetInstance();
                     uint32_t res = shadowRes.resolution;

                     // 全数组 DSV 清除一次（6 slice 共享纹理）
                     D3D12_CPU_DESCRIPTOR_HANDLE arrayDsvHandle = dsPool.GetDsvHandle(shadowRes.arrayHandle);
                     cmd.Get()->ClearDepthStencilView(arrayDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

                     D3D12_VIEWPORT vp = {0, 0, (float)res, (float)res, 0, 1};
                     D3D12_RECT scissor = {0, 0, (LONG)res, (LONG)res};
                     cmd.Get()->RSSetViewports(1, &vp);
                     cmd.Get()->RSSetScissorRects(1, &scissor);

                     if (ID3D12PipelineState *gsPSO = m_shadowRenderer->GetPointGSPSO()) {
                         // GS 路径：一次 DrawCall 展开 6 面
                         cmd.Get()->SetPipelineState(gsPSO);
                         cmd.Get()->OMSetRenderTargets(0, nullptr, FALSE, &arrayDsvHandle);
                         cmd.Get()->SetGraphicsRootConstantBufferView(1, shadowRes.cbvAddress);

                         for (const auto &item : m_opaqueQueue) {
                             if (!item.IsValid())
                                 continue;
                             if (item.indexCount > 0) {
                                 m_shadowRenderer->DrawIndexedInstancedSubmesh(
                                     cmd, item.geometryHandle, item.instanceBuffer, item.instanceCount, item.startIndex,
                                     item.startVertex, item.indexCount);
                             } else {
                                 m_shadowRenderer->DrawInstanced(cmd, item.geometryHandle, item.instanceBuffer,
                                                                 item.instanceCount);
                             }
                         }
                     } else {
                         // 6 遍渲染（GS 不可用时的回退）
                         for (int face = 0; face < 6; ++face) {
                             D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
                                 m_context->DescriptorHeaps->GetCpuHandle(PartitionType::Dsv, shadowRes.dsvSlots[face]);

                             cmd.Get()->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);

                             cmd.Get()->SetPipelineState(m_shadowRenderer->GetPointInstancedPSO());
                             cmd.Get()->SetGraphicsRootConstantBufferView(1, shadowRes.cbvAddress);
                             cmd.Get()->SetGraphicsRoot32BitConstant(3, face, 0); // gShadowLightIndex

                             for (const auto &item : m_opaqueQueue) {
                                 if (!item.IsValid())
                                     continue;
                                 if (item.indexCount > 0) {
                                     m_shadowRenderer->DrawIndexedInstancedSubmesh(
                                         cmd, item.geometryHandle, item.instanceBuffer, item.instanceCount,
                                         item.startIndex, item.startVertex, item.indexCount);
                                 } else {
                                     m_shadowRenderer->DrawInstanced(cmd, item.geometryHandle, item.instanceBuffer,
                                                                     item.instanceCount);
                                 }
                             }
                         }
                     }
                 }

                 m_shadowRenderer->SetInPass(false);

                 cmd.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::PrePass, cmdH);

                 uint64_t seq = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::PrePass,
         .alwaysRun = true});
}

void GameWorld::RegisterSpotShadowRenderSystem() {
    SystemRegistry::Register(
        {.name = "SpotShadowRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 auto &lightMgr = LightManager::GetInstance();

                 if (!lightMgr.HasShadow(LightType::Spot) || m_opaqueQueue.Empty())
                     return;

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 // 使用聚光灯专用 PSO（SpotShadowVS_Instanced 读取 gSpotLightViewProj）
                 cmdList.Get()->SetGraphicsRootSignature(m_shadowRenderer->GetRootSignature());

                 uint32_t spCount = lightMgr.GetLightCount(LightType::Spot);
                 for (uint32_t i = 0; i < spCount; ++i) {
                     const Light *light = lightMgr.GetLight(LightType::Spot, i);
                     if (!light || light->CastShadow <= 0.5f)
                         continue;

                     const auto &shadowRes = lightMgr.GetSpotShadowResource(i);
                     if (!shadowRes.isValid || shadowRes.cbvAddress == 0)
                         continue;

                     D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
                         m_context->DescriptorHeaps->GetCpuHandle(PartitionType::Dsv, shadowRes.dsvSlot);

                     ID3D12Resource *depthTexture = DepthStencilPool::GetInstance().GetResource(shadowRes.handle);
                     if (!depthTexture)
                         continue;

                     m_shadowRenderer->BeginOffscreen(cmdList, shadowRes.cbvAddress, dsvHandle, shadowRes.resolution,
                                                      shadowRes.resolution);
                     // BeginOffscreen 设了方向光 PSO，覆盖回聚光灯 PSO
                     cmdList.Get()->SetPipelineState(m_shadowRenderer->GetSpotPSO());

                     for (const auto &item : m_opaqueQueue) {
                         if (!item.IsValid())
                             continue;
                         m_shadowRenderer->DrawInstanced(cmdList, item.geometryHandle, item.instanceBuffer,
                                                         item.instanceCount);
                     }

                     m_shadowRenderer->EndOffscreen(cmdList);
                 }

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::PrePass, cmdListHandle);

                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::PrePass,
         .alwaysRun = true});
}

void GameWorld::RegisterSsaoSystem() {
    SystemRegistry::Register(
        {.name = "SsaoSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 auto &aoMgr = AmbientOcclusionManager::GetInstance();
                 if (!aoMgr.IsInitialized() || !m_appRTs)
                     return;

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);
                 auto *nativeCL = cmdList.Get();

                 // 从 G-buffer 读取法线 + 主深度缓冲
                 D3D12_GPU_DESCRIPTOR_HANDLE depthSRV = m_context->DeviceContext->GetDepthSRV();
                 D3D12_GPU_DESCRIPTOR_HANDLE normalSRV = m_appRTs->GetGBufferNormalSRV();
                 ID3D12Resource *depthRes = m_context->DeviceContext->GetSwapChainManager().GetDepthStencilBuffer();

                 auto *ambient0 = aoMgr.GetAmbientResource0();
                 auto *ambient1 = aoMgr.GetAmbientResource1();
                 auto barrier = [&](ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
                     if (!res)
                         return;
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, from, to);
                     nativeCL->ResourceBarrier(1, &b);
                 };

                 // 屏障：主深度 DEPTH_WRITE → PIXEL_SHADER_RESOURCE
                 barrier(depthRes, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                 // 屏障：G-buffer 法线 COMMON → PIXEL_SHADER_RESOURCE
                 barrier(m_appRTs->GetGBufferNormalResource(), D3D12_RESOURCE_STATE_COMMON,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                 // 屏障：AO 环境贴图 COMMON → RENDER_TARGET（RenderTargetPool 初始状态为 COMMON）
                 barrier(ambient0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
                 barrier(ambient1, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);

                 // 设置视口（新命令列表需要）
                 const auto &vp = m_context->DeviceContext->GetViewport();
                 const auto &sr = m_context->DeviceContext->GetScissorRect();
                 nativeCL->RSSetViewports(1, &vp);
                 nativeCL->RSSetScissorRects(1, &sr);

                 const auto &passCB = m_context->FrameResourceManager->GetPassConstants();
                 aoMgr.Execute(nativeCL, depthSRV, normalSRV, passCB.View, passCB.Proj);

                 // 屏障：AO 环境贴图 RENDER_TARGET → COMMON（回到 RenderTargetPool 初始状态）
                 barrier(ambient0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
                 barrier(ambient1, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
                 // 屏障：G-buffer 法线 PIXEL_SHADER_RESOURCE → COMMON
                 barrier(m_appRTs->GetGBufferNormalResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_COMMON);
                 // 屏障：主深度 PIXEL_SHADER_RESOURCE → DEPTH_WRITE
                 barrier(depthRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::DynamicAOcclusion, cmdListHandle);

                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::DynamicAOcclusion,
         .alwaysRun = true});
}

void GameWorld::RegisterProbeCaptureSystem() {
    SystemRegistry::Register(
        {.name = "ProbeCaptureSystem",
         .func =
             [this](Registry &, const MessageContext &) {
                 if (!m_probeRenderer || m_activeProbeCount == 0)
                     return;

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 ID3D12DescriptorHeap *heaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 cmdList.Get()->SetDescriptorHeaps(1, heaps);

                 D3D12_GPU_VIRTUAL_ADDRESS lightCBAddr = LightManager::GetInstance().GetLightCBAddress();
                 D3D12_GPU_DESCRIPTOR_HANDLE matBufferSRV = m_context->MaterialMgr->GetMaterialBufferSRV();

                 for (uint32_t i = 0; i < m_activeProbeCount; ++i) {
                     if (m_probeQueues[i].Empty() || m_probeCaptureInfo[i].rtvBaseSlot == UINT32_MAX)
                         continue;
                     const auto &info = m_probeCaptureInfo[i];
                     D3D12_CPU_DESCRIPTOR_HANDLE depthDSV =
                         m_context->DescriptorHeaps->GetCpuHandle(PartitionType::Dsv, info.dsvSlot);

                     D3D12_GPU_VIRTUAL_ADDRESS captureCBAddr = info.captureCBAddress;
                     if (captureCBAddr == 0)
                         continue;
                     D3D12_CPU_DESCRIPTOR_HANDLE cubemapRTV =
                         m_context->DescriptorHeaps->GetCpuHandle(PartitionType::Rtv, info.rtvBaseSlot);
                     D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart =
                         m_context->DescriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, 0);
                     m_probeRenderer->BeginCapture(cmdList, info.cubemapResource, cubemapRTV, depthDSV, info.resolution,
                                                   info.resolution, captureCBAddr, lightCBAddr, matBufferSRV,
                                                   textureHeapStart);
                     for (const auto &item : m_probeQueues[i]) {
                         if (!item.IsValid())
                             continue;
                         m_probeRenderer->DrawInstanced(cmdList, item.geometryHandle, item.instanceBuffer,
                                                        item.instanceCount);
                     }
                     m_probeRenderer->EndCapture(cmdList);
                 }

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::PrePass, cmdListHandle);
                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::PrePass,
         .alwaysRun = true});
}

void GameWorld::RegisterBillboardRenderSystem() {
    SystemRegistry::Register(
        {.name = "BillboardRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 if (m_billboardQueue.Empty()) {
                     return;
                 }

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 auto backBuffer = m_context->GetBackBuffer();

                 D3D12_RESOURCE_BARRIER beginBarrier = {};
                 beginBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 beginBarrier.Transition.pResource = backBuffer;
                 beginBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                 beginBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 beginBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &beginBarrier);

                 const auto &viewport = m_context->DeviceContext->GetViewport();
                 const auto &scissorRect = m_context->DeviceContext->GetScissorRect();
                 cmdList.Get()->RSSetViewports(1, &viewport);
                 cmdList.Get()->RSSetScissorRects(1, &scissorRect);

                 auto rtvHandle = m_context->DeviceContext->GetCurrentBackBufferView();
                 auto dsvHandle = m_context->DeviceContext->GetDepthStencilView();
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 ID3D12DescriptorHeap *descriptorHeaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 cmdList.Get()->SetDescriptorHeaps(1, descriptorHeaps);

                 // 获取 Pass Constant Buffer 和 Light CB 地址
                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 D3D12_GPU_VIRTUAL_ADDRESS lightCBAddr = LightManager::GetInstance().GetLightCBAddress();
                 D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV = m_context->MaterialMgr->GetMaterialBufferSRV();

                 // 获取纹理数组堆起始 GPU handle（TextureSrv 分区起始）
                 D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart =
                     m_context->DescriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, 0);

                 // 获取公告牌 Texture2DArray 的 SRV（slot 5: t20）
                 D3D12_GPU_DESCRIPTOR_HANDLE billboardTexSRV =
                     m_context->TextureMgr->GetSRV(m_billboardTextureHandles[0]);

                 // 开始公告牌渲染
                 m_billboardRenderer->BeginFrame(cmdList, passCBAddr, lightCBAddr, materialBufferSRV, textureHeapStart,
                                                 billboardTexSRV);

                 // 遍历公告牌队列
                 for (const auto &item : m_billboardQueue.GetItems()) {
                     if (!item.IsValid())
                         continue;
                     m_billboardRenderer->DrawBillboard(cmdList, item);
                 }

                 // 屏障：转换回 PRESENT 状态
                 beginBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 beginBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 cmdList.Get()->ResourceBarrier(1, &beginBarrier);

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Billboard, cmdListHandle);

                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::Opaque,
         .alwaysRun = true});
}
