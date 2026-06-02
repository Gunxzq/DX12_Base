#include "CommandManager.h"

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

    CommandQueue *queue = GetGraphicsQueue();
    uint64_t initSequence = m_fenceManager.GetNextSequence();
    m_fenceManager.Signal(D3D12_COMMAND_LIST_TYPE_DIRECT, queue->Get(), initSequence);
    m_fenceManager.WaitForSequence(D3D12_COMMAND_LIST_TYPE_DIRECT, initSequence); // 等待完成

    for (uint32_t i = 0; i < m_frameCount; ++i) {
        m_frameResources[i].fenceValue = initSequence;
    }
}

void CommandManager::Shutdown() {
    // 等待所有队列完成（DIRECT + COMPUTE + COPY），防止资源被提前释放
    FlushAllQueues();

    // 清理所有 Map 容器，自动调用 unique_ptr 析构
    m_queues.clear();
    m_allocatorPools.clear();
    m_commandListPools.clear();

    m_fenceManager.Shutdown();
    m_frameResources.clear();
    m_device = nullptr;
}

void CommandManager::BeginFrame() {

    uint32_t frameToWait = (m_currentFrame + m_frameCount - 1) % m_frameCount;

    uint64_t fenceValue = m_frameResources[frameToWait].fenceValue;

    if (fenceValue > 0) {
        m_fenceManager.WaitForSequence(D3D12_COMMAND_LIST_TYPE_DIRECT, fenceValue);
    }

    m_frameResources[m_currentFrame].inUse = true;
}

void CommandManager::EndFrame() {
    uint32_t frame = m_currentFrame;
    uint64_t sequence = m_fenceManager.GetCurrentSequence();
    CommandQueue *queue = GetGraphicsQueue();
    uint64_t fenceValue = m_fenceManager.Signal(D3D12_COMMAND_LIST_TYPE_DIRECT, queue->Get(), sequence);
    m_frameResources[frame].fenceValue = fenceValue;
    m_currentFrame = (m_currentFrame + 1) % m_frameCount;
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

/**
 * @brief 提交命令批次
 * @param handles
 * @param waitSequence
 * @return uint64_t
 * @note 提交批次，但是不设置GPU端值，并不是真正的等待完成
 */
void CommandManager::SubmitBatch(const std::vector<CommandListPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle> &handles,
                                 uint64_t waitSequence) {

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
}

void CommandManager::Submit(D3D12_COMMAND_LIST_TYPE type, CommandList &cmdList) {
    assert(cmdList.IsValid() && "Cannot submit invalid command list");

    CommandQueue *queue = GetCommandQueue(type);
    if (queue) {
        queue->Execute(cmdList);
    }
}

} // namespace DX12Engine::Renderer