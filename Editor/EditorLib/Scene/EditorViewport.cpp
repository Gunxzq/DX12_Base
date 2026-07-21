#include "EditorViewport.h"
#include "Common/d3dUtil.h"
#include "DebugUI/DebugUIManager.h"
#include "Framework/SystemRegistry.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/Pipeline/SkyRenderer.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/GridManager.h"
#include "Renderer/Scene/GridRenderer.h"
#include "Renderer/Scene/SkyboxManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Pool/RenderTargetPool.h"
#include "Scheduler/FrameDriver.h"
#include "Scheduler/RenderPhase.h"
#include "ThirdParty/imgui/imgui.h"

#include <d3d12.h>
#include <d3dcompiler.h>

using namespace DX12Engine;
using namespace DX12Engine::Resource;
using namespace DX12Engine::Scheduler;

// ========================================================================
// 构造/析构
// ========================================================================

EditorViewport::EditorViewport(Boot::GameContext *context) : m_context(context) {}

EditorViewport::~EditorViewport() { Shutdown(); }

// ========================================================================
// 初始化
// ========================================================================

bool EditorViewport::Initialize() {
    if (!m_context || !m_context->DescriptorHeaps) {
        return false;
    }

    m_context->Logging->Info("[EditorViewport] Initializing...");

    // 为 EditorViewport 堆创建 RTV/DSV 分区（仅初始化时一次，OnResize 不重复）
    // 注意：CBV_SRV_UAV / Texture 分区已在 Bootstrap::InitializeModules() 中
    // 为多堆模式预先创建（isEditor && Multi），此处不重复注册
    m_context->DescriptorHeaps->AddPartition(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, PartitionType::Rtv, 0, 64,
                                             HeapTag::EditorViewport);
    m_context->DescriptorHeaps->AddPartition(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, PartitionType::Dsv, 0, 64,
                                             HeapTag::EditorViewport);

    // 创建 ApplicationRenderTargets（G-buffer 管理）
    m_appRTs = std::make_unique<Renderer::ApplicationRenderTargets>();
    m_appRTs->Initialize(m_context->DeviceContext->GetDevice(), m_context->DescriptorHeaps, 1280, 720,
                         HeapTag::EditorViewport);

    if (!CreateRenderTarget(1280, 720)) {
        m_context->Logging->Error("[EditorViewport] Failed to create render target");
        return false;
    }

    if (!CreateRenderResources()) {
        m_context->Logging->Error("[EditorViewport] Failed to create render resources");
        return false;
    }

    RegisterRenderSystems();

    m_initialized = true;
    m_context->Logging->Info("[EditorViewport] Initialized");
    return true;
}

// ========================================================================
// 离屏 RT + 深度缓冲
// ========================================================================

bool EditorViewport::CreateRenderTarget(uint32_t width, uint32_t height) {
    if (!m_context || !m_context->DescriptorHeaps || !m_appRTs)
        return false;

    auto *device = m_context->DeviceContext->GetDevice();
    auto &rtPool = RenderTargetPool::GetInstance();
    auto &dsPool = DepthStencilPool::GetInstance();

    // 保存旧尺寸，失败时恢复
    uint32_t oldWidth = m_width;
    uint32_t oldHeight = m_height;

    DestroyRenderTarget();

    // 通过 ApplicationRenderTargets 重建 G-buffer RT
    m_appRTs->OnResize(width, height);
    if (!m_appRTs->IsInitialized()) {
        m_width = oldWidth;
        m_height = oldHeight;
        return false;
    }

    // 缓存 sceneColor RTV 句柄（供渲染 System 绑定）
    m_rtvHandle = rtPool.GetRtvHandle(m_appRTs->GetSceneColor());
    if (m_rtvHandle.ptr == 0) {
        m_width = oldWidth;
        m_height = oldHeight;
        return false;
    }

    // 深度缓冲（使用与主交换链一致的深度格式，避免 PSO 格式不匹配）
    DXGI_FORMAT depthFormat = m_context->DeviceContext->GetDepthStencilFormat();
    DepthStencilDesc depthDesc = {};
    depthDesc.width = width;
    depthDesc.height = height;
    depthDesc.format = depthFormat;
    depthDesc.arraySize = 1;
    depthDesc.sampleDesc.Count = 1;
    depthDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    depthDesc.clearValue.Format = depthFormat;
    depthDesc.clearValue.DepthStencil.Depth = 1.0f;
    depthDesc.clearValue.DepthStencil.Stencil = 0;
    depthDesc.name = L"EditorViewport_Depth";

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = depthFormat;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

    m_depthHandle = dsPool.Allocate(depthDesc, HeapTag::EditorViewport, &dsvDesc);
    if (!m_depthHandle.IsValid()) {
        m_width = oldWidth;
        m_height = oldHeight;
        return false;
    }

    // 缓存 DSV 句柄
    m_dsvHandle = dsPool.GetDsvHandle(m_depthHandle);
    if (m_dsvHandle.ptr == 0) {
        m_width = oldWidth;
        m_height = oldHeight;
        return false;
    }

    // 输出 SRV（ImGui 用，通过 DebugUIManager 分配与释放）
    auto &debugUI = DX12Engine::DebugUI::DebugUIManager::Get();
    debugUI.AllocateSrvDescriptor(&m_srvCpu, &m_srvGpu);
    if (m_srvCpu.ptr == 0 || m_srvGpu.ptr == 0) {
        m_width = oldWidth;
        m_height = oldHeight;
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    ID3D12Resource *sceneColorRes = rtPool.GetResource(m_appRTs->GetSceneColor());
    device->CreateShaderResourceView(sceneColorRes, &srvDesc, m_srvCpu);
    m_outputSRV = m_srvGpu;

    m_width = width;
    m_height = height;
    return true;
}

void EditorViewport::DestroyRenderTarget() {
    auto &dsPool = DepthStencilPool::GetInstance();
    uint64_t fence = m_context ? m_context->GetNextFence() : ~0ull;

    // 释放深度缓冲
    if (m_depthHandle.IsValid()) {
        dsPool.Free(m_depthHandle, fence);
        m_depthHandle = {};
    }

    // 释放 ImGui SRV（通过 DebugUIManager）
    if (m_srvCpu.ptr != 0 && m_srvGpu.ptr != 0) {
        DX12Engine::DebugUI::DebugUIManager::Get().FreeSrvDescriptor(m_srvCpu, m_srvGpu);
    }

    m_rtvHandle = {};
    m_dsvHandle = {};
    m_srvCpu = {};
    m_srvGpu = {};
    m_outputSRV = {};
    m_width = m_height = 0;
}

// ========================================================================
// 渲染器资源
// ========================================================================

bool EditorViewport::CreateRenderResources() {
    m_gridRenderer = std::make_unique<Renderer::GridRenderer>();
    m_gridRenderer->SetDeviceContext(m_context->DeviceContext);
    m_gridRenderer->Initialize();

    // SkyRenderer
    m_skyRenderer = std::make_unique<Renderer::SkyRenderer>();
    m_skyRenderer->SetDeviceContext(m_context->DeviceContext);
    m_skyRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_skyRenderer->Initialize();

    m_context->Logging->Info("[EditorViewport] Render resources created");
    return true;
}

void EditorViewport::DestroyRenderResources() {
    m_skyRenderer.reset();
    if (m_gridRenderer) {
        m_gridRenderer.reset();
    }
}

// ========================================================================
// 注册渲染 System（Skybox + Grid 独立 System，共享视口资源）
// ========================================================================

void EditorViewport::RegisterRenderSystems() {
    // ── EditorViewportClearSystem：清除离屏 RT + depth，设置视口 ──
    SystemRegistry::Register(
        {.name = "EditorViewportClearSystem",
         .func =
             [this](const MessageContext &) {
                 if (!m_appRTs || !m_appRTs->IsInitialized() || !m_depthHandle.IsValid() || m_rtvHandle.ptr == 0 ||
                     m_dsvHandle.ptr == 0)
                     return;

                 auto &rtPool = RenderTargetPool::GetInstance();
                 auto &dsPool = DepthStencilPool::GetInstance();
                 ID3D12Resource *colorRes = rtPool.GetResource(m_appRTs->GetSceneColor());
                 ID3D12Resource *depthRes = dsPool.GetResource(m_depthHandle);
                 if (!colorRes || !depthRes)
                     return;

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto *alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle);
                 auto cmdHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdHandle);

                 // Barrier: COMMON → RENDER_TARGET / DEPTH_WRITE
                 auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(colorRes, D3D12_RESOURCE_STATE_COMMON,
                                                                     D3D12_RESOURCE_STATE_RENDER_TARGET);
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 auto depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(depthRes, D3D12_RESOURCE_STATE_COMMON,
                                                                          D3D12_RESOURCE_STATE_DEPTH_WRITE);
                 cmdList.Get()->ResourceBarrier(1, &depthBarrier);

                 // Clear RT + depth
                 const float clearColor[] = {0.0f, 0.0f, 0.0f, 0.0f};
                 cmdList.Get()->ClearRenderTargetView(m_rtvHandle, clearColor, 0, nullptr);
                 cmdList.Get()->ClearDepthStencilView(m_dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                                      1.0f, 0, 0, nullptr);

                 // 设置视口 + 裁剪矩形 + 渲染目标
                 D3D12_VIEWPORT vp = {0, 0, (float)m_width, (float)m_height, 0, 1};
                 D3D12_RECT sr = {0, 0, (LONG)m_width, (LONG)m_height};
                 cmdList.Get()->RSSetViewports(1, &vp);
                 cmdList.Get()->RSSetScissorRects(1, &sr);
                 cmdList.Get()->OMSetRenderTargets(1, &m_rtvHandle, FALSE, &m_dsvHandle);

                 // Barrier 回退: RENDER_TARGET → COMMON / DEPTH_WRITE → COMMON
                 auto barrierBack = CD3DX12_RESOURCE_BARRIER::Transition(colorRes, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                         D3D12_RESOURCE_STATE_COMMON);
                 cmdList.Get()->ResourceBarrier(1, &barrierBack);

                 auto depthBarrierBack = CD3DX12_RESOURCE_BARRIER::Transition(
                     depthRes, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COMMON);
                 cmdList.Get()->ResourceBarrier(1, &depthBarrierBack);

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::PrePass, cmdHandle);
                 uint64_t seq = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::PrePass,
         .alwaysRun = true});

    // ── SkyboxRenderSystem（仅渲染天空盒，不清除） ──
    SystemRegistry::Register(
        {.name = "EditorSkyboxRenderSystem",
         .func =
             [this](const MessageContext &) {
                 if (!m_appRTs || !m_appRTs->IsInitialized() || !m_skyRenderer || !m_context->CameraMgr ||
                     m_rtvHandle.ptr == 0) {
                     m_context->Logging->Warn(
                         "[EditorSkyboxRenderSystem] Skipped: appRTs={} skyRenderer={} cameraMgr={}",
                         m_appRTs && m_appRTs->IsInitialized(), m_skyRenderer != nullptr,
                         m_context->CameraMgr != nullptr);
                     return;
                 }
                 auto &rtPool = RenderTargetPool::GetInstance();
                 auto &dsPool = DepthStencilPool::GetInstance();
                 auto *heaps = m_context->DescriptorHeaps;
                 ID3D12Resource *colorRes = rtPool.GetResource(m_appRTs->GetSceneColor());
                 ID3D12Resource *depthRes = dsPool.GetResource(m_depthHandle);
                 if (!colorRes || !depthRes)
                     return;

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto *alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle);
                 auto cmdHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdHandle);

                 ID3D12DescriptorHeap *descHeaps[] = {
                     heaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, HeapTag::EditorViewport),
                     heaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, HeapTag::EditorViewport)};
                 cmdList.Get()->SetDescriptorHeaps(2, descHeaps);

                 auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(colorRes, D3D12_RESOURCE_STATE_COMMON,
                                                                     D3D12_RESOURCE_STATE_RENDER_TARGET);
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 auto depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(depthRes, D3D12_RESOURCE_STATE_COMMON,
                                                                          D3D12_RESOURCE_STATE_DEPTH_WRITE);
                 cmdList.Get()->ResourceBarrier(1, &depthBarrier);

                 // 绑定渲染目标（每个 System 自己的命令列表需要重新绑定）
                 D3D12_VIEWPORT vp = {0, 0, (float)m_width, (float)m_height, 0, 1};
                 D3D12_RECT sr = {0, 0, (LONG)m_width, (LONG)m_height};
                 cmdList.Get()->RSSetViewports(1, &vp);
                 cmdList.Get()->RSSetScissorRects(1, &sr);
                 cmdList.Get()->OMSetRenderTargets(1, &m_rtvHandle, FALSE, &m_dsvHandle);

                 // 天空盒（SRV 已在 EditorViewport 堆域，由 SkyboxManager 创建）
                 auto &skyMgr = Renderer::SkyboxManager::GetInstance();
                 if (skyMgr.IsValid()) {
                     D3D12_GPU_VIRTUAL_ADDRESS passCBAddr = m_context->FrameResourceManager->GetPassCBAddress();
                     m_skyRenderer->BeginFrame(cmdList, passCBAddr, skyMgr.GetCubeSRV());
                     m_skyRenderer->DrawSky(cmdList, skyMgr.GetGeometry(), skyMgr.GetObjectCBAddress());
                     m_skyRenderer->EndFrame();
                 } else {
                     m_context->Logging->Warn(
                         "[EditorSkyboxRenderSystem] SkyboxManager not valid yet (async load pending)");
                     return;
                 }

                 auto barrierBack = CD3DX12_RESOURCE_BARRIER::Transition(colorRes, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                         D3D12_RESOURCE_STATE_COMMON);
                 cmdList.Get()->ResourceBarrier(1, &barrierBack);

                 auto depthBarrierBack = CD3DX12_RESOURCE_BARRIER::Transition(
                     depthRes, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COMMON);
                 cmdList.Get()->ResourceBarrier(1, &depthBarrierBack);

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::PostProcess, cmdHandle);
                 uint64_t seq = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::PostProcess,
         .alwaysRun = true});

    // ── GridRenderSystem ──
    SystemRegistry::Register(
        {.name = "EditorGridRenderSystem",
         .func =
             [this](const MessageContext &) {
                 if (!m_appRTs || !m_appRTs->IsInitialized() || !m_gridRenderer || !m_context->CameraMgr ||
                     m_rtvHandle.ptr == 0)
                     return;

                 auto &rtPool = RenderTargetPool::GetInstance();
                 auto &dsPool = DepthStencilPool::GetInstance();
                 auto *heaps = m_context->DescriptorHeaps;
                 ID3D12Resource *colorRes = rtPool.GetResource(m_appRTs->GetSceneColor());
                 ID3D12Resource *depthRes = dsPool.GetResource(m_depthHandle);
                 if (!colorRes || !depthRes)
                     return;

                 uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                 auto *alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle);
                 auto cmdHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                 auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdHandle);

                 ID3D12DescriptorHeap *descHeaps[] = {
                     heaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, HeapTag::EditorViewport),
                     heaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, HeapTag::EditorViewport)};
                 cmdList.Get()->SetDescriptorHeaps(2, descHeaps);

                 auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(colorRes, D3D12_RESOURCE_STATE_COMMON,
                                                                     D3D12_RESOURCE_STATE_RENDER_TARGET);
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 // 深度缓冲已在 DEPTH_WRITE 状态（由 Opaque pass 保留），无需再 transition
                 D3D12_VIEWPORT vp = {0, 0, (float)m_width, (float)m_height, 0, 1};
                 D3D12_RECT sr = {0, 0, (LONG)m_width, (LONG)m_height};
                 cmdList.Get()->RSSetViewports(1, &vp);
                 cmdList.Get()->RSSetScissorRects(1, &sr);
                 cmdList.Get()->OMSetRenderTargets(1, &m_rtvHandle, FALSE, &m_dsvHandle);

                 const auto &camera = m_context->CameraMgr->GetMainCamera();
                 auto &gridMgr = Renderer::GridManager::GetInstance();
                 gridMgr.SetCameraPosition(camera.Position.x, camera.Position.z);
                 m_gridRenderer->Draw(cmdList.Get(), camera.ViewProjMatrix, camera.Position);

                 auto barrierBack = CD3DX12_RESOURCE_BARRIER::Transition(colorRes, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                         D3D12_RESOURCE_STATE_COMMON);
                 cmdList.Get()->ResourceBarrier(1, &barrierBack);

                 auto depthBarrierBack = CD3DX12_RESOURCE_BARRIER::Transition(
                     depthRes, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COMMON);
                 cmdList.Get()->ResourceBarrier(1, &depthBarrierBack);

                 cmdList.Close();
                 m_context->FrameDriver->SubmitRenderCommand(RenderPhase::PostProcess, cmdHandle);
                 uint64_t seq = m_context->GetNextSequence();
                 m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle, seq);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Render,
         .priority = TaskPriority::Normal,
         .renderPhase = RenderPhase::PostProcess,
         .alwaysRun = true});
}

// ========================================================================
// 视口尺寸变化
// ========================================================================

void EditorViewport::OnResize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)
        return;
    if (width == m_width && height == m_height) {
        // 尺寸已稳定，清除 pending
        m_pendingWidth = 0;
        m_pendingHeight = 0;
        return;
    }

    // 防抖：仅当连续两次请求同一尺寸时才执行实际重建
    if (width == m_pendingWidth && height == m_pendingHeight) {
        m_pendingWidth = 0;
        m_pendingHeight = 0;
        m_context->Logging->Info("[EditorViewport] OnResize: {}x{}", width, height);
        if (!CreateRenderTarget(width, height)) {
            m_context->Logging->Error("[EditorViewport] OnResize FAILED for {}x{}", width, height);
        }
    } else {
        // 第一次请求，记录 pending，等待下一帧确认
        m_pendingWidth = width;
        m_pendingHeight = height;
    }
}

// ========================================================================
// 关闭
// ========================================================================

void EditorViewport::Shutdown() {
    if (!m_initialized)
        return;
    DestroyRenderResources();
    DestroyRenderTarget();
    m_appRTs.reset();
    m_initialized = false;
    m_context->Logging->Info("[EditorViewport] Shutdown");
}