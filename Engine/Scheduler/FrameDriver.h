#pragma once
#include "RenderPhase.h"
#include "Renderer/RHI/Command/CommandList/CommandListPool.h"

#include "TaskExecutor.h"
#include "TaskGraphBuilder.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace DX12Engine {
namespace Boot {
class GameContext; // 前置声明
}
namespace Renderer {
class D3D12DeviceContext;
class CommandManager;
class CommandList;
} // namespace Renderer

} // namespace DX12Engine

namespace DX12Engine {
namespace Scheduler {

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
 * 1. 消息处理：ProcessMessages + Input Update
 * 2. DAG 构建：BuildFromBuckets 从消息构建本帧任务图
 * 3. Immediate 回调：零延迟路径（相机、UI）
 * 4. Render Phase：渲染提交（录制命令列表）
 * 5. Update Phases：EarlyUpdate → Update → LateUpdate → PreCulling → PostCulling → SceneDataUpload → PreRender
 * 6. Frame Sync：L4 多缓冲交换回调
 * ```
 *
 * 关键设计原则：
 * - 前台不 Merge 后台图，前后台仅通过 MessageDispatcher 通信
 * - 前台每帧从消息构建任务图，不依赖后台状态
 * - 后台图 m_backGraph 由 BackgroundExecutor 独立管理
 */
class FrameDriver {
public:
    FrameDriver();
    ~FrameDriver();

    // 禁止拷贝移动
    FrameDriver(const FrameDriver &) = delete;
    FrameDriver &operator=(const FrameDriver &) = delete;
    FrameDriver(FrameDriver &&) = delete;
    FrameDriver &operator=(FrameDriver &&) = delete;

    void Initialize(uint32_t workerThreadCount = 0); // 0 = 自动

    bool Tick();
    void Stop() { m_running = false; }
    bool IsRunning() const { return m_running; }

    const FrameStats &GetFrameStats() const { return m_stats; }
    void SetTargetFPS(uint32_t fps) { m_targetFPS = fps; }
    uint32_t GetTargetFPS() const { return m_targetFPS; }

    void SetDeviceContext(Renderer::D3D12DeviceContext *deviceContext) { m_deviceContext = deviceContext; }
    void SetGameContext(Boot::GameContext *context) { m_gameContext = context; }

    Boot::GameContext *GetGameContext() const { return m_gameContext; }
    Renderer::D3D12DeviceContext *GetDeviceContext() const { return m_deviceContext; }
    Renderer::CommandManager *GetCommandManager() const;

    /// 获取任务执行器（供 L4 层提交任务）
    TaskExecutor &GetExecutor() { return m_executor; }

    /// 获取任务图（供 L4 层注册 System）
    TaskGraph &GetTaskGraph() { return m_taskGraph; }

    // ========================================================================
    // 渲染阶段管理（新增）
    // ========================================================================
    void SubmitRenderCommand(RenderPhase phase,
                             const typename Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle &handle);

    // ========================================================================
    // L4 层回调注册
    // ========================================================================

    // 帧同步点
    uint32_t RegisterFrameSyncCallback(FrameSyncCallback callback, const std::string &name = "");
    void UnregisterFrameSyncCallback(uint32_t callbackId);
    size_t GetFrameSyncCallbackCount() const { return m_frameSyncCallbacks.size(); }

    // 立即回调（零延迟路径：相机、UI）
    uint32_t RegisterImmediateCallback(std::function<void()> callback, const std::string &name = "");
    void UnregisterImmediateCallback(uint32_t callbackId);

    // 场景数据上传回调（PostCulling 之后、PreRender 之前：探针/光源/地形上传）
    uint32_t RegisterSceneDataCallback(std::function<void()> callback, const std::string &name = "");
    void UnregisterSceneDataCallback(uint32_t callbackId);

private:
    TaskExecutor m_executor;
    TaskGraph m_taskGraph;
    FrameStats m_stats;
    std::atomic<bool> m_running{false};
    uint32_t m_targetFPS = 0;

    // D3D设备上下文
    Renderer::D3D12DeviceContext *m_deviceContext = nullptr;
    Boot::GameContext *m_gameContext = nullptr;

    // 渲染阶段命令列表收集器（使用 Handle 避免生命周期问题）
    using CmdListHandle = typename Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle;
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

    // 场景数据上传回调
    std::vector<ImmediateCallbackEntry> m_sceneDataCallbacks;
    uint32_t m_nextSceneDataCallbackId = 1;

    // 阶段执行
    void ExecuteImmediate();
    void ExecuteSceneDataUpload();
    void ExecutePhase(TaskPhase phase);
    void ExecuteRenderPhase(RenderPhase phase, uint64_t waitSequence); // 批量执行某阶段的命令
    void FrameSync();                                                  // 调用 L4 回调
    void WaitForTargetFPS();
    void UpdateStats();
};

struct SchedulerContext {
    FrameDriver *frameDriver = nullptr;
    TaskExecutor *executor = nullptr;
    TaskGraph *taskGraph = nullptr;
    const FrameStats *stats = nullptr;
    Renderer::D3D12DeviceContext *deviceContext = nullptr; // D3D12 设备上下文
};

/// 获取当前调度器上下文（线程局部）
SchedulerContext &GetSchedulerContext();
void InitializeSchedulerContext(Renderer::D3D12DeviceContext *deviceContext = nullptr);
void ShutdownSchedulerContext();

} // namespace Scheduler
} // namespace DX12Engine
