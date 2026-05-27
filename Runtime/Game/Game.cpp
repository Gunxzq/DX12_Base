#include "Game.h"
#include "DebugUI/DebugUIManager.h"
#include "Event/MessageDispatcher.h"
#include "Framework/SystemRegistry.h"
#include "Platform/Input/InputSystem.h"
#include "Platform/Windows/Window.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/CameraManager.h"
#include "Resource/AssetLoader/AssetLoader.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Texture/TextureManager.h"
#include "Scheduler/FrameDriver.h"

using namespace DX12Engine;
using namespace DX12Engine::DebugUI;
using namespace DX12Engine::Scheduler;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Input;
using namespace DX12Engine::Math;
using namespace DX12Engine::ECS;

Game::Game(Boot::GameContext *context) : m_context(context) {}

Game::~Game() {
    if (m_isRunning || m_isInitialized) {
        Shutdown();
    }
}

bool Game::Initialize() {
    if (!m_context || !m_context->IsValid()) {
        return false;
    }

    m_context->Logging->Info("[Game] Initializing game...");

    // 1. 初始化 GPU 资源管理器
    Resource::GpuResourceManager::GetInstance().Initialize();

    // 2. 创建渲染器
    m_opaqueRenderer = std::make_unique<OpaqueRenderer>();
    m_opaqueRenderer->SetDeviceContext(m_context->DeviceContext);
    m_opaqueRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_opaqueRenderer->SetMaterialManager(m_context->MaterialMgr);
    m_opaqueRenderer->Initialize();

    m_context->CullingSystem = &m_cullingSystem;
    m_context->LODSystem = &m_lodSystem;
    m_context->RenderItemBuilder = &m_renderItemBuilder;

    // 设置帧驱动器引用
    m_context->RenderItemBuilder->SetFrameResourceManager(m_context->FrameResourceManager);
    m_context->RenderItemBuilder->SetMaterialManager(m_context->MaterialMgr);

    // 4. 初始化相机
    if (m_context->CameraMgr) {
        auto &mainCamera = m_context->CameraMgr->GetMainCamera();
        mainCamera.Position = DirectX::XMFLOAT3(0.0f, 0.0f, -10.0f);
        mainCamera.Rotation = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        m_context->CameraMgr->UpdateMainCamera();
    }

    // 配置 LODSystem
    m_lodSystem.SetLODConfig(LODConfig::GetDefault());
    m_lodSystem.SetCameraManager(m_context->CameraMgr);
    m_lodSystem.SetGeometryManager(m_context->GeometryResourceManager);

    // 配置 RenderItemBuilder
    m_renderItemBuilder.SetCameraManager(m_context->CameraMgr);

    // 初始化 LightManager
    m_lightManager.Initialize();
    m_lightManager.CreateTestLights(); // 创建测试光源

    // 5. 注册引擎级系统（窗口大小变化、全屏切换等）
    RegisterEngineSystems();

    // 6. 初始化游戏模块（它们会自己注册游戏逻辑系统）
    m_world.Initialize(m_context, m_opaqueRenderer.get());
    m_world.CreateTestCube();

    m_inputHandler.Initialize(m_context);

    // 7. 注册相机更新回调（Immediate 路径）
    if (m_context->FrameDriver) {
        m_context->FrameDriver->RegisterImmediateCallback(
            [this]() {
                m_context->CameraMgr->UpdateMainCamera();

                const auto &camera = m_context->CameraMgr->GetMainCamera();
                auto &passConstants = m_context->FrameResourceManager->GetPassConstants();

                // 存储矩阵
                XMStoreFloat4x4(&passConstants.View, camera.ViewMatrix);
                XMStoreFloat4x4(&passConstants.Proj, camera.ProjMatrix);
                XMStoreFloat4x4(&passConstants.ViewProj, camera.ViewProjMatrix);
                XMStoreFloat4x4(&passConstants.InvViewProj, camera.InverseViewProj);

                // 存储其他数据
                passConstants.CameraPos = camera.Position;
                passConstants.TotalTime = static_cast<float>(m_context->MainTimer->GetGameTime());
                passConstants.DeltaTime = m_context->MainTimer->GetDeltaTime();
                passConstants.NearPlane = camera.NearPlane;
                passConstants.FarPlane = camera.FarPlane;
                passConstants.AspectRatio = camera.AspectRatio;
                passConstants.FrameCount = m_context->FrameDriver->GetFrameStats().frameNumber;
                passConstants.AmbientLight = {0.5f, 0.5f, 0.5f, 0.8f}; // 提高环境光强度

                // ========================================================================
                // 更新跟随相机的光源位置（点光源索引 2）
                // ========================================================================
                Light *followLight = m_lightManager.GetPointLight(2);
                if (followLight) {
                    followLight->Position =
                        DirectX::XMFLOAT4(camera.Position.x, camera.Position.y + 1.0f, camera.Position.z, 0.0f);
                }

                // ========================================================================
                // 上传光源数据到 GPU
                // ========================================================================
                D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress =
                    m_lightManager.UpdateAndUpload(m_context->FrameResourceManager);
                m_context->lightCBAddress = lightCBAddress;

                m_context->FrameResourceManager->UpdatePassConstants();
            },
            "CameraUpdate");
    }

    // 8. 注册调试 UI 面板
    DebugUIManager::Get().RegisterPanel(
        {.name = "Performance", .group = "Performance", .drawFunc = [this](float dt, uint32_t frame) {
             static int targetFPS = 60;
             if (ImGui::SliderInt("Target FPS", &targetFPS, 0, 240)) {
                 m_context->FrameDriver->SetTargetFPS(targetFPS);
             }

             static bool fullscreen = false;
             if (ImGui::Checkbox("Fullscreen", &fullscreen)) {
                 auto *dispatcher = Event::MessageDispatcher::GetInstance();
                 if (dispatcher) {
                     dispatcher->PostEvent(Event::FullscreenToggleEvent::StaticTypeHash, 0, fullscreen ? 1u : 0u,
                                           Event::EventPriority::P1_High);
                 }
             }

             ImGui::Text("Game Delta Time: %.3f ms", dt * 1000.0f);
             ImGui::Text("Raw Delta Time: %.3f ms", m_context->MainTimer->GetRawDeltaTime() * 1000.0f);
             ImGui::Text("Raw FPS: %.1f", 1.0f / m_context->MainTimer->GetRawDeltaTime());
             ImGui::Text("FrameDriver Target: %d FPS", m_context->FrameDriver->GetTargetFPS());

             float rawFPS = 1.0f / m_context->MainTimer->GetRawDeltaTime();
             if (rawFPS > 62.0f && dt * 1000.0f > 15.5f) {
                 ImGui::TextColored(ImVec4(1, 1, 0, 1), "⚠️ Limited by DWM (windowed mode)");
             }
         }});

    m_isInitialized = true;
    m_context->Logging->Info("[Game] Game initialized successfully");

    // 加载测试纹理
    {
        using namespace DX12Engine::Resource;

        // 1. 解析 DDS 文件
        DDSTextureInfo ddsInfo;
        std::wstring texturePath = L"Content/Textures/water1.dds";
        if (AssetLoader::GetInstance().LoadTextureFromFile(texturePath, ddsInfo)) {

            // 2. 创建 GPU 资源（使用 COMMON 状态，不是 COPY_DEST）
            auto &gpuMgr = GpuResourceManager::GetInstance();
            ID3D12Device *device = m_context->DeviceContext->GetDevice();
            GpuResourceHandle gpuHandle = gpuMgr.CreateTexture2D(device, ddsInfo.desc, D3D12_RESOURCE_STATE_COMMON);

            if (gpuHandle.IsValid()) {
                // 3. 分配 SRV 描述符
                auto &descriptorHeaps = m_context->DescriptorHeaps;
                uint32_t srvIndex = descriptorHeaps->Allocate(DescriptorHeapType::CbvSrvUav);

                if (srvIndex != UINT32_MAX) {
                    // 4. 创建 SRV
                    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Format = ddsInfo.desc.Format;
                    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

                    if (ddsInfo.isCubeMap) {
                        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                        srvDesc.TextureCube.MipLevels = ddsInfo.desc.MipLevels;
                        srvDesc.TextureCube.MostDetailedMip = 0;
                        srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
                    } else {
                        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        srvDesc.Texture2D.MipLevels = ddsInfo.desc.MipLevels;
                        srvDesc.Texture2D.MostDetailedMip = 0;
                        srvDesc.Texture2D.PlaneSlice = 0;
                        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
                    }

                    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
                        descriptorHeaps->GetCpuHandle(DescriptorHeapType::CbvSrvUav, srvIndex);
                    device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);

                    // 保存 GPU 句柄
                    m_context->testTextureSRVHandle =
                        descriptorHeaps->GetGpuHandle(DescriptorHeapType::CbvSrvUav, srvIndex);

                    // 5. 注册到 TextureManager
                    auto &texMgr = TextureManager::GetInstance();
                    m_context->testTextureHandle = texMgr.RegisterTexture(gpuHandle, srvIndex);

                    // ========== 6. 同步上传纹理数据 ==========
                    uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                    auto allocatorHandle =
                        m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
                    auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
                    auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                    auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                    // 准备子资源数据
                    std::vector<D3D12_SUBRESOURCE_DATA> subresources;
                    uint32_t skipMip;
                    DDSLoader::FillSubresourceData(ddsInfo.pixelData, ddsInfo.pixelDataSize, ddsInfo, 0, subresources,
                                                   skipMip);

                    // 计算所需上传缓冲区大小

                    UINT64 requiredSize = GetRequiredIntermediateSize(gpuMgr.GetResource(gpuHandle), 0,
                                                                      static_cast<UINT>(subresources.size()));

                    // 创建上传缓冲区
                    GpuResourceHandle uploadHandle = gpuMgr.CreateBuffer(device, requiredSize, D3D12_HEAP_TYPE_UPLOAD,
                                                                         D3D12_RESOURCE_STATE_GENERIC_READ);

                    // 屏障：COMMON -> COPY_DEST
                    auto barrier1 = CD3DX12_RESOURCE_BARRIER::Transition(
                        gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                    cmdList.Get()->ResourceBarrier(1, &barrier1);

                    // 更新子资源
                    UpdateSubresources(cmdList.Get(), gpuMgr.GetResource(gpuHandle), gpuMgr.GetResource(uploadHandle),
                                       0, 0, static_cast<UINT>(subresources.size()), subresources.data());

                    // 屏障：COPY_DEST -> PIXEL_SHADER_RESOURCE
                    auto barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle),
                                                                         D3D12_RESOURCE_STATE_COPY_DEST,
                                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    cmdList.Get()->ResourceBarrier(1, &barrier2);

                    cmdList.Close();

                    // 提交并刷新
                    m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList);
                    m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);

                    uint64_t sequence = m_context->GetNextSequence();

                    // 释放上传缓冲区
                    // gpuMgr.Release(uploadHandle, sequence);

                    // 释放临时资源
                    m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);
                    m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);

                    m_context->Logging->Info("[Game] Test texture loaded successfully");
                } else {
                    gpuMgr.Release(gpuHandle, 0);
                    OutputDebugString(L"Failed to allocate SRV");
                }
            } else {
                OutputDebugString(L"Failed to create GPU texture");
            }
        } else {
            OutputDebugString(L"Failed to load DDS file");
        }
    }
}

void Game::RegisterEngineSystems() {
    // WindowResizeSystem
    SystemRegistry::Register({.name = "WindowResizeSystem",
                              .func =
                                  [this](Registry &, const MessageContext &ctx) {
                                      uint32_t width = ctx.GetLow32();
                                      uint32_t height = ctx.GetHigh32();
                                      if (m_context && m_context->DeviceContext) {
                                          m_context->DeviceContext->OnResize(width, height);
                                      }
                                      if (m_context && m_context->CameraMgr) {
                                          m_context->CameraMgr->OnResize(width, height);
                                      }
                                      if (m_opaqueRenderer) {
                                          m_opaqueRenderer->OnResize(width, height);
                                      }
                                  },
                              .phase = TaskPhase::EarlyUpdate,
                              .threadType = ThreadType::Main,
                              .interestedMessages = {Event::WindowResizeEvent::StaticTypeHash}});

    // FullscreenSystem
    SystemRegistry::Register({.name = "FullscreenSystem",
                              .func =
                                  [this](Registry &, const MessageContext &ctx) {
                                      bool bRequestFullscreen = ctx.GetLow32() != 0;
                                      m_context->Window->SetFullscreen(bRequestFullscreen);
                                  },
                              .phase = TaskPhase::EarlyUpdate,
                              .threadType = ThreadType::Main,
                              .priority = TaskPriority::High,
                              .interestedMessages = {Event::FullscreenToggleEvent::StaticTypeHash}});

    SystemRegistry::Register({.name = "PreRenderPipeline",
                              .func =
                                  [this](Registry &reg, const MessageContext &ctx) {
                                      // 串行执行三个步骤
                                      m_context->CullingSystem->Execute(reg, m_context->cullingResult);
                                      m_context->LODSystem->Execute(reg, m_context->lodResult);
                                      m_context->RenderItemBuilder->Execute(
                                          reg, m_context->cullingResult, m_context->lodResult, m_context->renderQueue);

                                      m_context->renderQueue.Sort();
                                  },
                              .phase = TaskPhase::PreRender,
                              .threadType = ThreadType::Worker,
                              .alwaysRun = true});
}

int Game::Run() {
    if (!m_isInitialized || !m_context->FrameDriver) {
        return -1;
    }

    m_context->Logging->Info("[Game] Starting game loop...");
    m_isRunning = true;
    m_context->Window->Show();
    m_context->FrameDriver->Initialize();

    while (m_isRunning && !m_context->Window->ShouldClose()) {
        m_context->MainTimer->Tick();
        m_context->FrameDriver->Tick();
    }

    m_context->FrameDriver->Stop();
    Shutdown();
    return 0;
}

void Game::Shutdown() {
    if (!m_isInitialized && !m_isRunning)
        return;

    m_isRunning = false;

    if (m_context) {
        m_context->FlushAllQueues();
    }

    m_world.Clear();
    SystemRegistry::Clear();

    if (m_context) {
        Resource::GpuResourceManager::GetInstance().Update(m_context->GetFenceValue());
    }

    Resource::GpuResourceManager::GetInstance().Shutdown();

    m_isInitialized = false;
    m_context->Logging->Info("[Game] Game shutdown complete");
}