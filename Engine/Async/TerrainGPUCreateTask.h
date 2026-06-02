// Engine/Async/TerrainGPUCreateTask.h
#pragma once

#include "Event/EventRegistry.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Scheduler/Task.h"
#include "TerrainLoadTask.h"
#include <memory>

namespace DX12Engine::Async {

// 使用 EventRegistry 中定义的枚举值作为事件哈希
constexpr uint32_t TERRAIN_GEOMETRY_UPLOADED_EVENT_HASH = static_cast<uint32_t>(Event::EventType::TerrainGeometryUploadedEvent);

/**
 * @brief 地形 GPU 资源创建任务工厂 — 在 Render 线程创建 VB/IB 并注册
 *
 * 该任务在 Render 线程执行（因为需要访问 D3D12 设备）：
 * 1. 从 TerrainLoadData 读取 CPU 端网格数据
 * 2. 创建 UPLOAD 堆 VB/IB 并映射写入数据
 * 3. 注册到 GeometryResourceManager
 * 4. 将 GeometryHandle 写入输出 shared_ptr
 */
class TerrainGPUCreateTaskFactory {
public:
    static Scheduler::Task Create(uint32_t requestId, TerrainLoadDataPtr data, ID3D12Device *device,
                                  Resource::GeometryResourceManager *geoMgr, Resource::GeometryHandle *outHandle) {
        Scheduler::Task task;
        task.name = "TerrainGPUCreateTask";
        task.phase = Scheduler::TaskPhase::Update;
        task.thread = Scheduler::ThreadType::Render;
        task.priority = static_cast<uint32_t>(Scheduler::TaskPriority::Normal);

        task.execute = [requestId, data, device, geoMgr, outHandle]() {
            if (!data || data->vertices.empty()) {
                return;
            }

            auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

            // 1. 创建顶点缓冲区
            size_t vbSize = data->vertices.size() * sizeof(GeometryGenerator::Vertex);
            auto vbHandle = gpuMgr.CreateBuffer(device, vbSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

            ID3D12Resource *vbResource = gpuMgr.GetResource(vbHandle);
            if (vbResource) {
                void *mapped = nullptr;
                vbResource->Map(0, nullptr, &mapped);
                memcpy(mapped, data->vertices.data(), vbSize);
                vbResource->Unmap(0, nullptr);
            }

            // 2. 创建索引缓冲区
            size_t ibSize = data->indices.size() * sizeof(uint32_t);
            auto ibHandle = gpuMgr.CreateBuffer(device, ibSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

            ID3D12Resource *ibResource = gpuMgr.GetResource(ibHandle);
            if (ibResource) {
                void *mapped = nullptr;
                ibResource->Map(0, nullptr, &mapped);
                memcpy(mapped, data->indices.data(), ibSize);
                ibResource->Unmap(0, nullptr);
            }

            // 3. 构建 TriangleMesh
            Resource::TriangleMesh mesh;
            mesh.vertexBufferHandle = vbHandle;
            mesh.indexBufferHandle = ibHandle;
            mesh.vertexCount = static_cast<uint32_t>(data->vertices.size());
            mesh.indexCount = static_cast<uint32_t>(data->indices.size());
            mesh.vertexStride = sizeof(GeometryGenerator::Vertex);
            mesh.indexFormat = DXGI_FORMAT_R32_UINT;
            mesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            mesh.isGpuReady = true;
            mesh.localBounds = data->bounds;

            // 4. 注册到 GeometryResourceManager
            auto handle = geoMgr->RegisterTriangleMesh(mesh);
            *outHandle = handle;

            // 5. 发送 GPU 上传完成事件（Step 3: ResourceUploaded，携带围栏值）
            uint64_t payload = (static_cast<uint64_t>(requestId) << 42) | (static_cast<uint64_t>(handle.generation) << 32) |
                               (static_cast<uint64_t>(handle.index) & 0xFFFFFFFF);
            Event::MessageDispatcher::GetInstance()->PostEvent(TERRAIN_GEOMETRY_UPLOADED_EVENT_HASH, 0, payload,
                                                               Event::EventPriority::P4_Background);
        };

        return task;
    }
};

} // namespace DX12Engine::Async
