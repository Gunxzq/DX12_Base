#include "Renderer/Core/Command/CommandManager.h"

namespace DX12Engine::Renderer {

void CommandManager::Initialize(ID3D12Device *device, uint32_t frameCount) {
    assert(device != nullptr);
    assert(frameCount >= 2);

    m_device = device;
    m_frameCount = frameCount;
    m_currentFrame = 0;

    m_frameResources.resize(frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) {
        m_frameResources[i] = {0, false};
    }

    // 1. 创建围栏
    m_fenceManager.CreateFence(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    m_fenceManager.CreateFence(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
    m_fenceManager.CreateFence(device, D3D12_COMMAND_LIST_TYPE_COPY);

    // 2. 创建命令队列 (使用 Map)
    m_queues[D3D12_COMMAND_LIST_TYPE_DIRECT] = std::make_unique<CommandQueue>(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    m_queues[D3D12_COMMAND_LIST_TYPE_COMPUTE] = std::make_unique<CommandQueue>(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
    m_queues[D3D12_COMMAND_LIST_TYPE_COPY] = std::make_unique<CommandQueue>(device, D3D12_COMMAND_LIST_TYPE_COPY);

    // 3. 初始化分配器池 (使用 Map 和基类指针)
    m_allocatorPools[D3D12_COMMAND_LIST_TYPE_DIRECT] =
        std::make_unique<CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_DIRECT>>(device, INITIAL_ALLOCATOR_POOL_SIZE);

    m_allocatorPools[D3D12_COMMAND_LIST_TYPE_COMPUTE] =
        std::make_unique<CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COMPUTE>>(device, INITIAL_ALLOCATOR_POOL_SIZE);

    m_allocatorPools[D3D12_COMMAND_LIST_TYPE_COPY] =
        std::make_unique<CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COPY>>(device, INITIAL_ALLOCATOR_POOL_SIZE);

    // 4. 初始化命令列表池 (使用 Map 和基类指针)
    m_commandListPools[D3D12_COMMAND_LIST_TYPE_DIRECT] =
        std::make_unique<CommandListPool<D3D12_COMMAND_LIST_TYPE_DIRECT>>(device);

    m_commandListPools[D3D12_COMMAND_LIST_TYPE_COMPUTE] =
        std::make_unique<CommandListPool<D3D12_COMMAND_LIST_TYPE_COMPUTE>>(device);

    m_commandListPools[D3D12_COMMAND_LIST_TYPE_COPY] =
        std::make_unique<CommandListPool<D3D12_COMMAND_LIST_TYPE_COPY>>(device);
}

void CommandManager::Shutdown() {
    Flush();

    // 清理所有 Map 容器，自动调用 unique_ptr 析构
    m_queues.clear();
    m_allocatorPools.clear();
    m_commandListPools.clear();

    m_fenceManager.Shutdown();
    m_frameResources.clear();
    m_device = nullptr;
}

uint64_t CommandManager::SubmitAndSignal(D3D12_COMMAND_LIST_TYPE type, CommandList &cmdList, uint64_t sequence) {
    CommandQueue *queue = GetCommandQueue(type);
    assert(queue != nullptr);

    queue->Execute(cmdList);

    return m_fenceManager.Signal(type, queue->Get(), sequence);
}

uint64_t CommandManager::SubmitAndSignalBatch(D3D12_COMMAND_LIST_TYPE type, std::vector<CommandList> &cmdLists,
                                              uint64_t sequence) {
    if (cmdLists.empty()) {
        return sequence;
    }

    CommandQueue *queue = GetCommandQueue(type);
    assert(queue != nullptr);

    for (auto &cmdList : cmdLists) {
        cmdList.Close();
    }

    queue->ExecuteBatch(cmdLists);

    return m_fenceManager.Signal(type, queue->Get(), sequence);
}

void CommandManager::BeginFrame() {

    uint32_t frameToWait = (m_currentFrame + 1) % m_frameCount;
    uint64_t fenceValue = m_frameResources[frameToWait].fenceValue;

    // 2. 如果该帧有记录的 Fence 值，则进行等待
    if (fenceValue > 0) {
        m_fenceManager.WaitForSequence(D3D12_COMMAND_LIST_TYPE_DIRECT, fenceValue);
    }

    // 标记当前帧为使用中
    m_frameResources[m_currentFrame].inUse = true;
}

void CommandManager::EndFrame() {
    uint32_t frame = m_currentFrame;

    // 获取下一序列号作为本帧的结束标记
    uint64_t sequence = m_fenceManager.GetNextSequence();

    CommandQueue *queue = GetGraphicsQueue();
    if (queue) {
        // 在队列末尾发出信号，记录本帧完成的 Fence 值
        uint64_t fenceValue = m_fenceManager.Signal(D3D12_COMMAND_LIST_TYPE_DIRECT, queue->Get(), sequence);
        m_frameResources[frame].fenceValue = fenceValue;
    }

    // 推进到下一帧
    m_currentFrame = (m_currentFrame + 1) % m_frameCount;

    // 重置即将被复用的那一帧的状态（可选，但在逻辑上更清晰）
    uint32_t nextFrame = m_currentFrame;
    m_frameResources[nextFrame].inUse = false;
}

void CommandManager::WaitForFrame(uint32_t frameIndex, D3D12_COMMAND_LIST_TYPE type) {
    assert(frameIndex < m_frameCount);
    uint64_t fenceValue = m_frameResources[frameIndex].fenceValue;
    if (fenceValue > 0) {
        m_fenceManager.WaitForSequence(type, fenceValue);
    }
}

void CommandManager::WaitForAllFrames(D3D12_COMMAND_LIST_TYPE type) {
    for (uint32_t i = 0; i < m_frameCount; ++i) {
        if (i != m_currentFrame) {
            WaitForFrame(i, type);
        }
    }
}

void CommandManager::Flush(D3D12_COMMAND_LIST_TYPE type) {
    uint64_t sequence = m_fenceManager.GetNextSequence();
    CommandQueue *queue = GetCommandQueue(type);

    if (queue) {
        m_fenceManager.Signal(type, queue->Get(), sequence);
        m_fenceManager.WaitForSequence(type, sequence);
    }
}

CommandManager::PoolStats CommandManager::GetPoolStats() const {
    PoolStats stats;

    // 辅助 Lambda：从 Map 中获取统计信息并填充到 stats 结构体
    auto getAllocStats = [this](D3D12_COMMAND_LIST_TYPE type, size_t &total, size_t &inUse) {
        auto it = m_allocatorPools.find(type);
        if (it != m_allocatorPools.end() && it->second) {
            auto s = it->second->GetStats();
            total = s.totalCount;
            inUse = s.inUseCount;
        }
    };

    auto getListStats = [this](D3D12_COMMAND_LIST_TYPE type, size_t &total, size_t &inUse) {
        auto it = m_commandListPools.find(type);
        if (it != m_commandListPools.end() && it->second) {
            auto s = it->second->GetStats();
            total = s.totalCount;
            inUse = s.inUseCount;
        }
    };

    getAllocStats(D3D12_COMMAND_LIST_TYPE_DIRECT, stats.directAllocatorsTotal, stats.directAllocatorsInUse);
    getAllocStats(D3D12_COMMAND_LIST_TYPE_COMPUTE, stats.computeAllocatorsTotal, stats.computeAllocatorsInUse);
    getAllocStats(D3D12_COMMAND_LIST_TYPE_COPY, stats.copyAllocatorsTotal, stats.copyAllocatorsInUse);

    getListStats(D3D12_COMMAND_LIST_TYPE_DIRECT, stats.directCommandListsTotal, stats.directCommandListsInUse);
    getListStats(D3D12_COMMAND_LIST_TYPE_COMPUTE, stats.computeCommandListsTotal, stats.computeCommandListsInUse);
    getListStats(D3D12_COMMAND_LIST_TYPE_COPY, stats.copyCommandListsTotal, stats.copyCommandListsInUse);

    return stats;
}

uint64_t
CommandManager::SubmitBatch(const std::vector<CommandListPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle> &handles,
                            uint64_t waitSequence) {

    // 调用全确保列表是已关闭的
    if (handles.empty())
        return 0;

    CommandQueue *queue = GetCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT);

    // GPU 端等待（如果需要）
    if (waitSequence > 0) {
        auto *fence = m_fenceManager.GetFence(D3D12_COMMAND_LIST_TYPE_DIRECT);
        if (fence) {
            queue->Wait(fence->Get(), waitSequence);
        }
    }

    // 收集 CommandList 对象
    std::vector<CommandList> cmdLists;
    cmdLists.reserve(handles.size());

    for (const auto &handle : handles) {
        if (handle.IsValid()) {
            CommandList cmdList = GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(handle);
            cmdLists.push_back(cmdList);
        }
    }

    // 使用 CommandQueue 的批量执行方法
    if (!cmdLists.empty()) {
        queue->ExecuteBatch(cmdLists);
    }

    // 返回当前序列号用于后续同步
    return m_fenceManager.GetCurrentSequence();
}

} // namespace DX12Engine::Renderer