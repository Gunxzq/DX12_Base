#pragma once

#include "Asset/Definitions/Mesh/DxMeshFormat.h"
#include "BackgroundExecutor.h"
#include "Math/BoundingVolume.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Resource/Core/GpuHandlePool.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace DX12Engine::Async {

// ========================================================================
// MeshLoadOutput — 网格加载结果
// ========================================================================
struct MeshLoadOutput {
    Resource::GeometryHandle geometryHandle;
    bool success = false;
};

// ========================================================================
// MeshLoadTask — 从 .dxmesh 文件异步加载网格到 GPU
//
// 三阶段：
//   cpuWork:  读取文件 → 解析顶点/索引数据
//   gpuWork:  创建 DEFAULT VB/IB + UPLOAD 中转 → 录制 COPY → 状态转换
//   onComplete: 注册到 GeometryResourceManager
//
// 用法:
//   auto result = std::make_shared<MeshLoadOutput>();
//   auto task = MeshLoadTask::Create("cube.dxmesh", device, cmdMgr, geoMgr, fence, result);
//   executor.SubmitLoadTask(task);
// ========================================================================

class MeshLoadTask {
public:
    static LoadTask Create(const std::string &filePath, ID3D12Device *device, Renderer::CommandManager *cmdMgr,
                           Resource::GeometryResourceManager *geoMgr, uint64_t fence,
                           std::shared_ptr<MeshLoadOutput> outResult = nullptr) {
        LoadTask task;
        task.name = "MeshLoad:" + filePath;

        // ── 三段共享状态 ──
        struct SharedState {
            std::vector<uint8_t> vertexData;
            std::vector<uint8_t> indexData;
            uint32_t vertexCount = 0;
            uint32_t indexCount = 0;
            uint32_t vertexStride = 0;
            DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT;
            Math::BoundingAABB localBounds;
            bool failed = false;
        };
        auto state = std::make_shared<SharedState>();
        auto path = std::make_shared<std::string>(filePath);
        auto triMesh = std::make_shared<Resource::TriangleMesh>();
        auto result = outResult ? outResult : std::make_shared<MeshLoadOutput>();

        // ── Step 1: CPU（后台线程，文件 I/O + 解析） ──
        task.cpuWork = [path, state]() {
            std::ifstream file(*path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                state->failed = true;
                return;
            }
            size_t fileSize = static_cast<size_t>(file.tellg());
            file.seekg(0, std::ios::beg);
            std::vector<uint8_t> fileData(fileSize);
            file.read(reinterpret_cast<char *>(fileData.data()), fileSize);
            file.close();

            if (fileData.size() < sizeof(DxMeshHeader)) {
                state->failed = true;
                return;
            }
            const auto *header = reinterpret_cast<const DxMeshHeader *>(fileData.data());
            if (memcmp(header->magic, DX_MESH_MAGIC, 8) != 0) {
                state->failed = true;
                return;
            }

            uint32_t vtxSize = header->vertexCount * header->vertexStride;
            uint32_t idxSize = header->indexCount * header->indexSize;
            const void *vtxData = DxMesh_GetVertexData(header);
            const void *idxData = DxMesh_GetIndexData(header);

            state->vertexCount = header->vertexCount;
            state->indexCount = header->indexCount;
            state->vertexStride = header->vertexStride;
            state->indexFormat = (header->indexSize == 2) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

            state->vertexData.resize(vtxSize);
            state->indexData.resize(idxSize);
            memcpy(state->vertexData.data(), vtxData, vtxSize);
            memcpy(state->indexData.data(), idxData, idxSize);

            state->localBounds.min =
                DirectX::XMFLOAT3(header->boundsMin[0], header->boundsMin[1], header->boundsMin[2]);
            state->localBounds.max =
                DirectX::XMFLOAT3(header->boundsMax[0], header->boundsMax[1], header->boundsMax[2]);
        };

        // ── Step 2: GPU（后台线程，DEFAULT + UPLOAD → COPY → DIRECT 屏障） ──
        task.gpuWork = [state, device, cmdMgr, triMesh]() -> GpuWorkItemPtr {
            if (state->failed)
                return nullptr;

            auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
            uint32_t vtxSize = state->vertexCount * state->vertexStride;
            uint32_t idxSize = state->indexCount * (state->indexFormat == DXGI_FORMAT_R16_UINT ? 2u : 4u);

            // DEFAULT VB/IB
            auto vb = gpuMgr.CreateBuffer(device, vtxSize, L"Mesh_VB_Default", D3D12_HEAP_TYPE_DEFAULT,
                                          D3D12_RESOURCE_STATE_COPY_DEST);
            if (!vb.IsValid())
                return nullptr;
            auto ib = gpuMgr.CreateBuffer(device, idxSize, L"Mesh_IB_Default", D3D12_HEAP_TYPE_DEFAULT,
                                          D3D12_RESOURCE_STATE_COPY_DEST);
            if (!ib.IsValid()) {
                gpuMgr.Release(vb, 0);
                return nullptr;
            }

            // UPLOAD 中转
            auto upVB = gpuMgr.CreateBuffer(device, vtxSize, L"Mesh_VB_Up", D3D12_HEAP_TYPE_UPLOAD,
                                            D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!upVB.IsValid()) {
                gpuMgr.Release(vb, 0);
                gpuMgr.Release(ib, 0);
                return nullptr;
            }
            auto upIB = gpuMgr.CreateBuffer(device, idxSize, L"Mesh_IB_Up", D3D12_HEAP_TYPE_UPLOAD,
                                            D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!upIB.IsValid()) {
                gpuMgr.Release(vb, 0);
                gpuMgr.Release(ib, 0);
                gpuMgr.Release(upVB, 0);
                return nullptr;
            }

            // Map + memcpy
            auto *upVBRes = gpuMgr.GetResource(upVB);
            auto *upIBRes = gpuMgr.GetResource(upIB);
            void *mapped = nullptr;
            upVBRes->Map(0, nullptr, &mapped);
            memcpy(mapped, state->vertexData.data(), vtxSize);
            upVBRes->Unmap(0, nullptr);
            upIBRes->Map(0, nullptr, &mapped);
            memcpy(mapped, state->indexData.data(), idxSize);
            upIBRes->Unmap(0, nullptr);

            // 释放 CPU 数据
            state->vertexData.clear();
            state->vertexData.shrink_to_fit();
            state->indexData.clear();
            state->indexData.shrink_to_fit();

            // COPY 队列（用当前 GPU 已完成值安全获取 allocator）
            uint64_t copyCompleted = cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_COPY);

            // COPY 队列
            auto cpAllocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(copyCompleted);
            auto *cpAlloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(cpAllocH);
            auto cpCmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_COPY>(cpAlloc);
            auto cpCmd = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_COPY>(cpCmdH);
            cpCmd.Get()->CopyResource(gpuMgr.GetResource(vb), upVBRes);
            cpCmd.Get()->CopyResource(gpuMgr.GetResource(ib), upIBRes);
            cpCmd.Close();

            // DIRECT 队列：COPY_DEST → VB/IB
            uint64_t directCompleted = cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
            uint64_t directFence = cmdMgr->GetNextSequence();
            auto drAllocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(directCompleted);
            auto *drAlloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(drAllocH);
            auto drCmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(drAlloc);
            auto drCmd = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(drCmdH);
            D3D12_RESOURCE_BARRIER barriers[2] = {
                CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(vb), D3D12_RESOURCE_STATE_COPY_DEST,
                                                     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
                CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(ib), D3D12_RESOURCE_STATE_COPY_DEST,
                                                     D3D12_RESOURCE_STATE_INDEX_BUFFER)};
            drCmd.Get()->ResourceBarrier(2, barriers);
            drCmd.Close();

            // 注意：不在后台线程释放上传缓冲！
            // 改为存入 GpuWorkItem，由 CheckPendingCompletions 在 GPU 完成后释放
            auto item = std::make_shared<GpuWorkItem>();
            item->uploadBufferHandles.push_back(upVB);
            item->uploadBufferHandles.push_back(upIB);

            // 填写 TriangleMesh
            triMesh->vertexBufferHandle = vb;
            triMesh->indexBufferHandle = ib;
            triMesh->vertexCount = state->vertexCount;
            triMesh->indexCount = state->indexCount;
            triMesh->vertexStride = state->vertexStride;
            triMesh->indexFormat = state->indexFormat;
            triMesh->topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            triMesh->isGpuReady = true;
            triMesh->localBounds = state->localBounds;

            item->copyCmdListHandle = cpCmdH;
            item->copyAllocatorHandle = cpAllocH;
            item->directCmdListHandle = drCmdH;
            item->directAllocatorHandle = drAllocH;
            item->ready.store(true, std::memory_order_release);
            return item;
        };

        // ── Step 3: GPU 完成（主线程）→ 注册到 GeometryResourceManager ──
        task.onComplete = [triMesh, geoMgr, result](bool success) {
            if (success && triMesh->isGpuReady && geoMgr) {
                result->geometryHandle = geoMgr->RegisterGeometry(*triMesh);
                result->success = result->geometryHandle.IsValid();
            }
        };

        return task;
    }
};

} // namespace DX12Engine::Async
