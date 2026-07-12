#pragma once

#include "Asset/IO/Loader/DDSLoader.h"
#include "BackgroundExecutor.h"
#include "Renderer/RHI/Command/Utils/CmdTextureUpload.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Texture/TextureManager.h"
#include <d3d12.h>
#include <string>

namespace DX12Engine::Async {

// ========================================================================
// TextureLoadOutput — 纹理加载结果（写入 AssetResult 所需数据）
// ========================================================================
struct TextureLoadOutput {
    Resource::GpuResourceHandle texHandle;
    Resource::TextureHandle texRegHandle; // 注册到 TextureManager 后的句柄
    bool success = false;
};

// ========================================================================
// TextureLoadTask — 从 DDS 文件异步加载纹理到 GPU
//
// 三阶段：
//   cpuWork:  读取文件 → 解析 DDS → 创建 GPU 纹理资源
//   gpuWork:  录制 COPY+DIRECT 命令，提交上传
//   onComplete: 分配 SRV + 注册到 TextureManager → 写入 TextureLoadOutput
//
// 用法:
//   auto result = std::make_shared<TextureLoadOutput>();
//   auto task = TextureLoadTask::Create("stone.dds", device, cmdMgr, texMgr, descHeaps, fence, result);
//   executor.SubmitLoadTask(task);
// ========================================================================

class TextureLoadTask {
public:
    static LoadTask Create(const std::string &filePath, ID3D12Device *device, Renderer::CommandManager *cmdMgr,
                           Resource::TextureManager *texMgr, Resource::DescriptorHeapCollection *descHeaps,
                           uint64_t fence, std::shared_ptr<TextureLoadOutput> outResult = nullptr) {
        LoadTask task;
        task.name = "TexLoad:" + filePath;

        // ── 三段共享状态 ──
        struct SharedState {
            Resource::DDSTextureInfo ddsInfo;
            Resource::GpuResourceHandle texHandle;
            std::vector<uint8_t> fileData; // cpuWork 填充；gpuWork 中 UpdateSubresources 读完才释放
            bool failed = false;
        };
        auto state = std::make_shared<SharedState>();
        auto path = std::make_shared<std::string>(filePath);
        auto result = outResult ? outResult : std::make_shared<TextureLoadOutput>();

        // ── Step 1: CPU（后台线程，文件 I/O + DDS 解析 + 创建 GPU 纹理） ──
        task.cpuWork = [path, state, device]() {
            std::ifstream file(*path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                state->failed = true;
                return;
            }
            size_t fileSize = static_cast<size_t>(file.tellg());
            file.seekg(0, std::ios::beg);
            state->fileData.resize(fileSize);
            file.read(reinterpret_cast<char *>(state->fileData.data()), fileSize);
            file.close();

            if (!Resource::DDSLoader::LoadFromMemory(state->fileData.data(), fileSize, state->ddsInfo)) {
                state->failed = true;
                return;
            }

            auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
            std::wstring wname(path->begin(), path->end());
            state->texHandle =
                gpuMgr.CreateTexture2D(device, state->ddsInfo.desc, wname.c_str(), D3D12_RESOURCE_STATE_COMMON);
            if (!state->texHandle.IsValid())
                state->failed = true;
        };

        // ── Step 2: GPU（后台线程，UPLOAD → COPY 上传 → DIRECT 状态转换） ──
        task.gpuWork = [state, device, cmdMgr]() -> GpuWorkItemPtr {
            if (state->failed || !state->texHandle.IsValid())
                return nullptr;

            auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
            auto *texRes = gpuMgr.GetResource(state->texHandle);
            if (!texRes)
                return nullptr;

            UINT64 copySize =
                GetRequiredIntermediateSize(texRes, 0, static_cast<UINT>(state->ddsInfo.subresources.size()));
            auto upH = gpuMgr.CreateBuffer(device, copySize, L"TexUpload", D3D12_HEAP_TYPE_UPLOAD,
                                           D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!upH.IsValid())
                return nullptr;
            ID3D12Resource *upRes = gpuMgr.GetResource(upH);
            if (!upRes) {
                gpuMgr.Release(upH, 0);
                return nullptr;
            }

            uint64_t copyCompleted = cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_COPY);

            // COPY 队列：COMMON→COPY_DEST→UpdateSubresources→COPY_DEST→COMMON
            auto cpAllocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(copyCompleted);
            auto *cpAlloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(cpAllocH);
            auto cpCmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_COPY>(cpAlloc);
            auto cpCmd = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_COPY>(cpCmdH);
            RecordCopyTexture(cpCmd.Get(), texRes, upRes, 0, state->ddsInfo);
            cpCmd.Close();

            // DIRECT 队列：COMMON→PIXEL_SHADER_RESOURCE
            uint64_t directCompleted = cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
            uint64_t directFence = cmdMgr->GetNextSequence();
            auto drAllocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(directCompleted);
            auto *drAlloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(drAllocH);
            auto drCmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(drAlloc);
            auto drCmd = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(drCmdH);
            RecordTransitionToSRV(drCmd.Get(), texRes);
            drCmd.Close();

            // 注意：不在后台线程释放上传缓冲！
            // 改为存入 GpuWorkItem，由 CheckPendingCompletions 在 GPU 完成后释放
            auto item = std::make_shared<GpuWorkItem>();
            item->uploadBufferHandles.push_back(upH);
            item->copyCmdListHandle = cpCmdH;
            item->copyAllocatorHandle = cpAllocH;
            item->directCmdListHandle = drCmdH;
            item->directAllocatorHandle = drAllocH;
            item->ready.store(true, std::memory_order_release);
            return item;
        };

        // ── Step 3: GPU 完成（主线程）→ 分配 SRV + 注册到 TextureManager ──
        task.onComplete = [state, device, texMgr, descHeaps, result](bool success) {
            if (!success || state->failed || !state->texHandle.IsValid() || !texMgr || !descHeaps) {
                result->success = false;
                return;
            }
            auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
            ID3D12Resource *texRes = gpuMgr.GetResource(state->texHandle);
            if (!texRes) {
                result->success = false;
                return;
            }

            uint32_t srvIndex = descHeaps->Allocate(Resource::PartitionType::Texture);
            if (srvIndex == UINT32_MAX) {
                result->success = false;
                return;
            }

            D3D12_RESOURCE_DESC rDesc = texRes->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = rDesc.Format;
            if (state->ddsInfo.isCubeMap) {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MostDetailedMip = 0;
                srvDesc.TextureCube.MipLevels = rDesc.MipLevels;
            } else {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = 0;
                srvDesc.Texture2D.MipLevels = rDesc.MipLevels;
            }
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            D3D12_CPU_DESCRIPTOR_HANDLE cpuH =
                descHeaps->GetPartitionCpuHandle(Resource::PartitionType::Texture, srvIndex);
            device->CreateShaderResourceView(texRes, &srvDesc, cpuH);

            result->texRegHandle = texMgr->RegisterTexture(state->texHandle, srvIndex);
            result->texHandle = state->texHandle;
            result->success = result->texRegHandle.IsValid();
        };

        return task;
    }
};

} // namespace DX12Engine::Async
