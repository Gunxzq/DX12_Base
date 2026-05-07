#include "System/Scheduler/FrameDriver.h"
#include "System/Scheduler/TaskGraphBuilder.h"
#include <thread>

namespace DX12::Scheduler {

using namespace DX12Engine::System::Event;

// ========================================================================
// Thread-local Scheduler Context
// ========================================================================

static thread_local SchedulerContext g_schedulerContext;

SchedulerContext &GetSchedulerContext() { return g_schedulerContext; }

void InitializeSchedulerContext(ECS::Registry &registry) {
    static std::unique_ptr<FrameDriver> s_frameDriver;
    s_frameDriver = std::make_unique<FrameDriver>(registry);
    s_frameDriver->Initialize();

    g_schedulerContext.frameDriver = s_frameDriver.get();
    g_schedulerContext.executor = &s_frameDriver->GetExecutor();
    g_schedulerContext.taskGraph = &s_frameDriver->GetTaskGraph();
    g_schedulerContext.registry = &registry;
    g_schedulerContext.stats = &s_frameDriver->GetFrameStats();
}

void ShutdownSchedulerContext() { g_schedulerContext = SchedulerContext{}; }

// ========================================================================
// FrameDriver Implementation
// ========================================================================

FrameDriver::FrameDriver(ECS::Registry &registry)
    : m_registry(registry), m_executor(std::thread::hardware_concurrency() - 1) // 留一个给主线程
{
    m_lastFrameTime = std::chrono::steady_clock::now();

    // ========================================================================
    // 初始化 L1 通信层
    // ========================================================================
    m_messageArena = std::make_unique<MessageArena>();
    m_bucketManager = std::make_unique<BucketManager>();
    m_bucketManager->Initialize(*m_messageArena, 2048); // 每个桶2K容量
}

FrameDriver::~FrameDriver() { Stop(); }

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

    // 2. 从通信层收集消息，构建本帧的任务图
    TaskGraphBuilder::BuildFromBuckets(m_taskGraph, *m_bucketManager, *m_messageArena, m_registry, m_stats);

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

    // 4. 重置消息桶（准备接收下一帧消息）
    m_bucketManager->ResetFrame();
    m_messageArena->ResetFrame();

    // ========================================================================
    // 阶段 1: EarlyUpdate - 输入、网络
    // ========================================================================
    ExecutePhase(TaskPhase::EarlyUpdate);

    // ========================================================================
    // 阶段 2: Update - 主逻辑、Physics
    // ========================================================================
    ExecutePhase(TaskPhase::Update);

    // ========================================================================
    // 阶段 3: LateUpdate - 动画、Transform
    // ========================================================================
    ExecutePhase(TaskPhase::LateUpdate);

    // ========================================================================
    // 阶段 4: PreRender - 视锥剔除、LOD
    // ========================================================================
    ExecutePhase(TaskPhase::PreRender);

    // ========================================================================
    // 关键点：帧同步 - 调用 L4 层回调
    // ========================================================================
    FrameSync();

    // ========================================================================
    // 阶段 5: Render - 渲染提交
    // ========================================================================
    ExecutePhase(TaskPhase::Render);

    // ========================================================================
    // 阶段 6: PostRender - 后处理
    // ========================================================================
    ExecutePhase(TaskPhase::PostRender);

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

} // namespace DX12::Scheduler
