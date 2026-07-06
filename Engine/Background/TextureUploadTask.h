#pragma once

#include "BackgroundExecutor.h"
#include "Renderer/RHI/Command/Utils/GpuUpload.h"
#include "Resource/AssetLoader/Loader/DDSLoader.h"
#include "Resource/Core/GpuHandlePool.h"
#include "Resource/GpuResourceManager.h"
#include <d3d12.h>

namespace DX12Engine::Async {

// ========================================================================
// TextureUploadTask — 上传 DDS 纹理数据到 GPU
//
// 遵循 LoadTask 三段式：
//   cpuWork:  创建 GPU 纹理资源
//   gpuWork:  录制 COPY+DIRECT 命令，返回 GpuWorkItem
//   onComplete: 拿到 GPU handle
//
// 用法:
//   auto task = TextureUploadTask::Create("stone", ddsInfo, device, cmdMgr, fence);
//   executor.SubmitLoadTask(task);
// ========================================================================

struct TextureUploadResult {
    Resource::GpuResourceHandle gpuHandle;
};

class TextureUploadTask {
public:
    static LoadTask Create(const std::string &name, const Resource::DDSTextureInfo &ddsInfo, ID3D12Device *device,
                           Renderer::CommandManager *cmdMgr, uint64_t fence) {
        LoadTask task;
        task.name = name;

        struct SharedData {
            Resource::GpuResourceHandle texHandle;
            ID3D12Resource *texResource = nullptr;
            bool ready = false;
        };
        auto data = std::make_shared<SharedData>();

        // Step 1: 创建 GPU 纹理（后台线程，纯 CPU）
        task.cpuWork = [data, ddsInfo, device, name]() {
            auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
            data->texHandle = gpuMgr.CreateTexture2D(
                device, ddsInfo.desc, std::wstring(name.begin(), name.end()).c_str(), D3D12_RESOURCE_STATE_COMMON);
            if (data->texHandle.IsValid())
                data->texResource = gpuMgr.GetResource(data->texHandle);
            data->ready = data->texResource != nullptr;
        };

        // Step 2: 录制 COPY+DIRECT 命令（后台线程）
        task.gpuWork = [data, ddsInfo, device, cmdMgr, fence]() -> GpuWorkItemPtr {
            if (!data->ready)
                return nullptr;

            auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
            auto item = std::make_shared<GpuWorkItem>();

            // 创建上传缓冲区
            UINT64 copySize =
                GetRequiredIntermediateSize(data->texResource, 0, static_cast<UINT>(ddsInfo.subresources.size()));
            auto uploadH = gpuMgr.CreateBuffer(device, copySize, L"TexUpload", D3D12_HEAP_TYPE_UPLOAD,
                                               D3D12_RESOURCE_STATE_GENERIC_READ);
            item->uploadBufferHandle = uploadH;
            ID3D12Resource *uploadRes = gpuMgr.GetResource(uploadH);
            if (!uploadRes)
                return nullptr;

            // 获取 COPY 命令列表
            auto cpAllocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(fence);
            auto *cpAlloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(cpAllocH);
            auto cpCmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_COPY>(cpAlloc);
            auto cpCmd = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_COPY>(cpCmdH);
            // Pool 已内部 Reset，直接录制
            item->copyCmdListHandle = cpCmdH;
            item->copyAllocatorHandle = cpAllocH;

            // 获取 DIRECT 命令列表
            auto drAllocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
            auto *drAlloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(drAllocH);
            auto drCmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(drAlloc);
            auto drCmd = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(drCmdH);
            // Pool 已内部 Reset，直接录制
            item->directCmdListHandle = drCmdH;
            item->directAllocatorHandle = drAllocH;

            // 录制命令（只录制，不涉及分配器管理）
            RecordTextureUpload(cpCmd.Get(), drCmd.Get(), data->texResource, uploadRes, 0, ddsInfo);

            cpCmd.Close();
            drCmd.Close();
            item->ready.store(true, std::memory_order_release);
            return item;
        };

        // Step 3: 完成回调（主线程）
        task.onComplete = [data](bool) { /* data->texHandle 可通过 shared_ptr 外部获取 */ };

        return task;
    }
};

} // namespace DX12Engine::Async
