#include "System/Scheduler/FrameDriver.h"
#include "Renderer/Core/Command/CommandList/CommandList.h"
#include "Renderer/Core/Command/CommandManager.h"
#include "Renderer/Core/D3D12DeviceContext.h"
#include "System/Event/MessageDispatcher.h"
#include "System/Scheduler/TaskGraphBuilder.h"
#include <Common/Common.h>
#include <Common/d3dx12.h>
#include <thread>

namespace DX12Engine::Scheduler {

using namespace DX12Engine::System::Event;
using namespace DX12Engine::Renderer;

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
    m_frameStartTime = std::chrono::steady_clock::now();
}

void FrameDriver::SubmitRenderCommand(RenderPhase phase, const CmdListHandle &handle) {
    if (handle.IsValid()) {
        m_renderBuckets[static_cast<size_t>(phase)].push_back(handle);
    }
}

void FrameDriver::ExecuteRenderPhase(RenderPhase phase, uint64_t waitSequence) {
    auto &handles = m_renderBuckets[static_cast<size_t>(phase)];
    if (!handles.empty()) {
        // 在批量执行前，让队列等待 BeginBarrier 完成
        if (waitSequence > 0) {
            auto *queue = m_deviceContext->GetCommandQueue();
            auto *fence = GetCommandManager()->GetFenceManager().GetFence(D3D12_COMMAND_LIST_TYPE_DIRECT)->Get();
            queue->Wait(fence, waitSequence);
        }
        m_deviceContext->GetCommandManager().ExecuteBatchAndClose(D3D12_COMMAND_LIST_TYPE_DIRECT, handles);
        handles.clear();
    }
}

uint64_t FrameDriver::SubmitBarrier(RenderPhase phase) {
    if (!m_deviceContext)
        return 0;

    auto &cmdMgr = m_deviceContext->GetCommandManager();
    uint64_t completed = cmdMgr.GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);

    auto allocHandle = cmdMgr.AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(completed);
    ID3D12CommandAllocator *allocator = cmdMgr.GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle);

    auto cmdListHandle = cmdMgr.AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
    CommandList cmdList = cmdMgr.GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

    cmdList.Reset(allocator, nullptr);

    ID3D12Resource *backBuffer = m_deviceContext->GetCurrentBackBuffer();

    D3D12_RESOURCE_STATES stateBefore =
        (phase == RenderPhase::BeginBarrier) ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES stateAfter =
        (phase == RenderPhase::BeginBarrier) ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_PRESENT;

    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, stateBefore, stateAfter);
    cmdList.Get()->ResourceBarrier(1, &barrier);

    if (phase == RenderPhase::BeginBarrier) {
        D3D12_VIEWPORT viewport = m_deviceContext->GetViewport();
        D3D12_RECT scissorRect = m_deviceContext->GetScissorRect();
        cmdList.Get()->RSSetViewports(1, &viewport);
        cmdList.Get()->RSSetScissorRects(1, &scissorRect);

        auto rtvHandle = m_deviceContext->GetCurrentBackBufferView();
        auto dsvHandle = m_deviceContext->GetDepthStencilView();
        cmdList.Get()->OMSetRenderTargets(1, &rtvHandle, true, &dsvHandle);

        // 根据时间计算颜色
        float time = static_cast<float>(m_stats.totalTime);
        float r = (sin(time * 0.5f) + 1.0f) / 2.0f;
        float g = (sin(time * 0.7f + 2.0f) + 1.0f) / 2.0f;
        float b = (sin(time * 0.9f + 4.0f) + 1.0f) / 2.0f;
        const float clearColor[] = {r, g, b, 1.0f};

        cmdList.Get()->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        cmdList.Get()->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0,
                                             nullptr);
    }

    uint64_t sequence = cmdMgr.GetNextSequence();

    cmdList.Close();

    cmdMgr.SubmitAndSignal(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList, sequence);

    // 屏障列表提交后立即释放资源
    cmdMgr.ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);
    cmdMgr.ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocHandle, sequence);

    // 关键：在 GPU 端插入等待，确保后续命令在屏障完成后才执行
    if (phase == RenderPhase::BeginBarrier) {
        auto *queue = m_deviceContext->GetCommandQueue();
        auto *fence = cmdMgr.GetFenceManager().GetFence(D3D12_COMMAND_LIST_TYPE_DIRECT)->Get();

        queue->Wait(fence, sequence);
    }

    return sequence;
}

bool FrameDriver::Tick() {
    if (!m_running)
        return false;

    m_frameStartTime = std::chrono::steady_clock::now();

    // 计算 DeltaTime
    auto duration = std::chrono::duration<float>(m_frameStartTime - m_lastFrameTime);
    m_stats.deltaTime = duration.count();
    m_stats.totalTime += m_stats.deltaTime;
    m_stats.frameNumber++;

    // ========================================================================
    // 阶段 0: Message -> DAG 编译器 (关键新增：消息驱动动态构建)
    // ========================================================================
    // 1. 清空上一帧的图（或使用双缓冲交换）
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
            // TODO: 使用Logger记录错误
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

    // ========================================================================
    // 逻辑更新阶段
    // ========================================================================
    ExecutePhase(TaskPhase::EarlyUpdate);
    ExecutePhase(TaskPhase::Update);
    ExecutePhase(TaskPhase::LateUpdate);
    ExecutePhase(TaskPhase::PreRender);

    // 4. 帧同步回调
    FrameSync();

    // ========================================================================
    // 渲染流程开始
    // ========================================================================

    // 1. 提交帧开始屏障 (Present -> RenderTarget)
    uint64_t barrierSeq = SubmitBarrier(RenderPhase::BeginBarrier);

    // 2. 执行用户注册的渲染 System
    ExecutePhase(TaskPhase::Render);
    ExecutePhase(TaskPhase::PostRender);

    // 3. 按顺序批量执行各个渲染阶段的命令列表
    ExecuteRenderPhase(RenderPhase::PrePass, barrierSeq);
    ExecuteRenderPhase(RenderPhase::Opaque, barrierSeq);
    ExecuteRenderPhase(RenderPhase::Transparent, barrierSeq);
    ExecuteRenderPhase(RenderPhase::PostProcess, barrierSeq);
    ExecuteRenderPhase(RenderPhase::UI, barrierSeq);

    // 5. 提交帧结束屏障 (RenderTarget -> Present) 并 Present
    SubmitBarrier(RenderPhase::EndBarrier);

    // ========================================================================
    // 帧结束：调用 DeviceContext 进行 Present 和帧推进
    // ========================================================================
    if (m_deviceContext) {

        m_deviceContext->EndFrame();
    }

    // 更新统计信息
    UpdateStats();

    // 等待目标帧率
    WaitForTargetFPS();

    m_lastFrameTime = m_frameStartTime;
    return m_running;
}

void FrameDriver::Run() {
    m_running = true;
    while (Tick()) {
        // 主循环
    }
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

    // 等待本阶段完成（屏障同步）
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
    auto elapsed = std::chrono::duration<float>(now - m_frameStartTime).count();

    if (elapsed < targetFrameTime) {
        float sleepTime = targetFrameTime - elapsed;
        std::this_thread::sleep_for(std::chrono::duration<float>(sleepTime));
    }
}

void FrameDriver::UpdateStats() {
    m_stats.taskCount = static_cast<uint32_t>(m_taskGraph.GetTaskCount());
    // m_stats.activeEntities = m_registry.Alive();  // 需要 Registry 提供接口
}

} // namespace DX12Engine::Scheduler
