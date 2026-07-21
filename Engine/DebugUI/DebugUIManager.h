#pragma once

#include "ECS/Core/Entity.h"
#include <mutex>
#include <wrl/client.h>

// 前置声明（避免头文件污染）
struct ImGuiContext;
struct ID3D12Device;
struct ID3D12DescriptorHeap;
struct ID3D12GraphicsCommandList;

namespace DX12Engine {

namespace Boot {
class GameContext; // 前置声明
}

namespace DebugUI {

// ========================================================================
// 面板绘制函数类型
// ========================================================================

using PanelDrawFunc = std::function<void(float deltaTime, uint32_t frameNumber)>;
using RootDrawFunc = std::function<void(float deltaTime, uint32_t frameNumber)>;

// ========================================================================
// 面板配置
// ========================================================================

struct PanelConfig {
    std::string name;             // 面板标题（同时也是唯一标识）
    std::string group;            // 分组名称（如 "Performance", "Rendering", "ECS"）
    PanelDrawFunc drawFunc;       // 绘制函数
    bool visibleByDefault = true; // 默认是否可见
    bool closable = true;         // 是否可关闭（右上角 X 按钮）
    bool movable = true;          // 是否可拖动
    bool collapsible = true;      // 是否可折叠
};

// ========================================================================
// DebugUIManager - 调试界面管理器
// ========================================================================

class DebugUIManager {
public:
    // ========================================================================
    // 单例访问
    // ========================================================================

    static DebugUIManager &Get();

    // 禁止拷贝和移动
    DebugUIManager(const DebugUIManager &) = delete;
    DebugUIManager &operator=(const DebugUIManager &) = delete;

    // ========================================================================
    // 生命周期
    // ========================================================================

    void Initialize(HWND hwnd);

    void InitDX12Backend(ID3D12Device *device, ID3D12CommandQueue *commandQueue, uint32_t frameCount,
                         DXGI_FORMAT rtvFormat);

    void Shutdown();

    bool IsInitialized() const { return m_initialized; }
    void SetGameContext(Boot::GameContext *context) { m_gameContext = context; }

    /// 合并图标字体（iconfont），由 Bootstrap 调用，传入已解析的绝对路径
    void MergeIconFont(const std::string &ttfPath);

    // ========================================================================
    // 面板管理
    // ========================================================================

    bool RegisterPanel(const PanelConfig &config);
    void UnregisterPanel(const std::string &name);
    void SetPanelVisible(const std::string &name, bool visible);
    void TogglePanelVisible(const std::string &name);
    bool IsPanelVisible(const std::string &name) const;

    // ========================================================================
    // 根级绘制回调（不包裹在面板窗口中，直接注册在根级）
    // ========================================================================

    void RegisterRootDrawCallback(RootDrawFunc func);

    // ========================================================================
    // 渲染
    // ========================================================================

    void RenderAndSubmit(ID3D12GraphicsCommandList *commandList, float deltaTime, uint32_t frameNumber);

    // ====================================================================
    // 样式配置
    // ====================================================================

    void ApplyDarkTheme();
    void ApplyLightTheme();
    void SetDefaultPanelAlpha(float alpha);
    void SetShowMenuBar(bool show) { m_showMenuBar = show; }
    void SetShowDemoWindow(bool show) { m_showDemoWindow = show; }

    // 获取 DX12 后端需要的描述符堆（供外部使用）
    ID3D12DescriptorHeap *GetSrvDescriptorHeap() const { return m_srvHeap.Get(); }

    void AutoRegisterToFrameDriver(Boot::GameContext *context);

    // 描述符分配接口（供 ImGui 回调使用）
    void AllocateSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu_handle,
                               D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu_handle);
    void FreeSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle);

private:
    DebugUIManager() = default;
    ~DebugUIManager() = default;

    void CreateSrvDescriptorHeap(ID3D12Device *device);
    void DrawMenuBar();
    void DrawPanel(const PanelConfig &config, float deltaTime, uint32_t frameNumber);
    void DrawDemoWindow();

    // 描述符分配器（简单计数器实现）
    struct DescriptorAllocator {
        D3D12_CPU_DESCRIPTOR_HANDLE startHandle = {};
        UINT descriptorSize = 0;
        UINT numDescriptors = 0;
        UINT nextFreeIndex = 0;
        std::vector<bool> used;

        void Initialize(D3D12_CPU_DESCRIPTOR_HANDLE start, UINT size, UINT count) {
            startHandle = start;
            descriptorSize = size;
            numDescriptors = count;
            nextFreeIndex = 0;
            used.resize(count, false);
        }

        bool Allocate(D3D12_CPU_DESCRIPTOR_HANDLE *out_handle) {
            for (UINT i = 0; i < numDescriptors; ++i) {
                if (!used[i]) {
                    used[i] = true;
                    if (i < nextFreeIndex) {
                        nextFreeIndex = i;
                    }
                    out_handle->ptr = startHandle.ptr + i * descriptorSize;
                    return true;
                }
            }
            return false;
        }

        void Free(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
            UINT index = static_cast<UINT>((handle.ptr - startHandle.ptr) / descriptorSize);
            if (index < numDescriptors) {
                used[index] = false;
                if (index < nextFreeIndex) {
                    nextFreeIndex = index;
                }
            }
        }
    };

private:
    std::unordered_map<std::string, PanelConfig> m_panels;
    std::unordered_map<std::string, bool> m_panelVisible;
    std::unordered_map<std::string, std::string> m_panelGroup; // name -> group

    bool m_initialized = false;
    bool m_showMenuBar = true;
    bool m_showDemoWindow = false; // 是否显示 ImGui Demo 窗口

    ImGuiContext *m_imguiContext = nullptr;
    HWND m_hwnd = nullptr;

    Boot::GameContext *m_gameContext = nullptr;
    bool m_registered = false;

    // 根级绘制回调（不包裹在面板窗口中）
    RootDrawFunc m_rootDrawCallback;

    // 自行管理的 SRV 描述符堆
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_srvDescriptorSize = 0;
    uint32_t m_frameCount = 0;
    DXGI_FORMAT m_rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    // 描述符分配器
    DescriptorAllocator m_descriptorAllocator;
    std::mutex m_descriptorMutex;
};

} // namespace DebugUI
} // namespace DX12Engine