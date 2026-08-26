#include "BackgroundExecutor.h"
#include "Logger/Logger.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/RHI/Command/Fence/FenceManager.h"
#include "Resource/GpuResourceManager.h"
#include <algorithm>

namespace DX12Engine::Async {

BackgroundExecutor::BackgroundExecutor(size_t threadCount) : m_executor(threadCount > 0 ? threadCount : 2) {}

BackgroundExecutor::~BackgroundExecutor() { WaitAll(); }

void BackgroundExecutor::Submit(Scheduler::Task task) {
    if (!task.execute)
        return;

    auto tf = std::make_unique<tf::Taskflow>();
    tf->emplace([task = std::move(task)]() mutable { task.execute(); });

    m_totalSubmitted.fetch_add(1, std::memory_order_relaxed);
    m_pendingCount.fetch_add(1, std::memory_order_release);

    auto future = m_executor.run(*tf);

    PendingTaskflow pending;
    pending.taskflow = std::move(tf);
    pending.future = std::move(future);
    pending.taskCount = 1;

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pending.push_back(std::move(pending));
    }
}

void BackgroundExecutor::SubmitGraph(Scheduler::TaskGraph graph) {
    const auto &tasks = graph.GetTasks();
    if (tasks.empty())
        return;

    auto tf = std::make_unique<tf::Taskflow>();
    std::vector<tf::Task> tfTasks;
    tfTasks.reserve(tasks.size());
    size_t taskCount = 0;

    for (const auto &task : tasks) {
        if (!task.execute)
            continue;

        auto tfTask = tf->emplace([task = task]() mutable { task.execute(); });
        tfTasks.push_back(tfTask);
        taskCount++;
    }

    for (size_t i = 0; i < tasks.size(); ++i) {
        const auto &task = tasks[i];
        const auto &deps = graph.GetDependencies(task.id);
        if (deps.empty())
            continue;

        for (auto depId : deps) {
            for (size_t j = 0; j < tasks.size(); ++j) {
                if (tasks[j].id == depId && j < tfTasks.size() && i < tfTasks.size()) {
                    tfTasks[j].precede(tfTasks[i]);
                    break;
                }
            }
        }
    }

    if (taskCount == 0)
        return;

    m_totalSubmitted.fetch_add(taskCount, std::memory_order_relaxed);
    m_pendingCount.fetch_add(taskCount, std::memory_order_release);

    auto future = m_executor.run(*tf);

    PendingTaskflow pending;
    pending.taskflow = std::move(tf);
    pending.future = std::move(future);
    pending.taskCount = taskCount;

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pending.push_back(std::move(pending));
    }
}

void BackgroundExecutor::SubmitLoadTask(LoadTask task) {
    if (!task.cpuWork)
        return;

    Scheduler::Task cpuTask;
    cpuTask.name = task.name;
    cpuTask.phase = Scheduler::TaskPhase::Update;
    cpuTask.thread = Scheduler::ThreadType::Worker;
    cpuTask.priority = static_cast<uint32_t>(Scheduler::TaskPriority::Background);

    auto gpuWorkFn = std::move(task.gpuWork);
    auto onCompleteFn = std::move(task.onComplete);

    cpuTask.execute = [this, cpuName = task.name, cpuWork = std::move(task.cpuWork), gpuWork = std::move(gpuWorkFn),
                       onComplete = std::move(onCompleteFn)]() mutable {
        // Step 1: CPU 工作
        cpuWork();

        // Step 2: GPU 工作（录制命令）
        if (gpuWork) {
            auto item = gpuWork();
            if (item) {
                item->onComplete = [item_onComplete = std::move(onComplete)](bool success) {
                    if (item_onComplete)
                        item_onComplete(success);
                };
                RegisterGpuWork(std::move(item));
                return;
            }
        }

        // 没有 GPU 工作 → 延后到主线程 Tick 执行
        if (onComplete) {
            this->DeferToMainThread([onComplete = std::move(onComplete)] { onComplete(true); });
        }
    };

    Submit(cpuTask);
}

void BackgroundExecutor::RegisterGpuWork(GpuWorkItemPtr item) {
    if (!item)
        return;
    {
        std::lock_guard<std::mutex> lock(m_gpuWorkMutex);
        m_gpuWorkQueue.push_back(std::move(item));
    }
}

size_t BackgroundExecutor::GetGpuWorkQueueSize() const {
    std::lock_guard<std::mutex> lock(m_gpuWorkMutex);
    return m_gpuWorkQueue.size();
}

void BackgroundExecutor::ProcessGpuWork(const GpuWorkItemPtr &item) {
    if (!m_cmdMgr) {
        auto *logger = Logger::Logger::GetInstance();
        logger->Error("[BackgroundExecutor] CommandManager not set, cannot process GPU work");
        if (item->onComplete)
            item->onComplete(false);
        return;
    }

    auto &cmdMgr = *m_cmdMgr;
    auto &fenceMgr = cmdMgr.GetFenceManager();
    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
    auto *logger = Logger::Logger::GetInstance();

    // ================================================================
    // Step 1: Submit COPY 命令列表
    // ================================================================
    if (item->copyCmdListHandle.IsValid()) {
        auto copyCmdList = cmdMgr.GetCommandList<D3D12_COMMAND_LIST_TYPE_COPY>(item->copyCmdListHandle);
        if (copyCmdList.IsValid()) {
            cmdMgr.Submit(D3D12_COMMAND_LIST_TYPE_COPY, copyCmdList);
            logger->Debug("[BackgroundExecutor] COPY command list submitted");
        }
    }

    // Signal COPY fence
    uint64_t copyFenceValue = 0;
    {
        auto *copyQueue = cmdMgr.GetCommandQueue(D3D12_COMMAND_LIST_TYPE_COPY);
        if (copyQueue) {
            copyFenceValue = fenceMgr.GetNextSequence();
            fenceMgr.Signal(D3D12_COMMAND_LIST_TYPE_COPY, copyQueue->Get(), copyFenceValue);
            item->copyFenceValue = copyFenceValue; // 持久化，供外部查询
        }
    }

    // 释放 COPY 命令列表
    if (item->copyCmdListHandle.IsValid()) {
        cmdMgr.ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_COPY>(item->copyCmdListHandle);
    }
    if (item->copyAllocatorHandle.IsValid()) {
        cmdMgr.ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(item->copyAllocatorHandle, copyFenceValue);
    }

    // ================================================================
    // Step 2: Submit DIRECT 命令列表（Wait COPY fence）
    // ================================================================
    if (item->directCmdListHandle.IsValid()) {
        auto directCmdList = cmdMgr.GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(item->directCmdListHandle);
        if (directCmdList.IsValid()) {
            // GPU 端等待 COPY 完成
            if (copyFenceValue > 0) {
                auto *directQueue = cmdMgr.GetCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT);
                auto *copyFence = fenceMgr.GetFence(D3D12_COMMAND_LIST_TYPE_COPY);
                if (directQueue && copyFence) {
                    directQueue->Wait(copyFence->Get(), copyFenceValue);
                }
            }

            cmdMgr.Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, directCmdList);
            logger->Debug("[BackgroundExecutor] DIRECT command list submitted");
        }
    }

    // Signal DIRECT fence
    uint64_t directFenceValue = 0;
    {
        auto *directQueue = cmdMgr.GetCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT);
        if (directQueue) {
            directFenceValue = fenceMgr.GetNextSequence();
            fenceMgr.Signal(D3D12_COMMAND_LIST_TYPE_DIRECT, directQueue->Get(), directFenceValue);
            item->directFenceValue = directFenceValue; // 持久化，供外部查询
        }
    }

    // 释放 DIRECT 命令列表
    if (item->directCmdListHandle.IsValid()) {
        cmdMgr.ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(item->directCmdListHandle);
    }
    if (item->directAllocatorHandle.IsValid()) {
        cmdMgr.ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(item->directAllocatorHandle, directFenceValue);
    }

    // ================================================================
    // Step 3: 登记到待完成队列（不阻塞，由 Tick Phase 4 非阻塞检查）
    // 上传缓冲区用 fence 标记延迟释放，无需 CPU 等
    // ================================================================
    if (directFenceValue > 0) {
        {
            std::lock_guard<std::mutex> lock(m_pendingCompletionMutex);
            m_pendingCompletion.push_back({item, directFenceValue});
        }
        logger->Debug("[BackgroundExecutor] GPU submitted, pending fence={} (copyFence={})", directFenceValue,
                      copyFenceValue);
    } else {
        // 没有 fence 值——理论上不应发生，直接回调
        if (item->onComplete)
            item->onComplete(false);
    }
}

// ========================================================================
// Tick — 类似 FrameDriver::Tick 的主线程入口
// ========================================================================
// 流程：
//   1. 清理已完成的 CPU taskflow
//   2. 收集就绪的 GpuWorkItem
//   3. 逐个 Submit COPY → Signal → Submit DIRECT(Wait) → Signal（不阻塞）
//   4. 非阻塞检查 pending GPU fence 完成 → 释放上传缓冲 → 回调
//   5. 执行延后的主线程回调（无 gpuWork 任务）
// ========================================================================
void BackgroundExecutor::Tick() {
    // ── Phase 1: 清理已完成的 CPU 任务 ──
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);

        auto it = m_pending.begin();
        while (it != m_pending.end()) {
            if (it->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                m_pendingCount.fetch_sub(it->taskCount, std::memory_order_release);
                m_totalCompleted.fetch_add(it->taskCount, std::memory_order_relaxed);
                it = m_pending.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ── Phase 2: 收集就绪的 GPU 工作项 ──
    std::vector<GpuWorkItemPtr> readyItems;
    {
        std::lock_guard<std::mutex> lock(m_gpuWorkMutex);
        readyItems.swap(m_gpuWorkQueue);
    }
    if (!readyItems.empty()) {
        auto *logger = Logger::Logger::GetInstance();
        logger->Debug("[BackgroundExecutor] Phase2: collected {} GPU work items", readyItems.size());
    }

    // ── Phase 3: 统一提交 GPU 工作（只 Submit + Signal，不阻塞） ──
    for (const auto &item : readyItems) {
        if (item && item->ready.load(std::memory_order_acquire)) {
            ProcessGpuWork(item);
        }
    }

    // ── Phase 4: 非阻塞检查 pending GPU 完成 ──
    CheckPendingCompletions();

    // ── Phase 5: 执行延后的主线程回调（无 gpuWork 任务） ──
    ExecuteDeferredCallbacks();
}

void BackgroundExecutor::WaitAll() {
    m_executor.wait_for_all();

    std::lock_guard<std::mutex> lock(m_pendingMutex);
    for (auto &pending : m_pending) {
        m_pendingCount.fetch_sub(pending.taskCount, std::memory_order_release);
        m_totalCompleted.fetch_add(pending.taskCount, std::memory_order_relaxed);
    }
    m_pending.clear();
}

// ========================================================================
// CheckPendingCompletions — 非阻塞检查 GPU fence 是否完成
// ========================================================================
void BackgroundExecutor::CheckPendingCompletions() {
    if (!m_cmdMgr || m_pendingCompletion.empty())
        return;

    auto &fenceMgr = m_cmdMgr->GetFenceManager();
    auto *logger = Logger::Logger::GetInstance();

    std::lock_guard<std::mutex> lock(m_pendingCompletionMutex);

    auto it = m_pendingCompletion.begin();
    while (it != m_pendingCompletion.end()) {
        if (fenceMgr.IsSequenceCompleted(D3D12_COMMAND_LIST_TYPE_DIRECT, it->directFenceValue)) {
            // 释放上传缓冲区（传 fence 值，池化延迟回收）
            for (auto &upH : it->item->uploadBufferHandles) {
                if (upH.IsValid()) {
                    Resource::GpuResourceManager::GetInstance().Release(upH, it->directFenceValue);
                }
            }
            it->item->uploadBufferHandles.clear();

            // 主线程回调
            if (it->item->onComplete) {
                it->item->onComplete(true);
            }

            logger->Debug("[BackgroundExecutor] GPU work completed (fence={})", it->directFenceValue);
            it = m_pendingCompletion.erase(it);
        } else {
            ++it;
        }
    }
}

// ========================================================================
// DeferToMainThread — 将回调延后到主线程 Tick 执行
// ========================================================================
void BackgroundExecutor::DeferToMainThread(std::function<void()> callback) {
    if (!callback)
        return;
    std::lock_guard<std::mutex> lock(m_deferredMutex);
    m_deferredCallbacks.push_back(std::move(callback));
}

// ========================================================================
// ExecuteDeferredCallbacks — 执行延后的主线程回调
// ========================================================================
void BackgroundExecutor::ExecuteDeferredCallbacks() {
    if (m_deferredCallbacks.empty())
        return;

    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(m_deferredMutex);
        callbacks.swap(m_deferredCallbacks);
    }

    for (auto &cb : callbacks) {
        cb();
    }
}

} // namespace DX12Engine::Async
