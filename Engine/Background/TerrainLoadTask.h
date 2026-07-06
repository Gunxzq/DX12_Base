#pragma once

#include "BackgroundExecutor.h"
#include "Core/SharedDataStore/SharedDataStore.h"
#include "Event/EventRegistry.h"
#include "Event/EventTypes.h"
#include "Event/MessageDispatcher.h"
#include "Logger/Logger.h"
#include "Math/BoundingVolume.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/RHI/Command/Utils/GpuUpload.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/AssetLoader/AssetLoader.h"
#include "Resource/AssetLoader/Loader/TerrainLoader.h"
#include "Resource/AssetManager/ResourceType.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Core/GpuHandlePool.h"
#include "Resource/GpuResourceManager.h"
#include "Scheduler/Task.h"
#include <DirectXMath.h>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>

namespace DX12Engine::Async {

// 使用 EventRegistry 中定义的枚举值作为事件哈希
constexpr uint32_t RESOURCE_LOAD_FAILED_EVENT_HASH = static_cast<uint32_t>(Event::EventType::ResourceLoadFailedEvent);

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
//   回调 (主线程): 发送 ResourceReadyEvent → TerrainGPUCreateSystem 响应
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

    // ── 法线贴图（N_heightmap.dds, 1280x1280）──
    std::atomic<bool> normalCreated{false};
    Resource::GpuResourceHandle normalMapGpuHandle = Resource::GpuResourceHandle::Invalid();
    D3D12_RESOURCE_DESC normalMapDesc = {};

    // ── 包围盒（CPU 计算，直接写入）──
    Math::BoundingAABB bounds;
};

using TerrainReadyStatePtr = std::shared_ptr<TerrainReadyState>;

// ============================================================================
// TerrainGPUResult — 跨线程传递的地形 GPU 资源数据（POD，用于 AssetDataManager）
//
// 由 gpuWork 在后台线程填充，通过 AssetDataManager::RegisterData 存储，
// 主线程 System 通过事件 payload 中的 CpuResourceHandle 反查读取。
// ============================================================================
struct TerrainGPUResult {
    Resource::GpuResourceHandle vbHandle = Resource::GpuResourceHandle::Invalid();
    Resource::GpuResourceHandle ibHandle = Resource::GpuResourceHandle::Invalid();
    Resource::GpuResourceHandle heightMapGpuHandle = Resource::GpuResourceHandle::Invalid();
    Resource::GpuResourceHandle albedoGpuHandle = Resource::GpuResourceHandle::Invalid();
    Resource::GpuResourceHandle normalMapGpuHandle = Resource::GpuResourceHandle::Invalid();
    D3D12_RESOURCE_DESC heightMapDesc = {};
    D3D12_RESOURCE_DESC albedoDesc = {};
    D3D12_RESOURCE_DESC normalMapDesc = {};
    Math::BoundingAABB bounds;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    float maxHeight = 0.0f;
};

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
    };

    /**
     * @brief 创建地形加载 LoadTask（三段式模型）
     *
     * 与 BackgroundExecutor::SubmitLoadTask 配合使用：
     *   cpuWork  (后台线程): CPU 加载高度图 → 网格数据
     *   gpuWork  (后台线程): 创建 VB/IB → 创建纹理 → 录制 COPY+DIRECT 命令 → 返回 GpuWorkItem
     *   onComplete (主线程): GPU 上传完成 → PostEvent(TerrainLoaded / TerrainLoadFailed)
     *
     * @param requestId 请求ID
     * @param heightmapPath 灰度图地形路径
     * @param width 地形宽度
     * @param depth 地形深度
     * @param maxHeight 最大高度
     * @param segments 地形分割段数
     * @param outData 输出数据
     * @param input 输入参数
     * @return LoadTask
     * @date 2026-07-05
     */
    static LoadTask CreateLoadTask(uint32_t requestId, const std::wstring &heightmapPath, float width, float depth,
                                   float maxHeight, uint32_t segments, TerrainLoadDataPtr &outData,
                                   const Input &input) {
        LoadTask task;
        task.name = "TerrainLoadTask";

        // 三段式共享状态
        struct SharedState {
            TerrainLoadDataPtr loadData;
            uint32_t requestId;
            std::atomic<bool> failed{false};
            Core::DataSlotHandle resultHandle = Core::DataSlotHandle::Invalid();
            Math::BoundingAABB bounds;
        };
        auto state = std::make_shared<SharedState>();
        state->requestId = requestId;
        state->loadData = std::make_shared<TerrainLoadData>();
        outData = state->loadData;

        // ── Step 1: CPU 工作（后台线程，纯 CPU 计算） ──
        task.cpuWork = [state, heightmapPath, width, depth, maxHeight, segments]() {
            auto *logger = Logger::Logger::GetInstance();
            logger->Info("[TerrainLoadTask] cpuWork started (request={})", state->requestId);

            try {
                Resource::TerrainMeshData meshData;
                if (!Resource::AssetLoader::GetInstance().LoadTerrainFromFile(heightmapPath, width, depth, maxHeight,
                                                                              segments, meshData)) {
                    logger->Error("[TerrainLoadTask] cpuWork: failed to load heightmap (request={})", state->requestId);
                    state->failed.store(true, std::memory_order_release);
                    return;
                }

                logger->Info("[TerrainLoadTask] cpuWork: heightmap loaded, {} vertices, {} indices (request={})",
                             meshData.vertices.size(), meshData.indices.size(), state->requestId);

                state->loadData->vertices = std::move(meshData.vertices);
                state->loadData->indices = std::move(meshData.indices);
                state->loadData->width = meshData.width;
                state->loadData->depth = meshData.depth;
                state->loadData->maxHeight = meshData.maxHeight;
                state->loadData->widthSegments = meshData.widthSegments;
                state->loadData->heightSegments = meshData.heightSegments;

                state->bounds.min = DirectX::XMFLOAT3(-meshData.width * 0.5f, 0.0f, -meshData.depth * 0.5f);
                state->bounds.max = DirectX::XMFLOAT3(meshData.width * 0.5f, meshData.maxHeight, meshData.depth * 0.5f);
                state->loadData->bounds = state->bounds;
            } catch (const std::exception &e) {
                logger->Error("[TerrainLoadTask] cpuWork exception: {} (request={})", e.what(), state->requestId);
                state->failed.store(true, std::memory_order_release);
            } catch (...) {
                logger->Error("[TerrainLoadTask] cpuWork unknown exception (request={})", state->requestId);
                state->failed.store(true, std::memory_order_release);
            }
        };

        // ── Step 2: GPU 工作（后台线程，创建 GPU 资源 + 录制命令） ──
        task.gpuWork = [state, device = input.device, cmdMgr = input.cmdMgr]() -> GpuWorkItemPtr {
            if (state->failed.load(std::memory_order_acquire)) {
                return nullptr;
            }

            auto *logger = Logger::Logger::GetInstance();
            logger->Info("[TerrainLoadTask] gpuWork started (request={})", state->requestId);

            try {
                // 创建临时 readyState（后台线程局部，CPU 加载时已计算 bounds）
                auto readyState = std::make_shared<TerrainReadyState>();
                readyState->bounds = state->bounds;

                // 创建 VB/IB（UPLOAD 堆，Map/Unmap）
                if (!UploadGeometryToGPU(device, state->loadData->vertices.data(), state->loadData->vertices.size(),
                                         sizeof(GeometryGenerator::Vertex), state->loadData->indices.data(),
                                         static_cast<uint32_t>(state->loadData->indices.size()), readyState->vbHandle,
                                         readyState->ibHandle)) {
                    logger->Error("[TerrainLoadTask] Failed to create VB/IB!");
                    state->failed.store(true, std::memory_order_release);
                    return nullptr;
                }
                readyState->geometryCreated.store(true, std::memory_order_release);
                logger->Info("[TerrainLoadTask] VB/IB created: {} verts, {} indices", state->loadData->vertices.size(),
                             state->loadData->indices.size());

                // 加载 3 张纹理（不录制命令，只创建 GPU 资源）
                auto heightTex =
                    LoadTextureFromFileAndCreateGpuResource(L"Content/Textures/H_Runtime_heightmap.dds", device);
                logger->Info("[TerrainLoadTask] Height texture GPU resource created: ok={}", heightTex.ok);
                if (heightTex.ok) {
                    readyState->heightMapGpuHandle = heightTex.gpuHandle;
                    readyState->heightMapDesc = heightTex.desc;
                }
                readyState->heightMapCreated.store(true, std::memory_order_release);

                auto albedoTex = LoadTextureFromFileAndCreateGpuResource(L"Content/Textures/D_heightmap.dds", device);
                logger->Info("[TerrainLoadTask] Albedo texture GPU resource created: ok={}", albedoTex.ok);
                if (albedoTex.ok) {
                    readyState->albedoGpuHandle = albedoTex.gpuHandle;
                    readyState->albedoDesc = albedoTex.desc;
                }
                readyState->albedoCreated.store(true, std::memory_order_release);

                auto normalTex = LoadTextureFromFileAndCreateGpuResource(L"Content/Textures/N_heightmap.dds", device);
                logger->Info("[TerrainLoadTask] Normal texture GPU resource created: ok={}", normalTex.ok);
                if (normalTex.ok) {
                    readyState->normalMapGpuHandle = normalTex.gpuHandle;
                    readyState->normalMapDesc = normalTex.desc;
                }
                readyState->normalCreated.store(true, std::memory_order_release);

                // 计算纹理上传尺寸 + 准备 TextureUploadRecord 数组
                constexpr UINT64 TEX_ALIGN = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
                auto AlignUp = [](UINT64 s, UINT64 a) -> UINT64 { return (s + a - 1) & ~(a - 1); };

                TextureLoadResult texArray[] = {heightTex, albedoTex, normalTex};
                constexpr uint32_t TEX_COUNT = 3;

                UINT64 totalUploadSize = 0;
                for (uint32_t ti = 0; ti < TEX_COUNT; ++ti) {
                    if (!texArray[ti].ok || !texArray[ti].texResource)
                        continue;
                    totalUploadSize += AlignUp(
                        GetRequiredIntermediateSize(texArray[ti].texResource, 0,
                                                    static_cast<UINT>(texArray[ti].ddsInfo.subresources.size())),
                        TEX_ALIGN);
                }

                if (totalUploadSize == 0) {
                    logger->Error("[TerrainLoadTask] No textures to upload!");
                    state->failed.store(true, std::memory_order_release);
                    return nullptr;
                }

                // 创建合并 upload buffer
                auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
                Resource::GpuResourceHandle uploadHandle =
                    gpuMgr.CreateBuffer(device, totalUploadSize, L"BatchTexUpload", D3D12_HEAP_TYPE_UPLOAD,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
                ID3D12Resource *uploadResource = gpuMgr.GetResource(uploadHandle);
                if (!uploadResource) {
                    logger->Error("[TerrainLoadTask] Failed to create upload buffer!");
                    state->failed.store(true, std::memory_order_release);
                    return nullptr;
                }

                // 准备录制参数
                TextureUploadRecord records[TEX_COUNT];
                UINT64 offset = 0;
                for (uint32_t ti = 0; ti < TEX_COUNT; ++ti) {
                    auto &rec = records[ti];
                    const auto &tex = texArray[ti];
                    if (tex.ok && tex.texResource) {
                        rec.texResource = tex.texResource;
                        rec.ddsInfo = &tex.ddsInfo;
                        rec.uploadOffset = offset;
                        offset += AlignUp(GetRequiredIntermediateSize(
                                              tex.texResource, 0, static_cast<UINT>(tex.ddsInfo.subresources.size())),
                                          TEX_ALIGN);
                    }
                }

                // 获取 COPY + DIRECT 命令列表
                auto item = std::make_shared<GpuWorkItem>();
                item->uploadBufferHandle = uploadHandle;

                uint64_t fence = cmdMgr->GetNextSequence();

                // COPY 队列
                {
                    auto allocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(fence);
                    auto *alloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(allocH);
                    auto cmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_COPY>(alloc);
                    auto cmd = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_COPY>(cmdH);
                    // Pool 已内部 Reset，直接录制
                    item->copyCmdListHandle = cmdH;
                    item->copyAllocatorHandle = allocH;

                    // 只录制 COPY 命令
                    D3D12_RESOURCE_BARRIER dirBarriers[TEX_COUNT];
                    RecordTextureUploadBatch(cmd.Get(), nullptr, uploadResource, records, TEX_COUNT, dirBarriers,
                                             TEX_COUNT);

                    cmd.Close();
                }

                // DIRECT 队列
                {
                    auto allocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
                    auto *alloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
                    auto cmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
                    auto cmd = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
                    // Pool 已内部 Reset，直接录制
                    item->directCmdListHandle = cmdH;
                    item->directAllocatorHandle = allocH;

                    // 只录制 COMMON → SRV 状态转换
                    D3D12_RESOURCE_BARRIER dirBarriers[TEX_COUNT];
                    RecordTextureUploadBatch(nullptr, cmd.Get(), uploadResource, records, TEX_COUNT, dirBarriers,
                                             TEX_COUNT);

                    cmd.Close();
                }

                item->ready.store(true, std::memory_order_release);
                logger->Info("[TerrainLoadTask] Texture batch upload recorded: {} textures, {} bytes", TEX_COUNT,
                             totalUploadSize);

                // ── 存储结果到 AssetDataManager ──
                TerrainGPUResult result;
                result.vbHandle = readyState->vbHandle;
                result.ibHandle = readyState->ibHandle;
                result.heightMapGpuHandle = readyState->heightMapGpuHandle;
                result.heightMapDesc = readyState->heightMapDesc;
                result.albedoGpuHandle = readyState->albedoGpuHandle;
                result.albedoDesc = readyState->albedoDesc;
                result.normalMapGpuHandle = readyState->normalMapGpuHandle;
                result.normalMapDesc = readyState->normalMapDesc;
                result.bounds = state->bounds;
                result.indexCount = static_cast<uint32_t>(state->loadData->indices.size());
                result.vertexCount = static_cast<uint32_t>(state->loadData->vertices.size());
                result.maxHeight = state->loadData->maxHeight;

                auto &assetMgr = Core::SharedDataStore::GetInstance();
                state->resultHandle = assetMgr.AllocateSlot(Core::DataSlotType::Mesh, 0);
                if (state->resultHandle.IsValid()) {
                    assetMgr.StoreData(state->resultHandle, &result, sizeof(TerrainGPUResult));
                    logger->Info(
                        "[TerrainLoadTask] TerrainGPUResult stored in AssetDataManager (handle={}, request={})",
                        static_cast<uint32_t>(state->resultHandle), state->requestId);
                } else {
                    logger->Error("[TerrainLoadTask] Failed to allocate AssetDataManager slot (request={})",
                                  state->requestId);
                    state->failed.store(true, std::memory_order_release);
                    return nullptr;
                }
                return item;
            } catch (const std::exception &e) {
                logger->Error("[TerrainLoadTask] gpuWork exception: {} (request={})", e.what(), state->requestId);
                state->failed.store(true, std::memory_order_release);
                return nullptr;
            } catch (...) {
                logger->Error("[TerrainLoadTask] gpuWork unknown exception (request={})", state->requestId);
                state->failed.store(true, std::memory_order_release);
                return nullptr;
            }
        };

        // ── Step 3: GPU 上传完成回调（主线程） ──
        task.onComplete = [state](bool success) {
            if (success && !state->failed.load(std::memory_order_acquire)) {
                // Payload: 低位 32 bits = DataSlotHandle, 高位 32 bits = 资源类型标识
                uint64_t payload =
                    (static_cast<uint64_t>(Resource::ResourceType::Terrain) << 32) | static_cast<uint32_t>(state->resultHandle);
                Event::MessageDispatcher::GetInstance()->PostEvent(
                    static_cast<uint32_t>(Event::EventType::ResourceReadyEvent), 0, payload,
                    Event::EventPriority::P4_Background);
                auto *logger = Logger::Logger::GetInstance();
                logger->Info("[TerrainLoadTask] PostEvent ResourceReady: type={} handle={} (request={})",
                             static_cast<uint32_t>(Resource::ResourceType::Terrain), static_cast<uint32_t>(state->resultHandle), state->requestId);
            } else {
                uint64_t payload =
                    (static_cast<uint64_t>(Resource::ResourceType::Terrain) << 32) | static_cast<uint64_t>(state->requestId) << 0;
                Event::MessageDispatcher::GetInstance()->PostEvent(RESOURCE_LOAD_FAILED_EVENT_HASH, 0, payload,
                                                                   Event::EventPriority::P4_Background);
            }
        };

        return task;
    }
};

} // namespace DX12Engine::Async
