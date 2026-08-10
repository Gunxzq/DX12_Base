#pragma once

#include "BackgroundExecutor.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include <memory>
#include <string>

namespace DX12Engine::Async {

// ========================================================================
// GeometryProceduralOutput — 程序化几何体生成结果
// ========================================================================
struct GeometryProceduralOutput {
    Resource::GeometryHandle geometryHandle;
    bool success = false;
};

// ========================================================================
// GeometryProceduralTask — 程序化几何体异步生成并上传 GPU
//
// 三阶段：
//   cpuWork:  GeometryGenerator → CPU MeshData
//   gpuWork:  创建 DEFAULT VB/IB + UPLOAD 中转 → 录制 COPY → 录制 DIRECT barrier
//   onComplete: 注册到 GeometryResourceManager
//
// 用法：
//   auto result = std::make_shared<GeometryProceduralOutput>();
//   auto task = GeometryProceduralTask::Create(type, params, device, cmdMgr, geoMgr, result);
//   executor.SubmitLoadTask(task);
// ========================================================================

class GeometryProceduralTask {
public:
    static LoadTask Create(const std::string &type, const Resource::ProceduralGeometryDesc &params,
                           ID3D12Device *device, Renderer::CommandManager *cmdMgr,
                           Resource::GeometryResourceManager *geoMgr,
                           std::shared_ptr<GeometryProceduralOutput> outResult = nullptr) {
        LoadTask task;
        task.name = "ProceduralGeo:" + type;

        // ── 三段共享状态 ──
        struct SharedState {
            std::vector<uint8_t> vertexData;
            std::vector<uint8_t> indexData;
            uint32_t vertexCount = 0;
            uint32_t indexCount = 0;
            uint32_t vertexStride = sizeof(GeometryGenerator::Vertex);
            DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT;
            std::string type;           // 生成类型（"sphere" / "grid" / 默认 "cube"）
            uint32_t widthSegments = 0; // grid：X 方向分段数
            uint32_t depthSegments = 0; // grid：Z 方向分段数
            bool failed = false;
            bool gpuReady = false; // gpuWork 成功填写几何后置位（onComplete 兜底——gpuWork 返回
                                   // nullptr 时 BackgroundExecutor 以 success=true 回调，见
                                   // BackgroundExecutor.cpp DeferToMainThread）
        };
        auto state = std::make_shared<SharedState>();
        state->type = type;
        auto geometryOut = std::make_shared<Resource::GeometryVariant>();
        auto result = outResult ? outResult : std::make_shared<GeometryProceduralOutput>();

        // ── Step 1: CPU（后台线程，生成几何体） ──
        task.cpuWork = [type, params, state]() {
            GeometryGenerator gen;
            GeometryGenerator::MeshData meshData;
            if (type == "sphere") {
                meshData = gen.CreateSphere(params.radius, params.segments, params.rings);
            } else if (type == "grid") {
                // 程序化水面四边形（对齐 WaterSystemArchitecture §十 定案）：
                // CreateGrid(width, depth, m, n) 中 m=Z 向顶点数、n=X 向顶点数（分段数+1）
                meshData =
                    gen.CreateGrid(params.width, params.depth, params.depthSegments + 1, params.widthSegments + 1);
                state->widthSegments = params.widthSegments;
                state->depthSegments = params.depthSegments;
            } else {
                // 默认生成立方体（"cube" 或其他值均走此路径）
                meshData = gen.CreateBox(1.0f, 1.0f, 1.0f, 0);
            }

            if (meshData.Vertices.empty() || meshData.Indices32.empty()) {
                state->failed = true;
                return;
            }

            state->vertexCount = static_cast<uint32_t>(meshData.Vertices.size());
            state->indexCount = static_cast<uint32_t>(meshData.Indices32.size());

            size_t vtxSize = meshData.Vertices.size() * sizeof(GeometryGenerator::Vertex);
            size_t idxSize = meshData.Indices32.size() * sizeof(uint32_t);

            state->vertexData.resize(vtxSize);
            state->indexData.resize(idxSize);
            memcpy(state->vertexData.data(), meshData.Vertices.data(), vtxSize);
            memcpy(state->indexData.data(), meshData.Indices32.data(), idxSize);
        };

        // ── Step 2: GPU（后台线程，DEFAULT + UPLOAD → COPY → DIRECT 屏障） ──
        task.gpuWork = [state, device, cmdMgr, geometryOut, params]() -> GpuWorkItemPtr {
            if (state->failed)
                return nullptr;

            auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
            uint32_t vtxSize = state->vertexCount * state->vertexStride;
            uint32_t idxSize = state->indexCount * sizeof(uint32_t);

            // DEFAULT VB/IB（初始 COMMON——COPY 命令列表 CopyResource 目标必须以 COMMON 创建，
            // 不能用 COPY_DEST：GBV #942 "Resources used in COPY command lists must start out in the
            // D3D12_RESOURCE_STATE_COMMON state. This includes Resources created in a COPY_SOURCE or COPY_DEST state."
            // 目标状态（VB/IB）由 DIRECT 队列 barrier 转出）
            auto vb = gpuMgr.CreateBuffer(device, vtxSize, L"ProcGeo_VB_Default", D3D12_HEAP_TYPE_DEFAULT,
                                          D3D12_RESOURCE_STATE_COMMON);
            if (!vb.IsValid())
                return nullptr;
            auto ib = gpuMgr.CreateBuffer(device, idxSize, L"ProcGeo_IB_Default", D3D12_HEAP_TYPE_DEFAULT,
                                          D3D12_RESOURCE_STATE_COMMON);
            if (!ib.IsValid()) {
                gpuMgr.Release(vb, 0);
                return nullptr;
            }

            // UPLOAD 中转
            auto upVB = gpuMgr.CreateBuffer(device, vtxSize, L"ProcGeo_VB_Up", D3D12_HEAP_TYPE_UPLOAD,
                                            D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!upVB.IsValid()) {
                gpuMgr.Release(vb, 0);
                gpuMgr.Release(ib, 0);
                return nullptr;
            }
            auto upIB = gpuMgr.CreateBuffer(device, idxSize, L"ProcGeo_IB_Up", D3D12_HEAP_TYPE_UPLOAD,
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

            // COPY 队列
            uint64_t copyCompleted = cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_COPY);
            auto cpAllocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(copyCompleted);
            auto *cpAlloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(cpAllocH);
            auto cpCmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_COPY>(cpAlloc);
            auto cpCmd = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_COPY>(cpCmdH);
            cpCmd.Get()->CopyResource(gpuMgr.GetResource(vb), upVBRes);
            cpCmd.Get()->CopyResource(gpuMgr.GetResource(ib), upIBRes);
            cpCmd.Close();

            // DIRECT 队列：COPY_DEST → VB/IB
            uint64_t directCompleted = cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
            auto drAllocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(directCompleted);
            auto *drAlloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(drAllocH);
            auto drCmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(drAlloc);
            auto drCmd = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(drCmdH);
            D3D12_RESOURCE_BARRIER barriers[2] = {
                CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(vb), D3D12_RESOURCE_STATE_COMMON,
                                                     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
                CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(ib), D3D12_RESOURCE_STATE_COMMON,
                                                     D3D12_RESOURCE_STATE_INDEX_BUFFER)};
            drCmd.Get()->ResourceBarrier(2, barriers);
            drCmd.Close();

            auto item = std::make_shared<GpuWorkItem>();
            item->uploadBufferHandles.push_back(upVB);
            item->uploadBufferHandles.push_back(upIB);

            // 填写几何（按 type 分支：grid → GridGeometry，其余 → TriangleMesh）
            if (state->type == "grid") {
                Resource::GridGeometry grid;
                grid.vertexBufferHandle = vb;
                grid.indexBufferHandle = ib;
                grid.vertexCount = state->vertexCount;
                grid.indexCount = state->indexCount;
                grid.vertexStride = state->vertexStride;
                grid.indexFormat = state->indexFormat;
                grid.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                grid.widthSegments = state->widthSegments;
                grid.depthSegments = state->depthSegments;
                grid.isGpuReady = true;

                // 程序化水面网格：XZ 平面，中心在原点（y=0）
                Math::BoundingAABB aabb;
                aabb.min = DirectX::XMFLOAT3(-params.width * 0.5f, 0.0f, -params.depth * 0.5f);
                aabb.max = DirectX::XMFLOAT3(params.width * 0.5f, 0.0f, params.depth * 0.5f);
                grid.localBounds = aabb;

                // 子网格表：1 条子网格覆盖整个索引区间（材质槽驱动）
                Resource::SubMeshInfo smi;
                smi.startIndex = 0;
                smi.indexCount = grid.indexCount;
                smi.startVertex = 0;
                grid.subMeshes.push_back(smi);

                *geometryOut = std::move(grid);
            } else {
                Resource::TriangleMesh triMesh;
                triMesh.vertexBufferHandle = vb;
                triMesh.indexBufferHandle = ib;
                triMesh.vertexCount = state->vertexCount;
                triMesh.indexCount = state->indexCount;
                triMesh.vertexStride = state->vertexStride;
                triMesh.indexFormat = state->indexFormat;
                triMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                triMesh.isGpuReady = true;

                // 子网格表：1 条子网格覆盖整个索引区间（材质槽驱动）
                Resource::SubMeshInfo smi;
                smi.startIndex = 0;
                smi.indexCount = triMesh.indexCount;
                smi.startVertex = 0;
                triMesh.subMeshes.push_back(smi);

                *geometryOut = std::move(triMesh);
            }

            item->copyCmdListHandle = cpCmdH;
            item->copyAllocatorHandle = cpAllocH;
            item->directCmdListHandle = drCmdH;
            item->directAllocatorHandle = drAllocH;
            item->ready.store(true, std::memory_order_release);
            state->gpuReady = true; // 几何已填写，onComplete 可注册
            return item;
        };

        // ── Step 3: GPU 完成（主线程）→ 注册到 GeometryResourceManager ──
        task.onComplete = [geometryOut, state, geoMgr, result](bool success) {
            // gpuReady 兜底：gpuWork 失败（返回 nullptr）时 BackgroundExecutor 仍以 success=true
            // 回调（DeferToMainThread），必须依赖几何填写标志拦截，避免注册空几何
            if (success && state->gpuReady && geoMgr) {
                result->geometryHandle = geoMgr->RegisterGeometry(*geometryOut);
                result->success = result->geometryHandle.IsValid();
            }
        };

        return task;
    }
};

} // namespace DX12Engine::Async
