// Engine/Async/ResourceTransitionTask.h
#pragma once

#include "Event/EventRegistry.h"
#include "Event/EventTypes.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Resource/AssetDataManager.h"
#include "Resource/GpuResourceManager.h"
#include <memory>
#include <string>

namespace DX12Engine::Async {

/**
 * @brief 资源转换任务 — 在 COPY 队列完成上传后，在 DIRECT 队列执行屏障转换
 *
 * 设计目的：
 * - COPY 队列不支持 ResourceBarrier（标准屏障），只能执行纯 Copy 命令
 * - 因此 Copy 操作在 COPY 队列完成，ResourceBarrier 在 DIRECT 队列完成
 * - 事件驱动，不阻塞主渲染线程
 *
 * 数据流：
 *   1. COPY 队列：UpdateSubresources / CopyTextureRegion（纯拷贝）
 *   2. COPY 队列 Signal → 记录 copyFence
 *   3. DIRECT 队列：Wait(copyFence) → ResourceBarrier(COPY_DEST → SRV) → Signal → 记录 transitionFence
 *   4. TerrainUploadCompletionSystem 检查 transitionFence 完成 → PostEvent(TerrainReady)
 */
class ResourceTransitionTaskFactory {
public:
    /**
     * @brief 在 DIRECT 队列上提交资源屏障转换任务
     *
     * @param deviceContext 设备上下文（用于获取 CommandManager）
     * @param resource     需要转换状态的 GPU 资源
     * @param stateBefore  转换前状态（通常为 D3D12_RESOURCE_STATE_COPY_DEST）
     * @param stateAfter   转换后状态（通常为 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE）
     * @param waitFence    等待的 COPY 队列 fence 值（0 表示不等待）
     * @param fenceKey     用于存储 transitionFence 的 AssetDataManager key
     * @return transitionFence 值（0 表示失败）
     */
    static uint64_t SubmitTransition(ID3D12Device *device,
                                     Renderer::CommandManager &cmdMgr,
                                     ID3D12Resource *resource,
                                     D3D12_RESOURCE_STATES stateBefore,
                                     D3D12_RESOURCE_STATES stateAfter,
                                     uint64_t waitFence,
                                     const std::string &fenceKey) {
        if (!device || !resource) {
            return 0;
        }

        // 从 DIRECT 队列池获取命令列表
        uint64_t completedFence = cmdMgr.GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
        auto allocatorHandle = cmdMgr.AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
        auto *allocator = cmdMgr.GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
        auto cmdListHandle = cmdMgr.AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
        auto cmdList = cmdMgr.GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

        // 如果需要在 COPY 队列完成后再执行屏障，先 Wait
        // 注意：ID3D12CommandQueue::Wait 是 GPU 端跨队列同步，不阻塞 CPU
        if (waitFence > 0) {
            Renderer::Fence *copyFence = cmdMgr.GetFenceManager().GetFence(D3D12_COMMAND_LIST_TYPE_COPY);
            if (copyFence) {
                cmdMgr.GetGraphicsQueue()->Get()->Wait(copyFence->Get(), waitFence);
            }
        }

        // 执行资源屏障转换
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, stateBefore, stateAfter);
        cmdList.Get()->ResourceBarrier(1, &barrier);

        cmdList.Close();

        // 提交到 DIRECT 队列
        cmdMgr.Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList);

        // Signal 并记录 fence value
        uint64_t transitionFence = cmdMgr.GetNextSequence();
        cmdMgr.GetFenceManager().Signal(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         cmdMgr.GetGraphicsQueue()->Get(),
                                         transitionFence);

        // 将 fence value 存入 AssetDataManager，供后续 System 检查
        if (!fenceKey.empty()) {
            Resource::AssetDataManager::GetInstance().StoreTypedData<uint64_t>(
                fenceKey, std::make_shared<uint64_t>(transitionFence));
        }

        return transitionFence;
    }
};

} // namespace DX12Engine::Async
