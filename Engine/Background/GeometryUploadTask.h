#pragma once

#include "BackgroundExecutor.h"
#include "Resource/Core/GpuHandlePool.h"
#include "Resource/GpuResourceManager.h"
#include <cstdint>

namespace DX12Engine::Async {

// ========================================================================
// GeometryUploadTask — 上传 CPU 顶点/索引数据到 GPU
//
// 输入CPU数据 → 创建 VB/IB（UPLOAD 堆, Map/Unmap）→ 输出 handle
//
// 用法:
//   auto task = GeometryUploadTask::Create("cube_vb", vertices.data(), count,
//                                          stride, indices.data(), idxCount, bounds);
//   executor.Submit(task);
// ========================================================================

struct GeometryUploadResult {
    Resource::GpuResourceHandle vbHandle;
    Resource::GpuResourceHandle ibHandle;
    uint32_t indexCount = 0;
};

class GeometryUploadTask {
public:
    static LoadTask Create(const std::string &name, const void *vertexData, size_t vertexCount, uint32_t vertexStride,
                           const void *indexData, uint32_t indexCount, ID3D12Device *device) {
        LoadTask task;
        task.name = name;

        auto result = std::make_shared<GeometryUploadResult>();
        result->indexCount = indexCount;

        task.cpuWork = [result, vertexData, vertexCount, vertexStride, indexData, indexCount, device, name]() {
            auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
            std::wstring wname(name.begin(), name.end());

            size_t vbSize = vertexCount * vertexStride;
            result->vbHandle = gpuMgr.CreateBuffer(device, vbSize, wname.c_str(), D3D12_HEAP_TYPE_UPLOAD,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ);
            if (auto *res = gpuMgr.GetResource(result->vbHandle)) {
                void *mapped = nullptr;
                res->Map(0, nullptr, &mapped);
                memcpy(mapped, vertexData, vbSize);
                res->Unmap(0, nullptr);
            }

            size_t ibSize = indexCount * sizeof(uint32_t);
            result->ibHandle = gpuMgr.CreateBuffer(device, ibSize, wname.c_str(), D3D12_HEAP_TYPE_UPLOAD,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ);
            if (auto *res = gpuMgr.GetResource(result->ibHandle)) {
                void *mapped = nullptr;
                res->Map(0, nullptr, &mapped);
                memcpy(mapped, indexData, ibSize);
                res->Unmap(0, nullptr);
            }
        };

        task.onComplete = [result](bool) { /* result 可通过 shared_ptr 外部获取 */ };
        return task;
    }
};

} // namespace DX12Engine::Async
