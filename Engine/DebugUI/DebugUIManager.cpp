#include "DebugUIManager.h"
#include "Boot/GameContext.h"
#include "Common/WindowsPlatform.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include <filesystem>

// Common
#include "Common/ImGuiWrapper.h"
#include "Common/d3dx12.h"

#include "Boot/GameContext.h"
#include "ECS/Core/Registry.h"
#include "Framework/SystemRegistry.h"
#include "Platform/Windows/Window.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Scheduler/FrameDriver.h"

// 外部声明：ImGui_ImplWin32 需要处理窗口消息
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using namespace DX12Engine::Scheduler;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Boot;
using namespace DX12Engine::Platform;

namespace DX12Engine::DebugUI {

// ========================================================================
// 静态回调函数（用于 ImGui DX12 后端）
// ========================================================================

static void ImGui_AllocDescriptor(ImGui_ImplDX12_InitInfo *info, D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu_handle,
                                  D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu_handle) {
    DebugUIManager *manager = reinterpret_cast<DebugUIManager *>(info->UserData);
    manager->AllocateSrvDescriptor(out_cpu_handle, out_gpu_handle);
}

static void ImGui_FreeDescriptor(ImGui_ImplDX12_InitInfo *info, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                 D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle) {
    DebugUIManager *manager = reinterpret_cast<DebugUIManager *>(info->UserData);
    manager->FreeSrvDescriptor(cpu_handle, gpu_handle); // 传递两个参数
}
// ========================================================================
// 单例实现
// ========================================================================

DebugUIManager &DebugUIManager::Get() {
    static DebugUIManager instance;
    return instance;
}

// ========================================================================
// 生命周期
// ========================================================================

void DebugUIManager::Initialize(HWND hwnd) {
    if (m_initialized)
        return;

    m_hwnd = hwnd;

    // 创建 ImGui 上下文
    m_imguiContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_imguiContext);

    // 初始化 ImGui 配置
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // 键盘导航
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = "imgui.ini"; // 保存布局

    // 应用默认样式
    ApplyDarkTheme();

    // 加载字体（优先使用系统字体以支持中文，回退到默认字体）
    bool fontLoaded = false;
    const char *systemFonts[] = {"C:/Windows/Fonts/msyh.ttc",   // Microsoft YaHei
                                 "C:/Windows/Fonts/simhei.ttf", // SimHei
                                 "C:/Windows/Fonts/msyhbd.ttc", // Microsoft YaHei Bold
                                 "C:/Windows/Fonts/deng.ttf",   // DengXian
                                 nullptr};
    for (int i = 0; systemFonts[i]; i++) {
        if (std::filesystem::exists(systemFonts[i])) {
            io.Fonts->AddFontFromFileTTF(systemFonts[i], 15.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
            fontLoaded = true;
            break;
        }
    }
    if (!fontLoaded) {
        io.Fonts->AddFontDefault();
    }

       // 初始化 Win32 后端
    ImGui_ImplWin32_Init(hwnd);

    m_initialized = true;
}

void DebugUIManager::InitDX12Backend(ID3D12Device *device, ID3D12CommandQueue *commandQueue, uint32_t frameCount,
                                     DXGI_FORMAT rtvFormat) {
    if (!m_initialized)
        return;

    m_frameCount = frameCount;
    m_rtvFormat = rtvFormat;

    // 从 DescriptorHeapCollection 获取 ImGui 专用分区
    if (m_gameContext && m_gameContext->DescriptorHeaps) {
        auto *descHeaps = m_gameContext->DescriptorHeaps;
        m_srvHeap = descHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Resource::HeapTag::ImGui);
        m_srvDescriptorSize =
            descHeaps->GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Resource::HeapTag::ImGui);
        D3D12_CPU_DESCRIPTOR_HANDLE partitionStart =
            descHeaps->GetCpuHandle(Resource::PartitionType::Texture, 0, Resource::HeapTag::ImGui);
        m_descriptorAllocator.Initialize(partitionStart, m_srvDescriptorSize, 2048);
    } else {
        // 无 DescriptorHeapCollection 时回退到自建堆
        CreateSrvDescriptorHeap(device);
        D3D12_CPU_DESCRIPTOR_HANDLE heapStart = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        m_descriptorAllocator.Initialize(heapStart, m_srvDescriptorSize, 1024);
    }

    // 使用新的 ImGui_ImplDX12_InitInfo 结构体初始化
    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device = device;
    initInfo.CommandQueue = commandQueue;
    initInfo.NumFramesInFlight = frameCount;
    initInfo.RTVFormat = rtvFormat;
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.SrvDescriptorHeap = m_srvHeap.Get();
    initInfo.SrvDescriptorAllocFn = ImGui_AllocDescriptor;
    initInfo.SrvDescriptorFreeFn = ImGui_FreeDescriptor;
    initInfo.UserData = this;

    bool result = ImGui_ImplDX12_Init(&initInfo);

    if (!result) {
        // 初始化失败处理
        m_initialized = false;
    }
}

void DebugUIManager::CreateSrvDescriptorHeap(ID3D12Device *device) {
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = 1024; // 增加到 1024，ImGui 可能需要更多描述符
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) {
        m_initialized = false;
        return;
    }
    m_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void DebugUIManager::Shutdown() {
    if (!m_initialized) {
        return;
    }

    ImGui::SetCurrentContext(m_imguiContext);

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(m_imguiContext);

    m_imguiContext = nullptr;
    m_initialized = false;
    m_panels.clear();
    m_panelVisible.clear();
    m_panelGroup.clear();
}

// ========================================================================
// 图标字体合并
// ========================================================================

void DebugUIManager::MergeIconFont(const std::string &ttfPath) {
    if (!m_initialized || ttfPath.empty())
        return;

    ImGuiIO &io = ImGui::GetIO();
    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.GlyphMinAdvanceX = 15.0f;
    static const ImWchar iconRanges[] = {0xe600, 0xec17, 0};

    ImFont *iconFont = io.Fonts->AddFontFromFileTTF(ttfPath.c_str(), 15.0f, &iconConfig, iconRanges);
    if (iconFont) {
        OutputDebugStringA(("[DebugUIManager] iconfont merged: " + ttfPath + "\n").c_str());
    } else {
        OutputDebugStringA(("[DebugUIManager] WARN: iconfont failed: " + ttfPath + "\n").c_str());
    }
}

// ========================================================================
// 描述符分配/释放
// ========================================================================

void DebugUIManager::AllocateSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu_handle,
                                           D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu_handle) {
    std::lock_guard<std::mutex> lock(m_descriptorMutex);
    if (m_descriptorAllocator.Allocate(out_cpu_handle)) {
        // 计算 GPU 描述符句柄
        D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        UINT index = static_cast<UINT>((out_cpu_handle->ptr - cpuStart.ptr) / m_srvDescriptorSize);
        out_gpu_handle->ptr = gpuStart.ptr + index * m_srvDescriptorSize;
    } else {
        // 分配失败，设置无效句柄
        out_cpu_handle->ptr = 0;
        out_gpu_handle->ptr = 0;
    }
}

void DebugUIManager::FreeSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle) {
    std::lock_guard<std::mutex> lock(m_descriptorMutex);
    m_descriptorAllocator.Free(cpu_handle);
}

// ========================================================================
// 面板管理
// ========================================================================

bool DebugUIManager::RegisterPanel(const PanelConfig &config) {
    if (m_panels.find(config.name) != m_panels.end()) {
        return false;
    }

    m_panels[config.name] = config;
    m_panelVisible[config.name] = config.visibleByDefault;
    m_panelGroup[config.name] = config.group;

    return true;
}

void DebugUIManager::RegisterRootDrawCallback(RootDrawFunc func) { m_rootDrawCallback = std::move(func); }

void DebugUIManager::UnregisterPanel(const std::string &name) {
    m_panels.erase(name);
    m_panelVisible.erase(name);
    m_panelGroup.erase(name);
}

void DebugUIManager::SetPanelVisible(const std::string &name, bool visible) {
    auto it = m_panelVisible.find(name);
    if (it != m_panelVisible.end()) {
        it->second = visible;
    }
}

void DebugUIManager::TogglePanelVisible(const std::string &name) {
    auto it = m_panelVisible.find(name);
    if (it != m_panelVisible.end()) {
        it->second = !it->second;
    }
}

bool DebugUIManager::IsPanelVisible(const std::string &name) const {
    auto it = m_panelVisible.find(name);
    return it != m_panelVisible.end() ? it->second : false;
}

// ========================================================================
// 样式配置
// ========================================================================

void DebugUIManager::ApplyDarkTheme() {
    ImGui::SetCurrentContext(m_imguiContext);

    ImGuiStyle &style = ImGui::GetStyle();
    ImVec4 *colors = style.Colors;

    // 经典深色主题（类似 Visual Studio 2019 Dark）
    colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.14f, 0.95f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.12f, 0.14f, 0.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.10f, 0.94f);
    colors[ImGuiCol_Border] = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.35f, 0.35f, 0.38f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.08f, 0.08f, 0.10f, 0.51f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.58f, 0.58f, 0.58f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.58f, 0.58f, 0.58f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.78f, 0.78f, 0.78f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.35f, 0.35f, 0.38f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.26f, 0.26f, 0.28f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.35f, 0.35f, 0.38f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.35f, 0.35f, 0.38f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.26f, 0.26f, 0.28f, 1.00f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.35f, 0.35f, 0.38f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
    colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.27f, 0.70f, 0.90f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.37f, 0.80f, 1.00f, 1.00f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.24f, 0.24f, 0.26f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.26f, 0.28f, 1.00f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.10f, 0.10f, 0.12f, 0.50f);

    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.PopupRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.TabRounding = 3.0f;
}

void DebugUIManager::ApplyLightTheme() {
    ImGui::SetCurrentContext(m_imguiContext);
    ImGui::StyleColorsLight();
}

void DebugUIManager::SetDefaultPanelAlpha(float alpha) {
    ImGui::SetCurrentContext(m_imguiContext);
    ImGuiStyle &style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg].w = alpha;
}

// ========================================================================
// 私有辅助方法
// ========================================================================

void DebugUIManager::DrawMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        // 文件菜单
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                PostQuitMessage(0);
            }
            ImGui::EndMenu();
        }

        // 视图菜单（控制面板显隐）
        if (ImGui::BeginMenu("View")) {
            // 按分组显示面板
            std::unordered_map<std::string, std::vector<std::string>> groupPanels;
            for (const auto &[name, group] : m_panelGroup) {
                groupPanels[group].push_back(name);
            }

            for (const auto &[group, names] : groupPanels) {
                if (ImGui::BeginMenu(group.c_str())) {
                    for (const auto &name : names) {
                        bool visible = IsPanelVisible(name);
                        if (ImGui::MenuItem(name.c_str(), nullptr, &visible)) {
                            SetPanelVisible(name, visible);
                        }
                    }
                    ImGui::EndMenu();
                }
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Show ImGui Demo", nullptr, &m_showDemoWindow)) {
                // 切换 Demo 窗口显隐
            }

            ImGui::EndMenu();
        }

        // 帮助菜单
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {
                // 显示关于对话框
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

void DebugUIManager::DrawPanel(const PanelConfig &config, float deltaTime, uint32_t frameNumber) {
    ImGuiWindowFlags flags = 0;
    if (!config.movable)
        flags |= ImGuiWindowFlags_NoMove;
    if (!config.collapsible)
        flags |= ImGuiWindowFlags_NoCollapse;

    bool open = m_panelVisible[config.name];
    if (ImGui::Begin(config.name.c_str(), config.closable ? &open : nullptr, flags)) {
        if (config.drawFunc) {
            config.drawFunc(deltaTime, frameNumber);
        }
    }
    ImGui::End();

    // 同步可见性状态（用户点击了 X 关闭按钮）
    if (config.closable && m_panelVisible[config.name] != open) {
        m_panelVisible[config.name] = open;
    }
}

void DebugUIManager::DrawDemoWindow() { ImGui::ShowDemoWindow(&m_showDemoWindow); }

void DebugUIManager::RenderAndSubmit(ID3D12GraphicsCommandList *commandList, float deltaTime, uint32_t frameNumber) {
    if (!m_initialized)
        return;

    ImGui::SetCurrentContext(m_imguiContext);

    // 设置描述符堆
    ID3D12DescriptorHeap *heaps[] = {m_srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);

    // 开始新帧
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // 绘制菜单栏和面板
    if (m_showMenuBar)
        DrawMenuBar();
    if (m_showDemoWindow)
        DrawDemoWindow();

    // 绘制根级回调（不包裹在面板窗口中）
    if (m_rootDrawCallback)
        m_rootDrawCallback(deltaTime, frameNumber);

    for (auto &[name, config] : m_panels) {
        if (m_panelVisible[name]) {
            DrawPanel(config, deltaTime, frameNumber);
        }
    }

    // 渲染
    ImGui::Render();

    ImGuiIO &io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
}

void DebugUIManager::AutoRegisterToFrameDriver(Boot::GameContext *context) {
    if (m_registered || !context)
        return;

    m_gameContext = context;

    // 注册常驻渲染 System
    SystemRegistry::Register(
        {.name = "ImGuiRenderSystem",
         .func =
             [this](const MessageContext &) {
                 if (!m_initialized || !m_gameContext)
                     return;

                 uint64_t completed = m_gameContext->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                 auto allocHandle = m_gameContext->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completed);
                 auto allocator = m_gameContext->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle);
                 auto cmdListHandle =
                     m_gameContext->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
                 auto cmdList = m_gameContext->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

                 // ✅ 关键：设置渲染目标和视口
                 auto backBuffer = m_gameContext->GetBackBuffer();

                 // 1. 资源状态转换：PRESENT -> RENDER_TARGET
                 D3D12_RESOURCE_BARRIER barrier = {};
                 barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                 barrier.Transition.pResource = backBuffer;
                 barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                 barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 // 2. 设置视口和裁剪矩形
                 const auto &viewport = m_gameContext->DeviceContext->GetViewport();
                 const auto &scissorRect = m_gameContext->DeviceContext->GetScissorRect();
                 cmdList.Get()->RSSetViewports(1, &viewport);
                 cmdList.Get()->RSSetScissorRects(1, &scissorRect);

                 // 3. 设置渲染目标
                 auto rtvHandle = m_gameContext->DeviceContext->GetCurrentBackBufferView();
                 auto dsvHandle = m_gameContext->DeviceContext->GetDepthStencilView();
                 cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

                 float dt = m_gameContext->MainTimer->GetDeltaTime();
                 uint32_t frame = m_gameContext->FrameDriver->GetFrameStats().frameNumber;

                 // 调用 ImGui 渲染
                 RenderAndSubmit(cmdList.Get(), dt, frame);

                 // 4. 资源状态转换：RENDER_TARGET -> PRESENT
                 barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                 barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                 cmdList.Get()->ResourceBarrier(1, &barrier);

                 cmdList.Close();
                 m_gameContext->FrameDriver->SubmitRenderCommand(RenderPhase::UI, cmdListHandle);

                 uint64_t sequence = m_gameContext->GetNextSequence();
                 m_gameContext->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle, sequence);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Main,
         .renderPhase = RenderPhase::UI,
         .alwaysRun = true});

    m_registered = true;
}
} // namespace DX12Engine::DebugUI