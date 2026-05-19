#pragma once
#include "Allocator/CommandAllocatorPool.h"
#include "CommandList/CommandListPool.h"
#include "CommandQueue.h"
#include "Fence/FenceManager.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <map> // 新增
#include <memory>
#include <shared_mutex>
#include <vector>

namespace DX12Engine::Renderer {

// ========================================================================
// CommandManager - 命令系统中枢
// ========================================================================

class CommandManager {
public:
    static constexpr uint32_t DEFAULT_FRAME_COUNT = 3;
    static constexpr size_t INITIAL_ALLOCATOR_POOL_SIZE = 8;
    static constexpr size_t INITIAL_COMMANDLIST_POOL_SIZE = 8;

    struct FrameResource {
        uint64_t fenceValue = 0;
        bool inUse = false;
    };

public:
    CommandManager() = default;
    ~CommandManager() { Shutdown(); }

    CommandManager(const CommandManager &) = delete;
    CommandManager &operator=(const CommandManager &) = delete;
    CommandManager(CommandManager &&) = delete;
    CommandManager &operator=(CommandManager &&) = delete;

    void Initialize(ID3D12Device *device, uint32_t frameCount = DEFAULT_FRAME_COUNT);
    void Shutdown();

    // ========================================================================
    // 工作线程接口（无锁，线程安全）
    // ========================================================================

    uint64_t GetNextSequence() { return m_fenceManager.GetNextSequence(); }

    CommandQueue *GetCommandQueue(D3D12_COMMAND_LIST_TYPE type) const {
        auto it = m_queues.find(type);
        return it != m_queues.end() ? it->second.get() : nullptr;
    }

    CommandQueue *GetGraphicsQueue() const { return GetCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT); }
    CommandQueue *GetComputeQueue() const { return GetCommandQueue(D3D12_COMMAND_LIST_TYPE_COMPUTE); }
    CommandQueue *GetCopyQueue() const { return GetCommandQueue(D3D12_COMMAND_LIST_TYPE_COPY); }

    template <D3D12_COMMAND_LIST_TYPE Type>
    typename CommandAllocatorPool<Type>::Handle AcquireAllocator(uint64_t currentCompleted) {
        auto it = m_allocatorPools.find(Type);
        assert(it != m_allocatorPools.end() && "Allocator pool not initialized for this type");

        auto *specificPool = static_cast<CommandAllocatorPool<Type> *>(it->second.get());
        return specificPool->Acquire(currentCompleted);
    }

    template <D3D12_COMMAND_LIST_TYPE Type>
    void ReleaseAllocator(const typename CommandAllocatorPool<Type>::Handle &handle, uint64_t fenceValue) {
        auto it = m_allocatorPools.find(Type);
        assert(it != m_allocatorPools.end());
        auto *specificPool = static_cast<CommandAllocatorPool<Type> *>(it->second.get());
        specificPool->Release(handle, fenceValue);
    }

    template <D3D12_COMMAND_LIST_TYPE Type>
    ID3D12CommandAllocator *GetAllocator(const typename CommandAllocatorPool<Type>::Handle &handle) {
        auto it = m_allocatorPools.find(Type);
        assert(it != m_allocatorPools.end() && "Allocator pool not initialized for this type");
        assert(handle.IsValid() && handle.allocator != nullptr);
        return handle.allocator->Get();
    }

    template <D3D12_COMMAND_LIST_TYPE Type>
    typename CommandListPool<Type>::Handle AcquireCommandListHandle(ID3D12CommandAllocator *allocator) {
        auto it = m_commandListPools.find(Type);
        assert(it != m_commandListPools.end() && "CommandList pool not initialized for this type");
        auto *specificPool = static_cast<CommandListPool<Type> *>(it->second.get());
        return specificPool->AcquireHandle(allocator);
    }

    template <D3D12_COMMAND_LIST_TYPE Type>
    CommandList GetCommandList(const typename CommandListPool<Type>::Handle &handle) {
        auto it = m_commandListPools.find(Type);
        assert(it != m_commandListPools.end());
        auto *specificPool = static_cast<CommandListPool<Type> *>(it->second.get());
        return specificPool->GetCommandList(handle);
    }

    template <D3D12_COMMAND_LIST_TYPE Type>
    void ReleaseCommandList(const typename CommandListPool<Type>::Handle &handle) {
        auto it = m_commandListPools.find(Type);
        assert(it != m_commandListPools.end());
        auto *specificPool = static_cast<CommandListPool<Type> *>(it->second.get());
        specificPool->Release(handle);
    }

    // ------------------------------------------------------------------------
    // 提交接口
    // ------------------------------------------------------------------------

    void Submit(D3D12_COMMAND_LIST_TYPE type, CommandList &cmdList);

    void SubmitBatch(const std::vector<CommandListPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle> &handles,
                     uint64_t waitSequence);

    // ========================================================================
    // 主线程接口
    // ========================================================================

    void BeginFrame();
    void EndFrame();
    void WaitForFrame(uint32_t frameIndex, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT);
    void WaitForAllFrames(D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT);
    void Flush(D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT);

    void FlushAllQueues() {
        Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);
        Flush(D3D12_COMMAND_LIST_TYPE_COMPUTE);
        Flush(D3D12_COMMAND_LIST_TYPE_COPY);
    }

    uint32_t GetCurrentFrame() const { return m_currentFrame; }
    uint32_t GetFrameCount() const { return m_frameCount; }
    uint64_t GetFrameFenceValue(uint32_t frameIndex) const {
        assert(frameIndex < m_frameCount);
        return m_frameResources[frameIndex].fenceValue;
    }

    FenceManager &GetFenceManager() { return m_fenceManager; }
    const FenceManager &GetFenceManager() const { return m_fenceManager; }

    uint64_t GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) {
        Fence *fence = m_fenceManager.GetFence(type);
        return fence ? fence->Get()->GetCompletedValue() : 0;
    }

    // ========================================================================
    // 调试和诊断
    // ========================================================================

    struct PoolStats {
        size_t directAllocatorsTotal = 0;
        size_t directAllocatorsInUse = 0;
        size_t computeAllocatorsTotal = 0;
        size_t computeAllocatorsInUse = 0;
        size_t copyAllocatorsTotal = 0;
        size_t copyAllocatorsInUse = 0;

        size_t directCommandListsTotal = 0;
        size_t directCommandListsInUse = 0;
        size_t computeCommandListsTotal = 0;
        size_t computeCommandListsInUse = 0;
        size_t copyCommandListsTotal = 0;
        size_t copyCommandListsInUse = 0;
    };
    PoolStats GetPoolStats() const;

private:
    ID3D12Device *m_device = nullptr;
    uint32_t m_frameCount = DEFAULT_FRAME_COUNT;
    uint32_t m_currentFrame = 0;
    std::vector<FrameResource> m_frameResources;

    FenceManager m_fenceManager;

    // 统一管理队列
    std::map<D3D12_COMMAND_LIST_TYPE, std::unique_ptr<CommandQueue>> m_queues;

    // 统一管理分配器池 (基类指针)
    std::map<D3D12_COMMAND_LIST_TYPE, std::unique_ptr<ICommandAllocatorPool>> m_allocatorPools;

    // 统一管理命令列表池 (基类指针)
    std::map<D3D12_COMMAND_LIST_TYPE, std::unique_ptr<ICommandListPool>> m_commandListPools;

    mutable std::shared_mutex m_mutex;
};

} // namespace DX12Engine::Renderer