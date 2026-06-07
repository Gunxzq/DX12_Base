#pragma once

#include "BackgroundExecutor.h"
#include "Event/EventRegistry.h"
#include "Event/EventTypes.h"
#include "Event/MessageDispatcher.h"
#include "Logger/Logger.h"
#include "Math/BoundingVolume.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/AssetDataManager.h"
#include "Resource/AssetLoader/AssetLoader.h"
#include "Resource/AssetLoader/Loader/TerrainLoader.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Struct/ResourceHandle.h"
#include "Scheduler/Task.h"
#include <DirectXMath.h>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>

namespace DX12Engine::Async {

// 使用 EventRegistry 中定义的枚举值作为事件哈希
constexpr uint32_t TERRAIN_LOADED_EVENT_HASH = static_cast<uint32_t>(Event::EventType::TerrainLoadedEvent);
constexpr uint32_t TERRAIN_LOAD_FAILED_EVENT_HASH = static_cast<uint32_t>(Event::EventType::TerrainLoadFailedEvent);

// ============================================================================
// 地形加载数据（CPU 端），使用 shared_ptr 在线程间共享
// ============================================================================
struct TerrainLoadData {
    std::vector<GeometryGenerator::Vertex> vertices;
    std::vector<uint32_t> indices;
    float width;
    float depth;
    float maxHeight;
    uint32_t widthSegments = 0;
    uint32_t heightSegments = 0;
    Math::BoundingAABB bounds;
};

using TerrainLoadDataPtr = std::shared_ptr<TerrainLoadData>;

// ============================================================================
// TerrainReadyState — 后台线程创建 GPU 资源后，将注册句柄所需数据写入此结构
//
// 数据流（新架构）：
//   后台线程: CPU加载 → 创建VB/IB → 创建纹理 → 录制COPY+DIRECT命令
//            → 构造 GpuWorkItem → BackgroundExecutor::RegisterGpuWork()
//            → 写入 TerrainReadyState (vbHandle, ibHandle, textureGpuHandle)
//            → 后台线程任务结束
//
//   BackgroundExecutor::Tick() (主线程):
//            → 收集 GpuWorkItem → Submit COPY → Signal → Submit DIRECT → Wait → 回调
//
//   回调 (主线程): 发送 TerrainLoadedEvent → TerrainGPUCreateSystem 响应
//
// 关键设计：BackgroundExecutor 承担类似帧驱动器的角色，统一提交 GPU 命令
// ============================================================================
struct TerrainReadyState {
    // ── 几何体：后台线程创建 VB/IB 后写入 ──
    std::atomic<bool> geometryCreated{false};
    Resource::GpuResourceHandle vbHandle = Resource::GpuResourceHandle::Invalid();
    Resource::GpuResourceHandle ibHandle = Resource::GpuResourceHandle::Invalid();

    // ── 高度图纹理（H_Runtime_heightmap.dds, 512x512）──
    std::atomic<bool> heightMapCreated{false};
    Resource::GpuResourceHandle heightMapGpuHandle = Resource::GpuResourceHandle::Invalid();
    D3D12_RESOURCE_DESC heightMapDesc = {};

    // ── 漫反射纹理（D_heightmap.dds, 1280x1280）──
    std::atomic<bool> albedoCreated{false};
    Resource::GpuResourceHandle albedoGpuHandle = Resource::GpuResourceHandle::Invalid();
    D3D12_RESOURCE_DESC albedoDesc = {};

    // ── 包围盒（CPU 计算，直接写入）──
    Math::BoundingAABB bounds;
};

using TerrainReadyStatePtr = std::shared_ptr<TerrainReadyState>;

/**
 * @brief 地形加载任务工厂 — 后台线程录制 GPU 命令，BackgroundExecutor 统一提交
 *
 * 类比 FrameDriver 架构：
 *   FrameDriver:           System录制命令 → SubmitRenderCommand → ExecuteRenderPhase
 *   BackgroundExecutor:    后台线程录制 → RegisterGpuWork → Tick中Submit → 回调
 *
 * 数据流：
 *   后台线程: CPU加载高度图 → 创建 VB/IB (UPLOAD堆 Map/Unmap)
 *            → 创建纹理(DEFAULT堆) → 录制 COPY 上传命令(Close, 不Submit)
 *            → 录制 Barrier 转换命令(Close, 不Submit)
 *            → 构造 GpuWorkItem → BackgroundExecutor::RegisterGpuWork()
 *            → 写入 TerrainReadyState (vbHandle, ibHandle, textureGpuHandle)
 *
 *   BackgroundExecutor::Tick() (主线程):
 *            → 收集 GpuWorkItem → Submit COPY → Signal COPY fence
 *            → Submit DIRECT (Wait COPY fence) → Signal DIRECT fence
 *            → Wait DIRECT fence → onComplete 回调
 *
 *   onComplete 回调 (主线程):
 *            → PostEvent(TerrainLoaded) → TerrainGPUCreateSystem 响应
 *
 * 线程安全说明：
 *   - GpuResourceManager (CreateBuffer/CreateTexture2D) 有 mutex 保护，线程安全
 *   - CommandAllocatorPool::Acquire 使用 CAS 无锁，线程安全
 *   - 后台线程只录制命令（Close），不 Submit，不与主线程渲染队列竞争
 *   - BackgroundExecutor::RegisterGpuWork 有 mutex 保护
 *   - GeometryResourceManager/TextureManager/DescriptorHeapCollection::Allocate = 非线程安全 → 主线程
 */
class TerrainLoadTaskFactory {
public:
    struct Input {
        ID3D12Device *device = nullptr;
        Renderer::CommandManager *cmdMgr = nullptr;
        Resource::DescriptorHeapCollection *descriptorHeaps = nullptr;
        TerrainReadyStatePtr readyState;
        BackgroundExecutor *backgroundExecutor = nullptr;
    };

    /**
     * @brief 创建地形加载任务
     * @param requestId 请求ID
     * @param heightmapPath 灰度图地形路径
     * @param width 地形宽度
     * @param depth 地形深度
     * @param maxHeight 最大高度
     * @param segments 地形分割段数
     * @param outData 输出数据
     * @param input 输入参数
     * @return Scheduler::Task
     * @date 2026-06-03
     */
    static Scheduler::Task Create(uint32_t requestId, const std::wstring &heightmapPath, float width, float depth,
                                  float maxHeight, uint32_t segments, TerrainLoadDataPtr &outData, const Input &input) {
        Scheduler::Task task;
        task.name = "TerrainLoadTask";
        task.phase = Scheduler::TaskPhase::Update;
        task.thread = Scheduler::ThreadType::Worker;
        task.priority = static_cast<uint32_t>(Scheduler::TaskPriority::Background);

        auto data = std::make_shared<TerrainLoadData>();
        outData = data;

        // 只是注册任务，没有执行任何 GPU 操作
        task.execute = [requestId, heightmapPath, width, depth, maxHeight, segments, data, device = input.device,
                        cmdMgr = input.cmdMgr, descriptorHeaps = input.descriptorHeaps, readyState = input.readyState,
                        bgExecutor = input.backgroundExecutor]() {
            // 任务代码，等task.execute()调用时才会执行
            ExecuteWorker(requestId, heightmapPath, width, depth, maxHeight, segments, data, device, cmdMgr,
                          descriptorHeaps, readyState, bgExecutor);
        };

        return task;
    }

private:
    static void ExecuteWorker(uint32_t requestId, const std::wstring &heightmapPath, float width, float depth,
                              float maxHeight, uint32_t segments, TerrainLoadDataPtr data, ID3D12Device *device,
                              Renderer::CommandManager *cmdMgr, Resource::DescriptorHeapCollection *descriptorHeaps,
                              TerrainReadyStatePtr readyState, BackgroundExecutor *bgExecutor) {
        auto *logger = Logger::Logger::GetInstance();
        logger->Info("[TerrainLoadTask] Worker thread started (request={})", requestId);

        try {
        // Step 1: CPU 端 — 加载高度图并生成网格数据
        Resource::TerrainMeshData meshData;
        if (!Resource::AssetLoader::GetInstance().LoadTerrainFromFile(heightmapPath, width, depth, maxHeight, segments,
                                                                      meshData)) {
            logger->Error("[TerrainLoadTask] Failed to load heightmap from file (request={})", requestId);
            PostLoadFailed(requestId);
            return;
        }

        logger->Info("[TerrainLoadTask] Heightmap loaded: {} vertices, {} indices (request={})",
                     meshData.vertices.size(), meshData.indices.size(), requestId);

        data->vertices = std::move(meshData.vertices);
        data->indices = std::move(meshData.indices);
        data->width = meshData.width;
        data->depth = meshData.depth;
        data->maxHeight = meshData.maxHeight;
        data->widthSegments = meshData.widthSegments;
        data->heightSegments = meshData.heightSegments;

        readyState->bounds.min = DirectX::XMFLOAT3(-meshData.width * 0.5f, 0.0f, -meshData.depth * 0.5f);
        readyState->bounds.max = DirectX::XMFLOAT3(meshData.width * 0.5f, meshData.maxHeight, meshData.depth * 0.5f);
        data->bounds = readyState->bounds;

        // Step 2: GPU 端 — 创建 VB/IB（UPLOAD 堆）
        if (!CreateVertexBuffers(requestId, device, data, readyState)) {
            return;
        }


        // Step 3: 创建纹理（高度图 + 漫反射）+ 录制 COPY/DIRECT 命令 + 注册到 BackgroundExecutor
        CreateTexturesAndUpload(requestId, device, cmdMgr, readyState, bgExecutor);
        } catch (const std::exception &e) {
            logger->Error("[TerrainLoadTask] Exception in ExecuteWorker: {} (request={})", e.what(), requestId);
            PostLoadFailed(requestId);
        } catch (...) {
            logger->Error("[TerrainLoadTask] Unknown exception in ExecuteWorker (request={})", requestId);
            PostLoadFailed(requestId);
        }
    }

    static void PostLoadFailed(uint32_t requestId) {
        uint64_t payload = static_cast<uint64_t>(requestId) << 32;
        Event::MessageDispatcher::GetInstance()->PostEvent(TERRAIN_LOAD_FAILED_EVENT_HASH, 0, payload,
                                                           Event::EventPriority::P4_Background);
    }

    static void PostLoaded(uint32_t requestId) {
        uint64_t payload = static_cast<uint64_t>(requestId) << 32;
        bool posted = Event::MessageDispatcher::GetInstance()->PostEvent(TERRAIN_LOADED_EVENT_HASH, 0, payload,
                                                                         Event::EventPriority::P4_Background);
        auto *logger = Logger::Logger::GetInstance();
        logger->Info("[TerrainLoadTask] PostEvent TerrainLoaded: posted={} (request={})", posted, requestId);
    }

    static bool CreateVertexBuffers(uint32_t requestId, ID3D12Device *device, const TerrainLoadDataPtr &data,
                                    TerrainReadyStatePtr &readyState) {
        auto *logger = Logger::Logger::GetInstance();
        auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

        size_t vbSize = data->vertices.size() * sizeof(GeometryGenerator::Vertex);
        auto vbHandle = gpuMgr.CreateBuffer(device, vbSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (auto *vbRes = gpuMgr.GetResource(vbHandle)) {
            void *mapped = nullptr;
            vbRes->Map(0, nullptr, &mapped);
            memcpy(mapped, data->vertices.data(), vbSize);
            vbRes->Unmap(0, nullptr);
            readyState->vbHandle = vbHandle;
            logger->Info("[TerrainLoadTask] VB created: handle={}, size={} bytes", vbHandle.index, vbSize);
        } else {
            logger->Error("[TerrainLoadTask] Failed to create VB!");
            PostLoadFailed(requestId);
            return false;
        }

        size_t ibSize = data->indices.size() * sizeof(uint32_t);
        auto ibHandle = gpuMgr.CreateBuffer(device, ibSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (auto *ibRes = gpuMgr.GetResource(ibHandle)) {
            void *mapped = nullptr;
            ibRes->Map(0, nullptr, &mapped);
            memcpy(mapped, data->indices.data(), ibSize);
            ibRes->Unmap(0, nullptr);
            readyState->ibHandle = ibHandle;
            logger->Info("[TerrainLoadTask] IB created: handle={}, size={} bytes", ibHandle.index, ibSize);
        } else {
            logger->Error("[TerrainLoadTask] Failed to create IB!");
            PostLoadFailed(requestId);
            return false;
        }

        readyState->geometryCreated.store(true, std::memory_order_release);
        return true;
    }

    // =========================================================================
    // 加载单张 DDS 纹理 → 创建 GPU 资源（不录制 COPY）
    // =========================================================================
    struct SingleTextureResult {
        Resource::GpuResourceHandle gpuHandle;
        D3D12_RESOURCE_DESC desc;
        ID3D12Resource *texResource = nullptr;
        Resource::DDSTextureInfo ddsInfo;
        bool ok = false;
    };

    static SingleTextureResult LoadSingleTextureGpu(const std::wstring &path, ID3D12Device *device) {
        SingleTextureResult result;
        auto *logger = Logger::Logger::GetInstance();
        auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

        if (!Resource::AssetLoader::GetInstance().LoadTextureFromFile(path, result.ddsInfo)) {
            logger->Error("[TerrainLoadTask] Failed to load texture: {}", std::string(path.begin(), path.end()));
            return result;
        }

        auto gpuHandle = gpuMgr.CreateTexture2D(device, result.ddsInfo.desc, D3D12_RESOURCE_STATE_COMMON);
        if (!gpuHandle.IsValid()) {
            logger->Error("[TerrainLoadTask] Failed to create GPU texture for: {}",
                          std::string(path.begin(), path.end()));
            return result;
        }

        result.gpuHandle = gpuHandle;
        result.desc = result.ddsInfo.desc;
        result.texResource = gpuMgr.GetResource(gpuHandle);
        result.ok = true;
        return result;
    }

    static void CreateTexturesAndUpload(uint32_t requestId, ID3D12Device *device, Renderer::CommandManager *cmdMgr,
                                        TerrainReadyStatePtr &readyState, BackgroundExecutor *bgExecutor) {
        auto *logger = Logger::Logger::GetInstance();
        auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

        logger->Info("[TerrainLoadTask] CreateTexturesAndUpload start (request={})", requestId);

        try {
        // ================================================================
        // Step 1: 创建 GPU 资源（高度图 + 漫反射）
        // ================================================================
        auto heightResult = LoadSingleTextureGpu(L"Content/Textures/H_Runtime_heightmap.dds", device);
        logger->Info("[TerrainLoadTask] Height texture GPU resource created: ok={}", heightResult.ok);
        if (heightResult.ok) {
            readyState->heightMapGpuHandle = heightResult.gpuHandle;
            readyState->heightMapDesc = heightResult.desc;
        }
        readyState->heightMapCreated.store(true, std::memory_order_release);

        auto albedoResult = LoadSingleTextureGpu(L"Content/Textures/D_heightmap.dds", device);
        logger->Info("[TerrainLoadTask] Albedo texture GPU resource created: ok={}", albedoResult.ok);
        if (albedoResult.ok) {
            readyState->albedoGpuHandle = albedoResult.gpuHandle;
            readyState->albedoDesc = albedoResult.desc;
        }
        readyState->albedoCreated.store(true, std::memory_order_release);

        if (!heightResult.ok && !albedoResult.ok) {
            logger->Error("[TerrainLoadTask] Both textures failed to load!");
            PostLoaded(requestId);
            return;
        }

        // ================================================================
        // Step 2: 创建 upload buffer（合并两纹理所需大小）
        // 注意：每个纹理的起始偏移必须对齐到 D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512)
        // ================================================================
        constexpr UINT64 TEXTURE_DATA_ALIGNMENT = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        auto AlignUp = [](UINT64 size, UINT64 alignment) -> UINT64 {
            return (size + alignment - 1) & ~(alignment - 1);
        };

        UINT64 totalUploadSize = 0;
        if (heightResult.ok) {
            UINT64 heightSize = GetRequiredIntermediateSize(heightResult.texResource, 0,
                                                            static_cast<UINT>(heightResult.ddsInfo.subresources.size()));
            totalUploadSize += AlignUp(heightSize, TEXTURE_DATA_ALIGNMENT);
        }
        if (albedoResult.ok) {
            UINT64 albedoSize = GetRequiredIntermediateSize(albedoResult.texResource, 0,
                                                            static_cast<UINT>(albedoResult.ddsInfo.subresources.size()));
            totalUploadSize += AlignUp(albedoSize, TEXTURE_DATA_ALIGNMENT);
        }

        logger->Info("[TerrainLoadTask] Creating upload buffer: size={} bytes", totalUploadSize);

        Resource::GpuResourceHandle uploadHandle =
            gpuMgr.CreateBuffer(device, totalUploadSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        ID3D12Resource *uploadResource = gpuMgr.GetResource(uploadHandle);


        // ================================================================
        // Step 3: 录制合并 COPY 命令列表（两纹理一起上传）
        // ================================================================
        Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_COPY>::Handle copyCmdHandle;
        Renderer::CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COPY>::Handle copyAllocHandle;
        UINT64 accumulatedOffset = 0;
        {
            uint64_t copyCompletedFence = cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_COPY);
            logger->Info("[TerrainLoadTask] Acquiring COPY allocator (fence={})", copyCompletedFence);
            copyAllocHandle = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(copyCompletedFence);
            logger->Info("[TerrainLoadTask] COPY allocator acquired: valid={}", copyAllocHandle.IsValid());
            auto *copyAlloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(copyAllocHandle);
            logger->Info("[TerrainLoadTask] Acquiring COPY command list");
            copyCmdHandle = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_COPY>(copyAlloc);
            logger->Info("[TerrainLoadTask] COPY command list acquired: valid={}", copyCmdHandle.IsValid());
            auto copyCmdList = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_COPY>(copyCmdHandle);

            auto uploadOne = [&](ID3D12Resource *texResource, const Resource::DDSTextureInfo &info,
                                 UINT64 &offset) {
                auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                    texResource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                copyCmdList.Get()->ResourceBarrier(1, &barrier);

                UpdateSubresources(copyCmdList.Get(), texResource, uploadResource, offset, 0,
                                   static_cast<UINT>(info.subresources.size()),
                                   const_cast<D3D12_SUBRESOURCE_DATA *>(info.subresources.data()));

                auto postBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
                    texResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                copyCmdList.Get()->ResourceBarrier(1, &postBarrier);

                offset += GetRequiredIntermediateSize(texResource, 0,
                                                      static_cast<UINT>(info.subresources.size()));
                // 下一个纹理的起始偏移必须对齐到 D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512)
                offset = AlignUp(offset, TEXTURE_DATA_ALIGNMENT);
            };

            if (heightResult.ok) {
                uploadOne(heightResult.texResource, heightResult.ddsInfo, accumulatedOffset);
            }
            if (albedoResult.ok) {
                uploadOne(albedoResult.texResource, albedoResult.ddsInfo, accumulatedOffset);
            }

            copyCmdList.Close();
            logger->Info("[TerrainLoadTask] Merged COPY recorded: {} textures, {} bytes upload",
                         (heightResult.ok ? 1 : 0) + (albedoResult.ok ? 1 : 0), totalUploadSize);
        }

        // ================================================================
        // Step 4: 录制 DIRECT 命令列表 — ResourceTransition
        // ================================================================
        logger->Info("[TerrainLoadTask] Step 4: Acquiring DIRECT command list");
        Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle directCmdHandle;
        Renderer::CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle directAllocHandle;
        {
            uint64_t directCompletedFence = cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
            directAllocHandle = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(directCompletedFence);
            auto *directAlloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(directAllocHandle);
            directCmdHandle = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(directAlloc);
            auto directCmdList = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(directCmdHandle);

            D3D12_RESOURCE_BARRIER barriers[2];
            uint32_t barrierCount = 0;

            if (heightResult.ok) {
                barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
                    heightResult.texResource, D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            if (albedoResult.ok) {
                barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
                    albedoResult.texResource, D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }

            if (barrierCount > 0) {
                directCmdList.Get()->ResourceBarrier(barrierCount, barriers);
            }

            directCmdList.Close();
            logger->Info("[TerrainLoadTask] DIRECT command list recorded for {} textures", barrierCount);
        }

        // ================================================================
        // Step 5: 注册到 BackgroundExecutor
        // ================================================================
        auto gpuWork = std::make_shared<GpuWorkItem>();
        gpuWork->copyCmdListHandle = copyCmdHandle;
        gpuWork->copyAllocatorHandle = copyAllocHandle;
        gpuWork->directCmdListHandle = directCmdHandle;
        gpuWork->directAllocatorHandle = directAllocHandle;
        gpuWork->uploadBufferHandle = uploadHandle;

        gpuWork->onComplete = [requestId, readyState](bool success) {
            if (success) {
                PostLoaded(requestId);
            } else {
                PostLoadFailed(requestId);
            }
        };

        gpuWork->ready.store(true, std::memory_order_release);

        if (bgExecutor) {
            bgExecutor->RegisterGpuWork(gpuWork);
            logger->Info("[TerrainLoadTask] GpuWorkItem registered to BackgroundExecutor (request={})", requestId);
        } else {
            logger->Error("[TerrainLoadTask] BackgroundExecutor is null, cannot register GpuWorkItem!");
            PostLoaded(requestId);
        }
        } catch (const std::exception &e) {
            logger->Error("[TerrainLoadTask] Exception in CreateTexturesAndUpload: {} (request={})", e.what(), requestId);
            PostLoadFailed(requestId);
        } catch (...) {
            logger->Error("[TerrainLoadTask] Unknown exception in CreateTexturesAndUpload (request={})", requestId);
            PostLoadFailed(requestId);
        }
    }
};

} // namespace DX12Engine::Async
