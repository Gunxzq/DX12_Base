#include "Async/TerrainLoadTask.h"
#include "Boot/GameContext.h"
#include "ECS/Core/Components.h"
#include "ECS/Core/Registry.h"
#include "Event/EventRegistry.h"
#include "Event/EventTypes.h"
#include "Framework/SystemRegistry.h"
#include "GameWorld.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "Renderer/Pipeline/BillboardRenderer.h"
#include "Renderer/Pipeline/OpaqueRenderer.h"
#include "Renderer/Pipeline/ReflectionProbeRenderer.h"
#include "Renderer/Pipeline/ShadowRenderer.h"
#include "Renderer/Pipeline/SkyRenderer.h"
#include "Renderer/Pipeline/TerrainRenderer.h"
#include "Renderer/Pipeline/WaterRenderer.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/AmbientOcclusionManager/AmbientOcclusionManager.h"
#include "Renderer/Scene/LightManager/LightManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Manager/MaterialManager.h"
#include "Resource/Texture/TextureManager.h"
#include "Scheduler/FrameDriver.h"
#include <DirectXMath.h>

using namespace DirectX;
using namespace DX12Engine;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::Scheduler;

// ========================================================================
// GameWorld — 渲染 System 注册
// ========================================================================

void GameWorld::RegisterRotationSystem() {
    // 已注释，保留空实现
}

void GameWorld::RegisterCubeRenderSystem() {
    SystemRegistry::Register(
        {.name = "CubeRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 if (m_opaqueQueue.Empty()) {
                     m_context->Logging->Debug("[CubeRender] Opaque queue is empty, skipping draw");
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

                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 D3D12_GPU_VIRTUAL_ADDRESS lightCBAddr = LightManager::GetInstance().GetLightCBAddress();

                 D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV = m_context->MaterialMgr->GetMaterialBufferSRV();

                 auto &lightMgr = LightManager::GetInstance();
                 D3D12_GPU_DESCRIPTOR_HANDLE shadowDataSRV = lightMgr.GetShadowDataSRV();
                 D3D12_GPU_DESCRIPTOR_HANDLE shadowMapSRV = lightMgr.GetShadowMapSRV();

                 ID3D12DescriptorHeap *descriptorHeaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};

                 cmdList.Get()->SetDescriptorHeaps(1, descriptorHeaps);

                 D3D12_GPU_DESCRIPTOR_HANDLE cubemapArraySRV = m_context->ReflectionProbeMgr->GetProbeCubemapArraySRV();

                 D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart =
                     m_context->DescriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, 0);

                 D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV = {};
                 if (m_skyboxTextureHandle.IsValid()) {
                     envMapSRV = m_context->TextureMgr->GetSRV(m_skyboxTextureHandle);
                 }

                 D3D12_GPU_DESCRIPTOR_HANDLE aoMapSRV = AmbientOcclusionManager::GetInstance().GetAmbientMapSRV();

                 m_renderer->BeginFrame(cmdList, passCBAddr, lightCBAddr, materialBufferSRV, shadowDataSRV,
                                        shadowMapSRV, cubemapArraySRV, textureHeapStart, envMapSRV, aoMapSRV);

                 for (const auto &item : m_opaqueQueue) {
                     if (!item.IsValid())
                         continue;
                     m_renderer->DrawInstanced(cmdList, item.geometryHandle, item.instanceBuffer, item.instanceCount,
                                               item.startIndex, item.startVertex, item.indexCount);
                 }

                 m_renderer->EndFrame();

                 D3D12_RESOURCE_BARRIER endBarrier = {};
                 endBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 endBarrier.Transition.pResource = backBuffer;
                 endBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 endBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 endBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &endBarrier);

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Opaque, cmdListHandle);
                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
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
                 if (!m_skinnedRenderer || m_skinnedQueue.Empty()) {
                     return;
                 }

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                 auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);

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

                 ID3D12DescriptorHeap *heaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 cmdList.Get()->SetDescriptorHeaps(1, heaps);

                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 D3D12_GPU_VIRTUAL_ADDRESS lightCBAddr = LightManager::GetInstance().GetLightCBAddress();
                 D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV = m_context->MaterialMgr->GetMaterialBufferSRV();
                 D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart =
                     m_context->DescriptorHeaps->GetPartitionGpuHandle(PartitionType::Texture, 0);

                 D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV = {};
                 if (m_skyboxTextureHandle.IsValid()) {
                     envMapSRV = m_context->TextureMgr->GetSRV(m_skyboxTextureHandle);
                 }

                 m_skinnedRenderer->BeginFrame(cmdList, passCBAddr, lightCBAddr, materialBufferSRV, textureHeapStart,
                                               envMapSRV);
                 m_skinnedRenderer->DrawOpaque(cmdList, m_skinnedQueue);
                 m_skinnedRenderer->EndFrame();

                 D3D12_RESOURCE_BARRIER endBarrier = {};
                 endBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 endBarrier.Transition.pResource = backBuffer;
                 endBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 endBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 endBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &endBarrier);

                 cmdList.Close();
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
                                                item.textureSRV, envMapSRV);
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
    // System A: TerrainGPUCreateSystem
    SystemRegistry::Register(
        {.name = "TerrainGPUCreateSystem",
         .func =
             [this](Registry &reg, const MessageContext &ctx) {
                 uint32_t requestId = static_cast<uint32_t>(ctx.payload >> 32);
                 m_context->Logging->Info("[TerrainGPUCreate] Triggered (request={})", requestId);
                 if (!m_terrainReadyState)
                     return;
                 auto &state = *m_terrainReadyState;
                 if (!state.geometryCreated.load()) {
                     return;
                 }
                 if (!state.vbHandle.IsValid() || !state.ibHandle.IsValid()) {
                     return;
                 }

                 uint32_t indexCount = static_cast<uint32_t>(m_terrainLoadData->indices.size());
                 Resource::PatchMesh mesh;
                 mesh.vertexBufferHandle = state.vbHandle;
                 mesh.indexBufferHandle = state.ibHandle;
                 mesh.vertexCount = static_cast<uint32_t>(m_terrainLoadData->vertices.size());
                 mesh.indexCount = indexCount;
                 mesh.patchCount = indexCount / 4;
                 mesh.vertexStride = sizeof(GeometryGenerator::Vertex);
                 mesh.indexFormat = DXGI_FORMAT_R32_UINT;
                 mesh.patchType = Resource::PatchType::Quad;
                 mesh.isGpuReady = true;
                 mesh.localBounds = state.bounds;
                 auto handle = m_context->GeometryResourceManager->RegisterGeometry<PatchMesh>(mesh);
                 m_terrainGeometryHandle = handle;

                 if ((!m_terrainTextureHandle.IsValid() || !m_terrainAlbedoHandle.IsValid() ||
                      !m_terrainNormalHandle.IsValid()) &&
                     state.heightMapCreated.load() && state.albedoCreated.load() && state.normalCreated.load()) {
                     uint32_t baseSrvIdx = m_context->DescriptorHeaps->AllocateConsecutive(PartitionType::Texture, 3);
                     if (baseSrvIdx != UINT32_MAX) {
                         auto device = m_context->DeviceContext->GetDevice();
                         auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
                         if (state.heightMapGpuHandle.IsValid() && !m_terrainTextureHandle.IsValid()) {
                             D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                             srvDesc.Format = state.heightMapDesc.Format;
                             srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                             srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                             srvDesc.Texture2D.MostDetailedMip = 0;
                             srvDesc.Texture2D.MipLevels = state.heightMapDesc.MipLevels;
                             auto cpuHandle = m_context->DescriptorHeaps->GetCpuHandle(
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, baseSrvIdx);
                             device->CreateShaderResourceView(gpuMgr.GetResource(state.heightMapGpuHandle), &srvDesc,
                                                              cpuHandle);
                             m_terrainTextureHandle =
                                 m_context->TextureMgr->RegisterTexture(state.heightMapGpuHandle, baseSrvIdx);
                         }
                         if (state.albedoGpuHandle.IsValid() && !m_terrainAlbedoHandle.IsValid()) {
                             D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                             srvDesc.Format = state.albedoDesc.Format;
                             srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                             srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                             srvDesc.Texture2D.MostDetailedMip = 0;
                             srvDesc.Texture2D.MipLevels = state.albedoDesc.MipLevels;
                             auto cpuHandle = m_context->DescriptorHeaps->GetCpuHandle(
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, baseSrvIdx + 1);
                             device->CreateShaderResourceView(gpuMgr.GetResource(state.albedoGpuHandle), &srvDesc,
                                                              cpuHandle);
                             m_terrainAlbedoHandle =
                                 m_context->TextureMgr->RegisterTexture(state.albedoGpuHandle, baseSrvIdx + 1);
                         }
                         if (state.normalMapGpuHandle.IsValid() && !m_terrainNormalHandle.IsValid()) {
                             D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                             srvDesc.Format = state.normalMapDesc.Format;
                             srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                             srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                             srvDesc.Texture2D.MostDetailedMip = 0;
                             srvDesc.Texture2D.MipLevels = state.normalMapDesc.MipLevels;
                             auto cpuHandle = m_context->DescriptorHeaps->GetCpuHandle(
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, baseSrvIdx + 2);
                             device->CreateShaderResourceView(gpuMgr.GetResource(state.normalMapGpuHandle), &srvDesc,
                                                              cpuHandle);
                             m_terrainNormalHandle =
                                 m_context->TextureMgr->RegisterTexture(state.normalMapGpuHandle, baseSrvIdx + 2);
                         }
                     }
                 }
                 uint64_t payload = Event::MakeAssetLoadedPayload(requestId, handle.index, handle.generation);
                 Event::MessageDispatcher::GetInstance()->PostEvent(
                     static_cast<uint32_t>(Event::EventType::TerrainReadyEvent), 0, payload,
                     Event::EventPriority::P2_Normal);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Main,
         .priority = TaskPriority::Normal,
         .interestedMessages = {static_cast<uint32_t>(Event::EventType::TerrainLoadedEvent)}});

    // System B: TerrainCombineSystem
    SystemRegistry::Register({.name = "TerrainCombineSystem",
                              .func =
                                  [this](Registry &reg, const MessageContext &ctx) {
                                      uint32_t requestId = 0, handleIdx = 0, handleGen = 0;
                                      Event::DecodeAssetLoadedPayload(ctx.payload, requestId, handleIdx, handleGen);
                                      auto entity = reg.CreateEntity();
                                      m_terrainEntity = entity;
                                      reg.AddComponent<ECS::TransformComponent>(entity, XMFLOAT3(0, -50, 0),
                                                                                XMFLOAT3(0, 0, 0), XMFLOAT3(1, 1, 1));
                                      ECS::TerrainComponent terrainComp;
                                      terrainComp.geometryHandle = m_terrainGeometryHandle;
                                      terrainComp.heightMapHandle = m_terrainTextureHandle;
                                      terrainComp.albedoHandle = m_terrainAlbedoHandle;
                                      terrainComp.normalHandle = m_terrainNormalHandle;
                                      terrainComp.heightScale =
                                          m_terrainLoadData ? m_terrainLoadData->maxHeight : 20.0f;
                                      reg.AddComponent<ECS::TerrainComponent>(entity, std::move(terrainComp));
                                      m_terrainReadyState.reset();
                                  },
                              .phase = TaskPhase::Render,
                              .threadType = ThreadType::Main,
                              .priority = TaskPriority::Normal,
                              .interestedMessages = {static_cast<uint32_t>(Event::EventType::TerrainReadyEvent)}});
}

void GameWorld::RegisterTerrainRenderSystem() {
    SystemRegistry::Register(
        {.name = "TerrainRenderSystem",
         .func =
             [this](Registry &registry, const MessageContext &ctx) {
                 if (m_terrainQueue.Empty()) {
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
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 ID3D12DescriptorHeap *descriptorHeaps[] = {
                     m_context->DescriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                 cmdList.Get()->SetDescriptorHeaps(1, descriptorHeaps);

                 D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                 D3D12_GPU_VIRTUAL_ADDRESS lightCBAddr = LightManager::GetInstance().GetLightCBAddress();
                 D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV = m_context->MaterialMgr->GetMaterialBufferSRV();

                 auto &lightMgr = LightManager::GetInstance();
                 D3D12_GPU_DESCRIPTOR_HANDLE shadowDataSRV = lightMgr.GetShadowDataSRV();
                 D3D12_GPU_DESCRIPTOR_HANDLE shadowMapSRV = lightMgr.GetShadowMapSRV();

                 m_terrainRenderer->BeginFrame(cmdList, passCBAddr, lightCBAddr, materialBufferSRV, shadowDataSRV,
                                               shadowMapSRV);

                 for (const auto &item : m_terrainQueue.GetItems()) {
                     if (!item.IsValid())
                         continue;
                     m_terrainRenderer->DrawTerrain(cmdList, item);
                 }

                 m_terrainRenderer->EndFrame();

                 D3D12_RESOURCE_BARRIER endBarrier = {};
                 endBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 endBarrier.Transition.pResource = backBuffer;
                 endBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 endBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 endBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &endBarrier);

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::Opaque, cmdListHandle);

                 uint64_t sequence = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);
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
                 D3D12_GPU_VIRTUAL_ADDRESS dirShadowAddr = lightMgr.GetDirShadowAddress();
                 const auto &shadowRes = lightMgr.GetDirShadowResources();

                 if (!lightMgr.HasDirShadow() || !shadowRes.isValid || dirShadowAddr == 0 || m_opaqueQueue.Empty()) {
                     return;
                 }

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 auto &gpuMgr = GpuResourceManager::GetInstance();
                 ID3D12Resource *depthTexture = gpuMgr.GetResource(shadowRes.textureHandle);
                 if (!depthTexture) {
                     m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);
                     m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle,
                                                                                 m_context->GetNextSequence());
                     return;
                 }

                 D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
                     m_context->DescriptorHeaps->GetCpuHandle(PartitionType::Dsv, shadowRes.dsvSlot);

                 CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                     depthTexture,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                     D3D12_RESOURCE_STATE_DEPTH_WRITE);
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 m_shadowRenderer->BeginOffscreen(cmdList, dirShadowAddr, dsvHandle, shadowRes.resolution,
                                                  shadowRes.resolution);

                 for (const auto &item : m_opaqueQueue) {
                     if (!item.IsValid())
                         continue;
                     m_shadowRenderer->DrawInstanced(cmdList, item.geometryHandle, item.instanceBuffer,
                                                     item.instanceCount);
                 }

                 m_shadowRenderer->EndOffscreen(cmdList);

                 CD3DX12_RESOURCE_BARRIER barrierBack = CD3DX12_RESOURCE_BARRIER::Transition(
                     depthTexture, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                 cmdList.Get()->ResourceBarrier(1, &barrierBack);

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
                 if (!aoMgr.IsInitialized())
                     return;

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                 auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 D3D12_GPU_DESCRIPTOR_HANDLE depthSRV = aoMgr.GetPrivateDepthSRV();

                 auto *normalRes = aoMgr.GetNormalResource();
                 auto *ambient0 = aoMgr.GetAmbientResource0();
                 auto *ambient1 = aoMgr.GetAmbientResource1();
                 auto barrier = [&](ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
                     if (!res)
                         return;
                     auto b = CD3DX12_RESOURCE_BARRIER::Transition(res, from, to);
                     cmdList.Get()->ResourceBarrier(1, &b);
                 };
                 barrier(normalRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                 barrier(ambient0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                 barrier(ambient1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

                 D3D12_CPU_DESCRIPTOR_HANDLE normalRTV = aoMgr.GetNormalMapRTV();
                 D3D12_CPU_DESCRIPTOR_HANDLE depthDSV = aoMgr.GetPrivateDepthDSV();
                 auto *nativeCL = cmdList.Get();

                 {
                     const auto &viewport = m_context->DeviceContext->GetViewport();
                     const auto &scissorRect = m_context->DeviceContext->GetScissorRect();
                     nativeCL->RSSetViewports(1, &viewport);
                     nativeCL->RSSetScissorRects(1, &scissorRect);
                 }

                 const float clearBlue[] = {0.0f, 0.0f, 1.0f, 0.0f};
                 nativeCL->ClearRenderTargetView(normalRTV, clearBlue, 0, nullptr);
                 nativeCL->ClearDepthStencilView(depthDSV, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0,
                                                 0, nullptr);
                 nativeCL->OMSetRenderTargets(1, &normalRTV, TRUE, &depthDSV);

                 if (ID3D12PipelineState *normPSO = aoMgr.GetNormalPipeline()) {
                     nativeCL->SetPipelineState(normPSO);
                     nativeCL->SetGraphicsRootSignature(aoMgr.GetNormalRootSig());

                     D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_context->FrameResourceManager->GetPassCBAddress();
                     nativeCL->SetGraphicsRootConstantBufferView(0, cbAddr);

                     auto &gpuMgr = GpuResourceManager::GetInstance();
                     for (const auto &item : m_opaqueQueue) {
                         if (!item.IsValid())
                             continue;
                         const TriangleMesh *mesh =
                             m_context->GeometryResourceManager->GetGeometry<TriangleMesh>(item.geometryHandle);
                         if (!mesh || !mesh->isGpuReady)
                             continue;

                         ID3D12Resource *vb = gpuMgr.GetResource(mesh->vertexBufferHandle);
                         ID3D12Resource *ib = gpuMgr.GetResource(mesh->indexBufferHandle);
                         if (!vb || !ib)
                             continue;

                         D3D12_VERTEX_BUFFER_VIEW vbv = {vb->GetGPUVirtualAddress(),
                                                         (UINT)(mesh->vertexCount * mesh->vertexStride),
                                                         mesh->vertexStride};
                         D3D12_INDEX_BUFFER_VIEW ibv = {
                             ib->GetGPUVirtualAddress(),
                             (UINT)(mesh->indexCount * (mesh->indexFormat == DXGI_FORMAT_R32_UINT ? 4 : 2)),
                             mesh->indexFormat};
                         nativeCL->IASetVertexBuffers(0, 1, &vbv);
                         nativeCL->IASetIndexBuffer(&ibv);
                         nativeCL->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                         nativeCL->SetGraphicsRootShaderResourceView(1, item.instanceBuffer);
                         nativeCL->DrawIndexedInstanced(mesh->indexCount, item.instanceCount, 0, 0, 0);
                     }
                 }

                 barrier(normalRes, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

                 ID3D12Resource *privateDepthRes = aoMgr.GetPrivateDepthResource();
                 if (privateDepthRes) {
                     barrier(privateDepthRes, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                 }

                 const auto &passCB = m_context->FrameResourceManager->GetPassConstants();
                 aoMgr.Execute(cmdList.Get(), depthSRV, passCB.Proj);

                 if (privateDepthRes) {
                     barrier(privateDepthRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
                 }

                 barrier(ambient0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

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
