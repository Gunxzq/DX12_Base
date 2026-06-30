#include "Scheduler/FrameDriver.h"
#include "Boot/GameContext.h"
#include "Event/MessageDispatcher.h"
#include "Platform/Input/InputManager.h"
#include "Platform/Windows/Window.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/TerrainManager/TerrainManager.h"
#include "Scheduler/TaskGraphBuilder.h"
#include <Common/Common.h>
#include <Common/d3dx12.h>
#include <thread>

using namespace DX12Engine::Event;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Scheduler;
using namespace DX12Engine::Boot;
using namespace DX12Engine::Input;

namespace DX12Engine::Scheduler {

// ========================================================================
// Thread-local Scheduler Context
// ========================================================================

static thread_local SchedulerContext g_schedulerContext;

SchedulerContext &GetSchedulerContext() { return g_schedulerContext; }

void InitializeSchedulerContext(ECS::Registry &registry, DX12Engine::Renderer::D3D12DeviceContext *deviceContext) {
    static std::unique_ptr<FrameDriver> s_frameDriver;
    s_frameDriver = std::make_unique<FrameDriver>(registry);
    s_frameDriver->Initialize();

    // 注入命令管理器
    s_frameDriver->SetDeviceContext(deviceContext);

    g_schedulerContext.frameDriver = s_frameDriver.get();
    g_schedulerContext.executor = &s_frameDriver->GetExecutor();
    g_schedulerContext.taskGraph = &s_frameDriver->GetTaskGraph();
    g_schedulerContext.registry = &registry;
    g_schedulerContext.stats = &s_frameDriver->GetFrameStats();
    g_schedulerContext.deviceContext = deviceContext;
}

void ShutdownSchedulerContext() { g_schedulerContext = SchedulerContext{}; }

// ========================================================================
// FrameDriver Implementation
// ========================================================================

FrameDriver::FrameDriver(ECS::Registry &registry)
    : m_registry(registry), m_executor(std::thread::hardware_concurrency() - 1) // 留一个给主线程
{
    m_lastFrameTime = std::chrono::steady_clock::now();
}

FrameDriver::~FrameDriver() { Stop(); }

DX12Engine::Renderer::CommandManager *FrameDriver::GetCommandManager() const {
    return m_deviceContext ? &m_deviceContext->GetCommandManager() : nullptr;
}

void FrameDriver::Initialize(uint32_t workerThreadCount) {
    if (workerThreadCount > 0) {
        // 注意：TaskExecutor 线程数在构造时确定
        // 如需动态调整，需要重新设计
    }

    // 验证任务图无死锁
    try {
        m_taskGraph.Validate();
    } catch (const std::runtime_error &e) {
        // TODO: 使用 Logger 记录错误
        throw;
    }

    m_running = true;
}

void FrameDriver::SubmitRenderCommand(RenderPhase phase, const CmdListHandle &handle) {
    if (handle.IsValid()) {
        m_renderBuckets[static_cast<size_t>(phase)].push_back(handle);
    }
}

void FrameDriver::ExecuteRenderPhase(RenderPhase phase, uint64_t waitSequence) {
    auto &handles = m_renderBuckets[static_cast<size_t>(phase)];
    if (!handles.empty()) {

        m_deviceContext->GetCommandManager().SubmitBatch(handles, waitSequence);

        for (const auto &handle : handles) {
            if (handle.IsValid()) {
                m_deviceContext->GetCommandManager().ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(handle);
            }
        }
        handles.clear();
    }
}

bool FrameDriver::Tick() {
    if (!m_running)
        return false;

    // 帧统计
    m_stats.frameNumber++;

    if (!m_gameContext || !m_gameContext->InputMgr || !m_gameContext->Window)
        return false;

    // ========================================================================
    // 0. 帧开始：清空上一帧的增量数据
    // =======================================================================
    m_gameContext->InputMgr->BeginFrame(); // ← 先清空，为接收本帧的新消息做准备

    // ========================================================================
    // 1. 消息处理（需要移到 Tick 内部，或者确保在 BeginFrame 之后）
    // ========================================================================
    m_gameContext->Window->ProcessMessages(); // ← 现在填充的是本帧的新数据

    // ========================================================================
    // 2. 更新输入系统
    // ========================================================================
    float deltaTime = m_gameContext->MainTimer->GetDeltaTime();
    float currentTime = static_cast<float>(m_gameContext->MainTimer->GetGameTime());
    m_gameContext->InputMgr->Update(deltaTime, currentTime);

    // ========================================================================
    // 阶段 0: Message -> DAG 编译器（消息驱动动态构建）
    // 前台每帧从消息构建任务图，不合并后台图
    // ========================================================================
    // 1. 清空前台图，准备构建本帧任务
    m_taskGraph.Clear();

    // 2. 从通信层收集消息，构建本帧的任务图（通过 Dispatcher 单例）
    auto *dispatcher = MessageDispatcher::GetInstance();
    if (dispatcher) {
        TaskGraphBuilder::BuildFromBuckets(m_taskGraph, *dispatcher, m_registry, m_stats);
    }

    // 3. 如果图非空，验证合法性
    if (m_taskGraph.GetTaskCount() > 0) {
        try {
            m_taskGraph.Validate();
        } catch (const std::runtime_error &e) {
            // 验证失败，清空图避免灾难
            m_taskGraph.Clear();
            if (m_gameContext) {
                m_gameContext->Logging->Error("[FrameDriver] Validate failed: {}", e.what());
            }
        }
    }

    // 4. 帧结束清理（通过 Dispatcher 单例统一清理 Arena 和 BucketManager）
    if (dispatcher) {
        dispatcher->EndFrame();
    }

    // ========================================================================
    // 帧开始：调用 DeviceContext 处理三帧同步
    // ========================================================================
    if (m_deviceContext) {

        m_deviceContext->BeginFrame();
    }

    if (m_gameContext && m_gameContext->FrameResourceManager) {
        uint64_t completedFence = m_gameContext->GetCompletedFence();
        uint64_t nextFence = m_gameContext->GetNextFence();
        m_gameContext->FrameResourceManager->BeginFrame(completedFence, nextFence);

        // TerrainManager 的 Reclaim 已移到 UpdateAndUpload 内部（匹配 LightManager 模式）
        // 这里只清空 pending 缓存
        TerrainManager::GetInstance().BeginFrame(completedFence);
    }

    // ========================================================================
    // 即时执行路径 (Immediate Path)
    // 目的：为零延迟的相机和 UI 提供最新数据
    // ========================================================================
    ExecuteImmediate();

    // ========================================================================
    // 渲染阶段 (Render Phase)
    // 读取的是【上一帧】在 FrameSync 中准备好的数据 (Slot N-1)
    // ========================================================================

    // A. 执行渲染逻辑 System (录制命令列表)
    // 这些 System 会读取 ECS 中的 Transform 等组件的"只读视图"或"前端缓冲区"
    ExecutePhase(TaskPhase::Render);
    ExecutePhase(TaskPhase::PostRender);

    // B. 提交命令列表到 GPU
    // 注意：此时 GPU 开始执行第 N-1 帧的渲染任务
    ExecuteRenderPhase(RenderPhase::PrePass, 0);

    ExecuteRenderPhase(RenderPhase::DynamicAOcclusion, 0); // 动态屏幕空间环境光遮蔽（PrePass 深度就绪后执行）

    ExecuteRenderPhase(RenderPhase::Opaque, 0);    // 不透明物体 + 地形（资源状态一致，合并执行）
    ExecuteRenderPhase(RenderPhase::Billboard, 0); // 复用 Opaque 深度，不写深度
    ExecuteRenderPhase(RenderPhase::Transparent, 0);
    ExecuteRenderPhase(RenderPhase::PostProcess, 0);
    ExecuteRenderPhase(RenderPhase::FSR3_Upscale, 0);
    ExecuteRenderPhase(RenderPhase::UI, 0);

    // ========================================================================
    // 逻辑更新阶段 (Update Phase)
    // 写入的是【当前帧】的数据 (Slot N)，为下一帧做准备
    // ========================================================================

    ExecutePhase(TaskPhase::EarlyUpdate);
    ExecutePhase(TaskPhase::Update);
    ExecutePhase(TaskPhase::LateUpdate);

    // PreCulling：剔除 + LOD 计算（CullingSystem + LODSystem）
    ExecutePhase(TaskPhase::PreCulling);

    // PostCulling：射线检测、遮挡查询（使用 PreCulling 可见集作为候选）
    ExecutePhase(TaskPhase::PostCulling);

    // SceneDataUpload：所有 Manager 上传 GPU 数据（探针/光源/地形）
    ExecuteSceneDataUpload();

    // PreRender：构建器并行生成渲染队列（Opaque/Transparent/Terrain/Billboard）
    ExecutePhase(TaskPhase::PreRender);

    // ========================================================================
    // 4. 帧同步 (FrameSync)
    // 将 Slot N 的数据"冻结"并标记为"可读"，准备供下一帧 Render 使用
    // ========================================================================
    FrameSync();

    // ========================================================================
    // 帧结束：调用 DeviceContext 进行 Present 和帧推进
    // ========================================================================
    if (m_deviceContext) {
        m_deviceContext->EndFrame();
    }

    // 更新统计信息
    UpdateStats();

    // ========================================================================
    // 等待目标帧率（注意：sleep 时间会被计入 cpuIdleTime）
    // ========================================================================
    WaitForTargetFPS();

    m_lastFrameTime = std::chrono::steady_clock::now(); // 更新

    return m_running;
}

uint32_t FrameDriver::RegisterFrameSyncCallback(FrameSyncCallback callback, const std::string &name) {
    uint32_t id = m_nextCallbackId++;
    m_frameSyncCallbacks.push_back({id, name, std::move(callback)});
    return id;
}

void FrameDriver::UnregisterFrameSyncCallback(uint32_t callbackId) {
    auto it = std::remove_if(m_frameSyncCallbacks.begin(), m_frameSyncCallbacks.end(),
                             [callbackId](const CallbackEntry &entry) { return entry.id == callbackId; });
    m_frameSyncCallbacks.erase(it, m_frameSyncCallbacks.end());
}

void FrameDriver::ExecutePhase(TaskPhase phase) {
    // 执行本阶段任务
    m_executor.ExecutePhase(m_taskGraph, phase);

    // 处理主线程任务
    auto mainTasks = m_executor.StealMainThreadTasks();
    for (auto &task : mainTasks) {
        task();
    }

    auto renderTasks = m_executor.StealRenderThreadTasks();
    for (auto &task : renderTasks)
        task();

    m_executor.WaitForCompletion();
}

void FrameDriver::FrameSync() {
    // 核心：在逻辑线程写完成后，调用 L4 层的多缓冲交换回调
    // L4 层在此执行自己的 Swap() 逻辑

    auto syncStart = std::chrono::steady_clock::now();

    // 调用所有 L4 层注册的回调
    for (const auto &entry : m_frameSyncCallbacks) {
        if (entry.callback) {
            entry.callback();
        }
    }

    // 清理墓碑（可选，可以每 N 帧执行一次）
    if (m_stats.frameNumber % 60 == 0) {
        m_registry.SweepTombstones();
    }

    auto syncEnd = std::chrono::steady_clock::now();
    auto syncDuration = std::chrono::duration<float>(syncEnd - syncStart);

    // 统计同步耗时（通常 < 0.1ms）
    // m_stats.cpuSyncTime = syncDuration.count();
}

void FrameDriver::WaitForTargetFPS() {
    if (m_targetFPS == 0)
        return;

    float targetFrameTime = 1.0f / m_targetFPS;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<float>(now - m_lastFrameTime).count();

    if (elapsed < targetFrameTime) {
        float sleepTime = targetFrameTime - elapsed;
        std::this_thread::sleep_for(std::chrono::duration<float>(sleepTime));
    }
}

void FrameDriver::UpdateStats() {
    m_stats.taskCount = static_cast<uint32_t>(m_taskGraph.GetTaskCount());
    // m_stats.activeEntities = m_registry.Alive();  // 需要 Registry 提供接口
}

uint32_t FrameDriver::RegisterImmediateCallback(std::function<void()> callback, const std::string &name) {
    uint32_t id = m_nextImmediateCallbackId++;
    m_immediateCallbacks.push_back({id, name, std::move(callback)});
    return id;
}

void FrameDriver::UnregisterImmediateCallback(uint32_t callbackId) {
    auto it = std::remove_if(m_immediateCallbacks.begin(), m_immediateCallbacks.end(),
                             [callbackId](const ImmediateCallbackEntry &entry) { return entry.id == callbackId; });
    m_immediateCallbacks.erase(it, m_immediateCallbacks.end());
}

void FrameDriver::ExecuteImmediate() {
    // 在主线程上串行执行所有注册的即时回调
    for (const auto &entry : m_immediateCallbacks) {
        if (entry.callback) {
            entry.callback();
        }
    }
}

// ============================================================================
// SceneDataUpload 回调
// ============================================================================

uint32_t FrameDriver::RegisterSceneDataCallback(std::function<void()> callback, const std::string &name) {
    uint32_t id = m_nextSceneDataCallbackId++;
    m_sceneDataCallbacks.push_back({id, name, std::move(callback)});
    return id;
}

void FrameDriver::UnregisterSceneDataCallback(uint32_t callbackId) {
    auto it = std::remove_if(m_sceneDataCallbacks.begin(), m_sceneDataCallbacks.end(),
                             [callbackId](const ImmediateCallbackEntry &entry) { return entry.id == callbackId; });
    m_sceneDataCallbacks.erase(it, m_sceneDataCallbacks.end());
}

void FrameDriver::ExecuteSceneDataUpload() {
    // 在主线程上串行执行所有注册的场景数据上传回调
    // 位置：PostCulling 之后、PreRender 之前
    for (const auto &entry : m_sceneDataCallbacks) {
        if (entry.callback) {
            entry.callback();
        }
    }
}

} // namespace DX12Engine::Scheduler
