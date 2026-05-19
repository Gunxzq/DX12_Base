#pragma once
#include "RenderPhase.h"
#include "Renderer/Core/Command/CommandList/CommandListPool.h"
#include "System/ECS/Registry.h"

#include "TaskExecutor.h"
#include "TaskGraphBuilder.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace DX12Engine {
namespace Core {
class GameContext; // 前置声明
}
namespace Renderer {
class D3D12DeviceContext;
class CommandManager;
class CommandList;
} // namespace Renderer

} // namespace DX12Engine

namespace DX12Engine::Scheduler {

// ========================================================================
// 帧统计信息
// ========================================================================

struct FrameStats {
    uint32_t frameNumber = 0;
    uint32_t taskCount = 0;
    uint32_t activeEntities = 0;

    float cpuLogicTime = 0.0f;
    float cpuRenderTime = 0.0f;
    float gpuTime = 0.0f;
};

// ========================================================================
// 帧同步回调类型
// ========================================================================

/**
 * @brief L4 层帧同步回调函数类型
 *
 * L4 层通过注册此回调，在帧同步点执行自己的多缓冲交换逻辑。
 * 这是 L4 层管理多缓冲的"钩子"。
 */
using FrameSyncCallback = std::function<void()>;

// ========================================================================
// 帧驱动器 (Frame Driver)
// ========================================================================

/**
 * @brief L3 调度层核心：帧循环控制器
 *
 * 职责：
 * 1. 驱动主循环：EarlyUpdate -> Update -> LateUpdate -> Render
 * 2. **帧同步回调**：在逻辑帧结束、渲染帧开始前，调用 L4 注册的回调
 * 3. 时间管理：DeltaTime、固定时间步长
 * 4. 线程协调：主线程 vs 渲染线程的同步
 *
 * 执行流程（每帧）：
 * ```
 * EarlyUpdate Phase:
 *   - 执行输入、网络接收任务
 *   - 屏障同步
 *
 * Update Phase:
 *   - 执行 Gameplay、Physics 任务（并行）
 *   - 屏障同步
 *
 * LateUpdate Phase:
 *   - 执行动画、Transform 计算
 *   - 屏障同步
 *
 * PreRender Phase:
 *   - 执行视锥剔除、LOD
 *   - 屏障同步
 *
 * Frame Sync (关键):
 *   - 调用 L4 注册的 FrameSyncCallback
 *   - L4 在此执行自己的多缓冲交换（如 TransformBuffer.Swap()）
 *
 * Render Phase:
 *   - 渲染线程读取 L4 管理的 Front Buffer
 *   - Present
 * ```
 *
 * @note L3 不管理多缓冲，只提供同步时机。多缓冲由 L4 层自行实现。
 */
class FrameDriver {
public:
    explicit FrameDriver(ECS::Registry &registry);
    ~FrameDriver();

    // 禁止拷贝移动
    FrameDriver(const FrameDriver &) = delete;
    FrameDriver &operator=(const FrameDriver &) = delete;
    FrameDriver(FrameDriver &&) = delete;
    FrameDriver &operator=(FrameDriver &&) = delete;

    /// 初始化（设置线程数等）
    void Initialize(uint32_t workerThreadCount = 0); // 0 = 自动

    /// 运行一帧（非阻塞，返回是否继续）
    bool Tick();

    /// 请求停止主循环
    void Stop() { m_running = false; }

    /// 是否正在运行
    bool IsRunning() const { return m_running; }

    /// 获取当前帧统计
    const FrameStats &GetFrameStats() const { return m_stats; }

    /// 设置目标帧率（0 = 不限制）
    void SetTargetFPS(uint32_t fps) { m_targetFPS = fps; }

    /// 设置 D3D12 设备上下文（由 Bootstrap 在初始化时注入）
    void SetDeviceContext(DX12Engine::Renderer::D3D12DeviceContext *deviceContext) { m_deviceContext = deviceContext; }

    void SetGameContext(DX12Engine::Core::GameContext *context) { m_gameContext = context; }

    /// 获取 GameContext 指针
    DX12Engine::Core::GameContext *GetGameContext() const { return m_gameContext; }

    /// 获取 D3D12 设备上下文
    DX12Engine::Renderer::D3D12DeviceContext *GetDeviceContext() const { return m_deviceContext; }

    /// 获取命令管理器（通过 DeviceContext 访问）
    DX12Engine::Renderer::CommandManager *GetCommandManager() const;

    /// 获取任务执行器（供 L4 层提交任务）
    TaskExecutor &GetExecutor() { return m_executor; }

    /// 获取任务图（供 L4 层注册 System）
    TaskGraph &GetTaskGraph() { return m_taskGraph; }

    // ========================================================================
    // 渲染阶段管理（新增）
    // ========================================================================

    /**
     * @brief 提交一个已录制的命令列表句柄到指定渲染阶段
     */
    void SubmitRenderCommand(
        RenderPhase phase,
        const typename DX12Engine::Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle &handle);

    // ========================================================================
    // L4 层回调注册（多缓冲交换钩子）
    // ========================================================================

    /**
     * @brief 注册帧同步回调
     *
     * L4 层通过此接口注册自己的多缓冲交换逻辑。
     * 回调在逻辑帧结束、渲染帧开始前调用。
     *
     * @param callback 回调函数
     * @param name 回调名称（用于调试）
     * @return 回调 ID（用于注销）
     *
     * 使用示例：
     * @code
     * // L4 层定义多缓冲管理器
     * class TransformBuffer {
     * public:
     *     void Swap() { m_frontIsA = !m_frontIsA; }
     * };
     *
     * // 注册到 FrameDriver
     * TransformBuffer transformBuffer;
     * driver.RegisterFrameSyncCallback([&]() {
     *     transformBuffer.Swap();  // 在正确时机交换
     * }, "TransformBufferSwap");
     * @endcode
     */
    uint32_t RegisterFrameSyncCallback(FrameSyncCallback callback, const std::string &name = "");

    /// 注销帧同步回调
    void UnregisterFrameSyncCallback(uint32_t callbackId);

    /// 获取已注册的回调数量
    size_t GetFrameSyncCallbackCount() const { return m_frameSyncCallbacks.size(); }

    // ========================================================================
    // 即时执行回调注册（用于零延迟需求，如相机、输入）
    // ========================================================================

    /**
     * @brief 注册即时执行回调
     *
     * 这些回调会在每帧 Render 阶段之前、主线程上立即执行。
     * 适用于需要零延迟反馈的逻辑（如相机矩阵更新、UI 交互状态刷新）。
     *
     * @param callback 回调函数
     * @param name 回调名称（用于调试）
     * @return 回调 ID
     */
    uint32_t RegisterImmediateCallback(std::function<void()> callback, const std::string &name = "");

    /// 注销即时执行回调
    void UnregisterImmediateCallback(uint32_t callbackId);

private:
    ECS::Registry &m_registry;
    TaskExecutor m_executor;
    TaskGraph m_taskGraph;
    FrameStats m_stats;
    std::atomic<bool> m_running{false};
    uint32_t m_targetFPS = 0;

    // D3D设备上下文
    DX12Engine::Renderer::D3D12DeviceContext *m_deviceContext = nullptr;

    DX12Engine::Core::GameContext *m_gameContext = nullptr;

    // 双缓冲TaskGraph（避免每帧分配）
    TaskGraph m_backGraph; // 后台构建的图

    // 渲染阶段命令列表收集器（使用 Handle 避免生命周期问题）
    using CmdListHandle = typename DX12Engine::Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle;
    std::array<std::vector<CmdListHandle>, static_cast<size_t>(RenderPhase::Count)> m_renderBuckets;

    // 帧同步回调
    struct CallbackEntry {
        uint32_t id;
        std::string name;
        FrameSyncCallback callback;
    };
    std::vector<CallbackEntry> m_frameSyncCallbacks;
    uint32_t m_nextCallbackId = 1;

    // 稳定帧率
    std::chrono::steady_clock::time_point m_lastFrameTime;

    // 即时执行回调
    struct ImmediateCallbackEntry {
        uint32_t id;
        std::string name;
        std::function<void()> callback;
    };
    std::vector<ImmediateCallbackEntry> m_immediateCallbacks;
    uint32_t m_nextImmediateCallbackId = 1;

    // 阶段执行
    void ExecuteImmediate();
    void ExecutePhase(TaskPhase phase);
    void ExecuteRenderPhase(RenderPhase phase, uint64_t waitSequence); // 批量执行某阶段的命令
    // uint64_t SubmitBarrier(RenderPhase phase);                         // 提交资源屏障
    void FrameSync(); // 调用 L4 回调
    void WaitForTargetFPS();
    void UpdateStats();
};

// ========================================================================
// 全局访问点（可选，方便 L4 层访问）
// ========================================================================

/**
 * @brief 调度器上下文
 *
 * L4 层通过此上下文访问 L3 调度功能。
 * 避免直接使用全局变量，便于测试和替换实现。
 *
 * @note CommandManager 通过 GameContext::DeviceContext->GetCommandManager() 访问
 */
struct SchedulerContext {
    FrameDriver *frameDriver = nullptr;
    TaskExecutor *executor = nullptr;
    TaskGraph *taskGraph = nullptr;
    ECS::Registry *registry = nullptr;
    const FrameStats *stats = nullptr;
    DX12Engine::Renderer::D3D12DeviceContext *deviceContext = nullptr; // D3D12 设备上下文
};

/// 获取当前调度器上下文（线程局部）
SchedulerContext &GetSchedulerContext();

/// 初始化全局调度器上下文
void InitializeSchedulerContext(ECS::Registry &registry,
                                DX12Engine::Renderer::D3D12DeviceContext *deviceContext = nullptr);

/// 关闭调度器上下文
void ShutdownSchedulerContext();

} // namespace DX12Engine::Scheduler
