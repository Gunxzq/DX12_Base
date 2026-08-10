#include "SceneConstructor.h"
#include "Background/GeometryProceduralTask.h"
#include "Boot/GameContext.h"
#include "Common/d3dUtil.h"
#include "Core/SharedDataStore/SharedDataStore.h"
#include "ECS/Core/Components/Camera.h"
#include "ECS/Core/Components/Light.h"
#include "ECS/Core/Components/Misc.h" // StaticComponent（静态实体持久化）
#include "ECS/Core/Components/Name.h"
#include "ECS/Core/Components/ReflectionProbe.h"
#include "ECS/Core/Components/Relationship.h"
#include "ECS/Core/Components/Render.h"
#include "ECS/Core/Components/Tags.h"
#include "ECS/Core/Components/Transform.h"
#include "ECS/Core/Components/Water.h"
#include "ECS/Core/Registry.h"
#include "Event/EventRegistry.h"
#include "Event/MessageDispatcher.h"
#include "Logger/Logger.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/Core/RenderSlotCache.h"
#include "Renderer/Core/ShaderRoute.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "Renderer/Material/MaterialManager.h"
#include "Renderer/Material/MaterialResource.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/SkyboxManager.h"
#include "Renderer/Scene/WaterManager.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/AssetManager/AssetManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Texture/TextureManager.h"
#include <algorithm> // std::clamp/std::min/std::max（blockConfig 推导，阶段 0c）
#include <cmath>     // std::isfinite（worldConfig 推导防御，2026-08-10）
#include <cstring>
#include <filesystem>

namespace DX12Engine::Scene {

using namespace DX12Engine::Resource;
using namespace DX12Engine::Renderer; // ShaderType/ParseShaderType（材质路由，RendererDataDriven.md §4.1a）

void SceneConstructor::LoadScene(const SceneDescription &desc, Boot::GameContext *context, HeapTag heapTag,
                                 Callback onComplete, const std::string &sceneFilePath) {
    m_desc = desc;
    m_context = context;
    m_heapTag = heapTag;
    m_onComplete = std::move(onComplete);
    m_sceneFilePath = sceneFilePath;
    m_loading = true;

    auto *log = context ? context->Logging : nullptr;

    // 保存原始依赖信息（用于编辑器重新导出，此时路径还是相对路径）
    m_originalDependencies = desc.dependencies;
    m_originalMaterials = desc.materials;
    m_originalBaseURL = desc.baseURL;

    if (!context || !context->GeometryResourceManager) {
        auto *log = context ? context->Logging : nullptr;
        if (log)
            log->Error("[SceneConstructor] Invalid GameContext");
        if (m_onComplete)
            m_onComplete(false);
        m_loading = false;
        return;
    }

    // 收集所有依赖路径（解析为基于项目根的绝对路径）
    // 类型不再显式声明——由 AssetManager 按后缀推断（见 AssetLoaderImprovement.md）
    std::vector<std::string> assets;
    std::string root = context->GetProjectConfig().Root;
    std::string baseURL = desc.baseURL;

    auto resolve = [&root, &baseURL](const std::string &path) -> std::string {
        if (path.empty())
            return path;
        // procedural:// URI 不走文件路径解析
        if (path.find("procedural://") == 0)
            return path;
        if (std::filesystem::path(path).is_absolute())
            return std::filesystem::path(path).generic_string();
        if (!baseURL.empty())
            return (std::filesystem::path(root) / baseURL / path).generic_string();
        return (std::filesystem::path(root) / path).generic_string();
    };

    for (const auto &[key, path] : desc.dependencies.meshes) {
        assets.emplace_back(resolve(path));
    }
    // 材质不加入 batch——等纹理加载完成后在 OnDependenciesLoaded 中统一解析注册
    for (const auto &[key, path] : desc.dependencies.textures) {
        assets.emplace_back(resolve(path));
    }
    for (const auto &[key, path] : desc.dependencies.terrains) {
        assets.emplace_back(resolve(path));
    }

    // 收集 entities 中 mesh.geometry 的 procedural:// URI（不存于 dependencies.meshes）
    for (const auto &entity : desc.entities) {
        if (entity.mesh && !entity.mesh->geometry.empty() && entity.mesh->geometry.find("procedural://") == 0) {
            assets.emplace_back(entity.mesh->geometry);
            log->Info("[SceneConstructor] Collected procedural URI: {}", entity.mesh->geometry);
        }
    }

    // 存储解析后的路径映射（用于 OnDependenciesLoaded 中查缓存）
    // 原 SceneDescription 中的路径是相对路径，缓存中是绝对路径
    // 把 m_desc.dependencies 里的路径也更新为绝对路径以保持一致
    // 或者建立两份映射。这里直接更新 m_desc
    for (auto &[key, path] : m_desc.dependencies.meshes)
        path = resolve(path);
    for (auto &[key, path] : m_desc.dependencies.textures)
        path = resolve(path);
    for (auto &[key, path] : m_desc.dependencies.terrains)
        path = resolve(path);

    if (assets.empty()) {
        // 没有依赖，直接构造实体
        log->Info("[SceneConstructor] No assets to load, constructing directly");
        OnDependenciesLoaded();
        return;
    }

    log->Info("[SceneConstructor] Submitting batch: {} assets ({}meshes {}textures {}terrains)", assets.size(),
              desc.dependencies.meshes.size(), desc.dependencies.textures.size(), desc.dependencies.terrains.size());

    // 提交批量加载（SceneConstructor 由 GameWorld 持有，生命周期足够长）
    // 分发任务前同步 heapTag（编辑器多堆模式：纹理 SRV 必须落在渲染绑定的 EditorViewport 堆）
    AssetManager::GetInstance().SetHeapTag(m_heapTag);
    m_batch = AssetManager::GetInstance().LoadBatch(assets, nullptr, [this]() {
        auto *l = m_context ? m_context->Logging : nullptr;
        if (l)
            l->Info("[SceneConstructor] Batch onAllComplete fired -> OnDependenciesLoaded");
        OnDependenciesLoaded();
    });
    log->Info("[SceneConstructor] Batch submitted, ptr={}", static_cast<void *>(m_batch.get()));
}

void SceneConstructor::OnDependenciesLoaded() {
    if (!m_context) {
        if (m_onComplete)
            m_onComplete(false);
        m_loading = false;
        return;
    }

    auto *log = m_context->Logging;
    auto &cache = Resource::AssetManager::GetInstance().GetCache();

    // ================================================================
    // Step 1: 建立 textureKey → SRV 索引映射
    // ================================================================
    std::unordered_map<std::string, uint32_t> texSrvMap;
    for (const auto &[key, path] : m_desc.dependencies.textures) {
        auto it = cache.find(path);
        if (it != cache.end() && it->second.success) {
            uint32_t srvIdx = m_context->TextureMgr->GetSRVIndex(it->second.textureHandle);
            if (srvIdx != UINT32_MAX) {
                texSrvMap[key] = srvIdx;
            }
        } else {
            log->Warn("[SceneConstructor] Texture '{}' not loaded: {}", key, path);
        }
    }

    // ================================================================
    // Step 2: 建立 key → GeometryHandle 映射
    // ================================================================
    std::unordered_map<std::string, GeometryHandle> geoMap;
    for (const auto &[key, path] : m_desc.dependencies.meshes) {
        auto it = cache.find(path);
        if (it != cache.end() && it->second.success) {
            geoMap[key] = it->second.geometryHandle;
        } else {
            log->Warn("[SceneConstructor] Mesh '{}' not loaded: {}", key, path);
        }
    }

    // 收集 procedural:// URI 几何体（内联于实体 mesh.geometry，不存于 dependencies.meshes）
    for (const auto &entity : m_desc.entities) {
        if (entity.mesh && !entity.mesh->geometry.empty() && entity.mesh->geometry.find("procedural://") == 0) {
            auto it = cache.find(entity.mesh->geometry);
            if (it != cache.end() && it->second.success) {
                geoMap[entity.mesh->geometry] = it->second.geometryHandle;
                log->Info("[SceneConstructor] Procedural geometry loaded: {} -> handle(idx={})", entity.mesh->geometry,
                          it->second.geometryHandle.index);
            } else {
                log->Warn("[SceneConstructor] Procedural geometry '{}' not loaded", entity.mesh->geometry);
            }
        }
    }

    // ================================================================
    // Step 3: 内联材质 → 纹理 key → SRV 索引 → 注册材质
    // ================================================================
    std::unordered_map<std::string, MaterialHandle> matMap;

    auto lookupSrvByKey = [&](const std::string &texKey) -> uint32_t {
        if (texKey.empty())
            return UINT32_MAX;
        auto it = texSrvMap.find(texKey);
        if (it != texSrvMap.end())
            return it->second;
        return UINT32_MAX;
    };

    for (const auto &[key, matDesc] : m_desc.materials) {
        // 从内联材质定义构造 MaterialData
        Resource::MaterialData data;
        data.name = key;
        data.materialId = TYPE_HASH(key);

        // params
        memcpy(&data.baseColor, matDesc.params.baseColor, sizeof(float) * 4);
        data.metallic = matDesc.params.metallic;
        data.roughness = matDesc.params.roughness;
        data.ambient = matDesc.params.ao;
        memcpy(&data.emissive, matDesc.params.emissive, sizeof(float) * 4);
        data.alphaCutoff = matDesc.params.alphaCutoff;

        // 纹理 key → SRV 索引
        data.baseColorTextureId = lookupSrvByKey(matDesc.textures.baseColor);
        data.normalTextureId = lookupSrvByKey(matDesc.textures.normal);
        data.metallicRoughnessTextureId = lookupSrvByKey(matDesc.textures.metallicRoughness);
        data.occlusionTextureId = lookupSrvByKey(matDesc.textures.ao);
        data.emissiveTextureId = lookupSrvByKey(matDesc.textures.emissive);

        // 调试日志
        // char debugBuf[512];
        // sprintf_s(debugBuf,
        //     "[SceneConstructor] Material '%s' pre-register: id=0x%08x "
        //     "color(%.2f,%.2f,%.2f,%.2f) metallic=%.2f rough=%.2f ao=%.2f "
        //     "texSRV(base=%u norm=%u mr=%u occ=%u emis=%u)\n",
        //     key.c_str(),
        //     static_cast<uint32_t>(data.materialId),
        //     data.baseColor.x, data.baseColor.y, data.baseColor.z, data.baseColor.w,
        //     data.metallic, data.roughness, data.ambient,
        //     data.baseColorTextureId, data.normalTextureId,
        //     data.metallicRoughnessTextureId, data.occlusionTextureId,
        //     data.emissiveTextureId);
        // OutputDebugStringA(debugBuf);

        MaterialHandle matHandle = m_context->MaterialMgr->RegisterMaterial(data);
        if (matHandle.IsValid()) {
            matMap[key] = matHandle;
            log->Info("[SceneConstructor][Diag] Material '{}' registered: handleIndex={} materialId=0x{:08x}", key,
                      matHandle.index, static_cast<uint32_t>(data.materialId));
        } else {
            log->Warn("[SceneConstructor] Material '{}' RegisterMaterial failed", key);
        }
    }

    // ================================================================
    // Step 4: 建立 textureKey → 原始 GPU 资源映射（供场景全局资源使用）
    // ================================================================
    std::unordered_map<std::string, GpuResourceHandle> texResourceMap;
    for (const auto &[key, path] : m_desc.dependencies.textures) {
        auto it = cache.find(path);
        if (it != cache.end() && it->second.success && it->second.gpuHandle.IsValid()) {
            texResourceMap[key] = it->second.gpuHandle;
        }
    }

    // ================================================================
    // Step 5: 场景全局天空盒（通过 SkyboxManager 直接设置，不走 ECS）
    // ================================================================
    if (m_desc.sceneEnvironment.skybox) {
        const auto &sb = *m_desc.sceneEnvironment.skybox;
        GpuResourceHandle texRes;
        auto texIt = texResourceMap.find(sb.texture);
        if (texIt != texResourceMap.end() && texIt->second.IsValid())
            texRes = texIt->second;
        else
            log->Error("[SceneConstructor] Skybox texture '{}' not found", sb.texture);

        if (sb.procedural.type.empty()) {
            log->Warn("[SceneConstructor] Skybox geometry type not specified, procedural default will be used");
            // 降级使用程序化立方体
        }

        // 程序化生成天空盒几何体：通过异步任务管线加载
        auto procResult = std::make_shared<Async::GeometryProceduralOutput>();
        auto task = Async::GeometryProceduralTask::Create(sb.procedural.type.empty() ? "cube" : sb.procedural.type,
                                                          sb.procedural, m_context->DeviceContext->GetDevice(),
                                                          &m_context->DeviceContext->GetCommandManager(),
                                                          m_context->GeometryResourceManager, procResult);

        // 注册完成回调：拿到 GeometryHandle 后设置天空盒
        auto prevOnComplete = task.onComplete;
        task.onComplete = [this, prevOnComplete, texRes, sb, log, procResult](bool success) {
            if (prevOnComplete)
                prevOnComplete(success);

            if (success && procResult->success) {
                GeometryHandle skyGeo = procResult->geometryHandle;
                m_skyboxProceduralHandles.push_back(skyGeo);

                if (texRes.IsValid() && skyGeo.IsValid()) {
                    Renderer::SkyboxManager::GetInstance().SetSkybox(texRes, skyGeo);
                    log->Info("[SceneConstructor] Skybox set via SkyboxManager: tex='{}' geo=procedural({})",
                              sb.texture, sb.procedural.type.empty() ? "cube" : sb.procedural.type);

                    auto cubeSRV = Renderer::SkyboxManager::GetInstance().GetCubeSRV();
                    if (cubeSRV.ptr != 0) {
                        Renderer::WaterManager::GetInstance().SetEnvironmentMap(cubeSRV);
                        log->Info("[SceneConstructor] Environment map injected into WaterManager");
                    }
                } else {
                    log->Warn("[SceneConstructor] Skybox incomplete: texValid={} geoValid={}", texRes.IsValid(),
                              skyGeo.IsValid());
                }
            }
        };

        // 提交到后台执行器（与现有异步加载管线一致）
        if (m_context && m_context->BackgroundExecutor) {
            m_context->BackgroundExecutor->SubmitLoadTask(std::move(task));
            log->Info("[SceneConstructor] Submitted procedural geometry task for skybox: type='{}'",
                      sb.procedural.type.empty() ? "cube" : sb.procedural.type);
        } else {
            log->Warn("[SceneConstructor] No BackgroundExecutor available, procedural skybox deferred");
        }
    }

    // ================================================================
    // Step 6: 场景数据存入 SharedDataStore + 材质 buffer 后台上传
    // ================================================================

    // 组装场景数据
    auto sceneData = std::make_shared<SceneConstructData>();
    sceneData->sceneName = m_desc.metadata.name;
    sceneData->sceneFilePath = m_sceneFilePath;
    sceneData->entities = m_desc.entities;
    sceneData->geoMap = std::move(geoMap);
    sceneData->matMap = std::move(matMap);

    // 携带场景环境数据（供 SceneConstructSystem 设置到 SceneManager）
    sceneData->skybox = m_desc.sceneEnvironment.skybox;
    sceneData->environment = m_desc.sceneEnvironment.ambient;
    // 完整场景环境（静态实体烘焙 precomputed / entityMotionPolicy，ConstructEntity 消费）
    sceneData->sceneEnvironment = m_desc.sceneEnvironment;
    sceneData->waterBlocks = m_desc.waterBlocks; // 水块（邻接 Sea 合并——程序化水面四边形）

    // 块划分配置（UE World Partition 模式，GPU-Drive.md §4.1）：
    // JSON 显式配置（blockConfig.cellSize>0）直接用；缺失则按实体世界范围推导
    // cellSize = clamp(mapExtent / blocksPerAxis)，blocksPerAxis 默认 4，上下限 100~1000
    {
        if (m_desc.blockConfig && m_desc.blockConfig->IsConfigured()) {
            sceneData->blockConfig = m_desc.blockConfig;
        } else {
            const int bpa =
                (m_desc.blockConfig && m_desc.blockConfig->blocksPerAxis > 0) ? m_desc.blockConfig->blocksPerAxis : 4;
            const float minCell = (m_desc.blockConfig && m_desc.blockConfig->minCellSize > 0.0f)
                                      ? m_desc.blockConfig->minCellSize
                                      : 100.0f;
            const float maxCell = (m_desc.blockConfig && m_desc.blockConfig->maxCellSize > 0.0f)
                                      ? m_desc.blockConfig->maxCellSize
                                      : 1000.0f;

            float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
            for (const auto &e : m_desc.entities) {
                if (e.transform && e.transform->position.size() >= 3) {
                    minX = (std::min)(minX, e.transform->position[0]);
                    maxX = (std::max)(maxX, e.transform->position[0]);
                    minZ = (std::min)(minZ, e.transform->position[2]);
                    maxZ = (std::max)(maxZ, e.transform->position[2]);
                }
            }
            Resource::BlockConfigDesc bc;
            bc.blocksPerAxis = bpa;
            bc.minCellSize = minCell;
            bc.maxCellSize = maxCell;
            if (maxX > minX || maxZ > minZ) {
                const float mapExtent = (std::max)(maxX - minX, maxZ - minZ);
                bc.cellSize = (std::clamp)(mapExtent / static_cast<float>(bpa), minCell, maxCell);
            }
            sceneData->blockConfig = bc;
        }
    }

    // 空间索引世界范围（OctreeSystem，OctreeCullingAndRaycaster.md §7.5）：
    // JSON 显式配置（worldConfig.worldSize>0）直接用；缺失则按实体 worldBounds 全范围推导
    // worldSize = max(spanX,spanY,spanZ) * 1.2 + 包围盒中心（与 OctreeSystem::Build 阶段 1 同款，
    // 保证 Octree 初始化即覆盖全场景，逐实体 AddEntity 不再触发动态扩容 O(N²) 卡死）
    {
        if (m_desc.worldConfig && m_desc.worldConfig->IsConfigured()) {
            sceneData->worldConfig = m_desc.worldConfig;
        } else {
            Resource::WorldConfigDesc wc;
            float cMinX = 1e30f, cMaxX = -1e30f, cMinY = 1e30f, cMaxY = -1e30f, cMinZ = 1e30f, cMaxZ = -1e30f;
            for (const auto &e : m_desc.entities) {
                if (e.transform && e.transform->position.size() >= 3) {
                    // 防御性修复（2026-08-10）：跳过非有限 position——NaN/Inf 参与 min/max 会污染
                    // 包围盒推断（cMinX..cMaxZ 变 NaN）→ worldSize NaN → halfCells (int)ceil(NaN) UB
                    // → 格子遍历爆炸。此处仅初始化推断（口径 = 实体位置点，不含 mesh radius）；
                    // 完整覆盖由 OctreeSystem::Build 阶段 1 按真实 worldBounds 兜底（max 取大）。
                    const float px = e.transform->position[0];
                    const float py = e.transform->position[1];
                    const float pz = e.transform->position[2];
                    if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz))
                        continue;
                    cMinX = (std::min)(cMinX, px);
                    cMaxX = (std::max)(cMaxX, px);
                    cMinY = (std::min)(cMinY, py);
                    cMaxY = (std::max)(cMaxY, py);
                    cMinZ = (std::min)(cMinZ, pz);
                    cMaxZ = (std::max)(cMaxZ, pz);
                }
            }
            if (cMaxX > cMinX || cMaxY > cMinY || cMaxZ > cMinZ) {
                wc.center[0] = (cMinX + cMaxX) * 0.5f;
                wc.center[1] = (cMinY + cMaxY) * 0.5f;
                wc.center[2] = (cMinZ + cMaxZ) * 0.5f;
                const float spanX = cMaxX - cMinX, spanY = cMaxY - cMinY, spanZ = cMaxZ - cMinZ;
                wc.worldSize = (std::max)({spanX, spanY, spanZ}) * 1.2f;
            }
            sceneData->worldConfig = wc;
        }
    }

    // 携带原始依赖/materials/baseURL（供编辑器重新导出用）
    sceneData->originalDependencies = m_originalDependencies;
    sceneData->originalMaterials = m_originalMaterials;
    sceneData->baseURL = m_originalBaseURL;

    // 携带天空盒 GPU 句柄（Tab 切换时重建用）
    if (m_desc.sceneEnvironment.skybox) {
        const auto &sb = *m_desc.sceneEnvironment.skybox;
        auto texIt = texResourceMap.find(sb.texture);
        if (texIt != texResourceMap.end() && texIt->second.IsValid())
            sceneData->skyboxTextureHandle = texIt->second;
        if (!sb.geometry.empty()) {
            auto geoIt = sceneData->geoMap.find(sb.geometry);
            if (geoIt != sceneData->geoMap.end() && geoIt->second.IsValid())
                sceneData->skyboxGeometryHandle = geoIt->second;
        } else if (!sb.procedural.type.empty() && !m_skyboxProceduralHandles.empty()) {
            sceneData->skyboxGeometryHandle = m_skyboxProceduralHandles.back();
        }
    }

    static std::atomic<uint32_t> s_sceneId{0};
    uint32_t sceneId = ++s_sceneId;
    std::string storeKey = "scene_construct_" + std::to_string(sceneId);
    Core::SharedDataStore::GetInstance().StoreTypedData(storeKey, sceneData);

    // 获取材质 GPU 数据（POD，按值捕获安全）
    auto materialList = m_context->MaterialMgr->GetGPUMaterialList();
    std::vector<Renderer::MaterialConstants> gpuData;
    gpuData.reserve(materialList.size());
    for (auto &[idx, constants] : materialList)
        gpuData.push_back(constants);

    if (!gpuData.empty()) {
        // 标记子任务开始——batch 的 onAllComplete 暂停触发，等待材质 buffer 上传完成
        m_batch->OnSubTaskBegin();

        // ── 提交 LoadTask：后台线程创建 buffer + COPY 上传 ──
        auto *cmdMgr = &m_context->DeviceContext->GetCommandManager();
        ID3D12Device *device = m_context->DeviceContext->GetDevice();

        // 共享状态：gpuWork 写入 bufH，onComplete 消费
        struct SharedUpload {
            Resource::GpuResourceHandle defaultBufHandle;
            uint32_t elementCount = 0;
        };
        auto shared = std::make_shared<SharedUpload>();

        Async::LoadTask task;
        task.name = "MatBufUp:" + m_desc.metadata.name;

        // cpuWork 空操作：实际工作在 gpuWork 中，但 SubmitLoadTask 要求 cpuWork 非空
        task.cpuWork = []() {};

        task.gpuWork = [gpuData = std::move(gpuData), device, cmdMgr, shared]() -> Async::GpuWorkItemPtr {
            auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
            size_t bufSize = gpuData.size() * sizeof(Renderer::MaterialConstants);

            // DEFAULT 堆，初始 COMMON 状态（COPY 命令列表 CopyResource 目标必须以 COMMON 创建，
            // 不能用 COPY_DEST：GBV #942 "Resources used in COPY command lists must start out in the
            // D3D12_RESOURCE_STATE_COMMON state. This includes Resources created in a COPY_SOURCE or COPY_DEST state."
            // SRV 状态由 DIRECT 队列 barrier 转出）
            auto bufH = gpuMgr.CreateBuffer(device, bufSize, L"Scene_MatBuf", D3D12_HEAP_TYPE_DEFAULT,
                                            D3D12_RESOURCE_STATE_COMMON);
            if (!bufH.IsValid())
                return nullptr;
            shared->defaultBufHandle = bufH;
            shared->elementCount = static_cast<uint32_t>(gpuData.size());

            // UPLOAD 堆
            auto upH = gpuMgr.CreateBuffer(device, bufSize, L"Scene_MatBuf_Up", D3D12_HEAP_TYPE_UPLOAD,
                                           D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!upH.IsValid()) {
                gpuMgr.Release(bufH, 0);
                return nullptr;
            }

            // Map + memcpy
            auto *upRes = gpuMgr.GetResource(upH);
            void *mapped;
            upRes->Map(0, nullptr, &mapped);
            memcpy(mapped, gpuData.data(), bufSize);
            upRes->Unmap(0, nullptr);

            // COPY 命令
            uint64_t copyCompleted = cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_COPY);
            auto cpAllocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(copyCompleted);
            auto *cpAlloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(cpAllocH);
            auto cpCmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_COPY>(cpAlloc);
            auto cpCmd = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_COPY>(cpCmdH);
            cpCmd.Get()->CopyResource(gpuMgr.GetResource(bufH), upRes);
            cpCmd.Close();

            // DIRECT 屏障命令（COMMON → SRV；资源初始为 COMMON，barrier 必须从 COMMON 转出）
            uint64_t directCompleted = cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
            uint64_t directFence = cmdMgr->GetNextSequence();
            auto drAllocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(directCompleted);
            auto *drAlloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(drAllocH);
            auto drCmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(drAlloc);
            auto drCmd = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(drCmdH);
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(bufH), D3D12_RESOURCE_STATE_COMMON,
                                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            drCmd.Get()->ResourceBarrier(1, &barrier);
            drCmd.Close();

            // 注意：不在后台线程释放上传缓冲！
            // 改为存入 GpuWorkItem，由 CheckPendingCompletions 在 GPU 完成后释放
            auto item = std::make_shared<Async::GpuWorkItem>();
            item->uploadBufferHandles.push_back(upH);
            item->copyCmdListHandle = cpCmdH;
            item->copyAllocatorHandle = cpAllocH;
            item->directCmdListHandle = drCmdH;
            item->directAllocatorHandle = drAllocH;
            item->ready.store(true, std::memory_order_release);
            return item;
        };

        // onComplete（主线程）：SRV + 通知 batch 子任务完成
        task.onComplete = [this, shared, storeKey, sceneId, batch = m_batch](bool success) {
            m_context->Logging->Info("[SceneConstructor] MatBuf onComplete: success={}, handleValid={}", success,
                                     shared->defaultBufHandle.IsValid());
            try {
                if (!success || !shared->defaultBufHandle.IsValid()) {
                    m_context->Logging->Error(
                        "[SceneConstructor] Material buffer upload failed for '{}' (success={}, handleValid={})",
                        storeKey, success, shared->defaultBufHandle.IsValid());
                    if (m_onComplete)
                        m_onComplete(false);
                    return;
                }

                auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
                ID3D12Device *device = m_context->DeviceContext->GetDevice();
                if (!device) {
                    m_context->Logging->Error("[SceneConstructor] Device is null in onComplete");
                    if (m_onComplete)
                        m_onComplete(false);
                    return;
                }
                // m_context->Logging->Info("[SceneConstructor] onComplete: allocating SRV for scene '{}'",
                //                          m_desc.metadata.name);

                // SRV
                uint32_t srvIdx = m_context->DescriptorHeaps->Allocate(m_heapTag, Resource::PartitionType::Buffer);
                if (srvIdx != UINT32_MAX) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srvDesc.Buffer.FirstElement = 0;
                    srvDesc.Buffer.NumElements = shared->elementCount;
                    srvDesc.Buffer.StructureByteStride = sizeof(Renderer::MaterialConstants);
                    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

                    ID3D12Resource *bufRes = gpuMgr.GetResource(shared->defaultBufHandle);
                    if (!bufRes) {
                        m_context->Logging->Error("[SceneConstructor] Buffer resource is null in onComplete");
                        if (m_onComplete)
                            m_onComplete(false);
                        return;
                    }

                    D3D12_CPU_DESCRIPTOR_HANDLE cpuH = m_context->DescriptorHeaps->GetPartitionCpuHandle(
                        Resource::PartitionType::Buffer, srvIdx, m_heapTag);
                    device->CreateShaderResourceView(bufRes, &srvDesc, cpuH);

                    D3D12_GPU_DESCRIPTOR_HANDLE gpuH = m_context->DescriptorHeaps->GetPartitionGpuHandle(
                        Resource::PartitionType::Buffer, srvIdx, m_heapTag);
                    m_context->MaterialMgr->SetMaterialBufferSRV(gpuH);
                    m_context->Logging->Info("[SceneConstructor] SRV created for scene material buffer");
                } else {
                    m_context->Logging->Warn("[SceneConstructor] SRV allocation failed for scene '{}'",
                                             m_desc.metadata.name);
                }

                // 通知 batch 子任务完成（onAllComplete 已触发，此处仅递减计数）
                if (batch)
                    batch->OnSubTaskEnd();

                OnSceneReady(sceneId);
            } catch (const std::exception &e) {
                m_context->Logging->Error("[SceneConstructor] onComplete exception: {}", e.what());
                if (m_onComplete)
                    m_onComplete(false);
            } catch (...) {
                m_context->Logging->Error("[SceneConstructor] onComplete unknown exception");
                if (m_onComplete)
                    m_onComplete(false);
            }
        };

        m_context->BackgroundExecutor->SubmitLoadTask(std::move(task));
    } else {
        // 无材质，无需上传，直接发事件
        OnSceneReady(sceneId);
    }
}

// ========================================================================
// OnSceneReady — 场景所有依赖（含材质 buffer）GPU 就绪后的回调
// ========================================================================

void SceneConstructor::OnSceneReady(uint64_t sceneId) {
    // 发事件通知 SceneConstructSystem
    // payload: 高位 = GENERATOR_TYPE_SCENE_CONSTRUCTOR, 低位 = sceneId
    uint64_t payload = (static_cast<uint64_t>(GENERATOR_TYPE_SCENE_CONSTRUCTOR) << 32) | static_cast<uint64_t>(sceneId);
    Event::MessageDispatcher::GetInstance()->PostEvent(
        static_cast<uint32_t>(Event::EventType::GeneratorTaskCompleteEvent), 0, payload,
        Event::EventPriority::P2_Normal);

    m_context->Logging->Info("[SceneConstructor] Scene '{}' ready (id={})", m_desc.metadata.name, sceneId);
    if (m_onComplete)
        m_onComplete(true);
    m_loading = false;
}

void SceneConstructor::ConstructEntity(ECS::Entity entity, const Resource::EntityDesc &eDesc,
                                       const std::unordered_map<std::string, GeometryHandle> &geoMap,
                                       const std::unordered_map<std::string, MaterialHandle> &matMap,
                                       const Resource::SceneEnvironment &sceneEnv, ECS::Registry *registry,
                                       Boot::GameContext *context) {
    auto *log = context->Logging;
    auto *geoMgr = context->GeometryResourceManager;

    // 0. NameComponent — 名称与持久化 ID
    {
        ECS::NameComponent nameComp;
        // 优先从 JSON 恢复 persistentId（关系引用的 targetId 基于此匹配）
        // 无 JSON ID 时（如编辑器新建实体），由 NextPersistentId 分配
        if (!eDesc.persistentId.empty()) {
            nameComp.persistentId = std::stoull(eDesc.persistentId, nullptr, 16);
        } else {
            nameComp.persistentId = ECS::NextPersistentId();
        }
        nameComp.name = eDesc.name.empty() ? "Entity" : eDesc.name;
        registry->AddComponent<ECS::NameComponent>(entity, std::move(nameComp));
    }

    // 0a. StaticComponent — 静态实体持久化（v2 方案，见 StaticEntityPersistentBuffer.md）
    //     加载时按 persistentId 从 precomputed 取回烘焙矩阵（基准值，仅首次加载消费）；
    //     运行时依赖 worldDirty / 缓存矩阵 / 持久缓冲地址（Step3 填充）
    {
        const auto &env = sceneEnv;
        const Resource::PrecomputedStaticData *pre = env.precomputed ? &*env.precomputed : nullptr;
        bool isStatic = false;
        ECS::StaticComponent sc;
        if (pre && !eDesc.persistentId.empty()) {
            for (const auto &pi : pre->instances) {
                if (pi.persistentId == eDesc.persistentId) {
                    if (pi.world.size() >= 16)
                        std::memcpy(&sc.cachedWorld, pi.world.data(), sizeof(DirectX::XMFLOAT4X4));
                    if (pi.worldInvTranspose.size() >= 16) {
                        std::memcpy(&sc.cachedWorldInvTranspose, pi.worldInvTranspose.data(),
                                    sizeof(DirectX::XMFLOAT4X4));
                    } else {
                        sc.cachedWorldInvTranspose = sc.cachedWorld; // 均匀缩放：worldInvTranspose = world
                    }
                    sc.worldDirty = false; // 烘焙值就绪，无需运行时重算
                    isStatic = true;
                    break;
                }
            }
        }
        // 未在 precomputed 命中 → 按场景默认动静策略分配（默认 static）
        if (!isStatic && env.entityMotionPolicy == "static")
            isStatic = true;
        if (isStatic)
            registry->AddComponent<ECS::StaticComponent>(entity, std::move(sc));
    }

    // 1. Transform
    if (eDesc.transform) {
        const auto &t = *eDesc.transform;
        DirectX::XMFLOAT3 pos(t.position[0], t.position[1], t.position[2]);
        DirectX::XMFLOAT4 rot(
            t.rotation.size() >= 4 ? t.rotation[0] : 0.0f, t.rotation.size() >= 4 ? t.rotation[1] : 0.0f,
            t.rotation.size() >= 4 ? t.rotation[2] : 0.0f, t.rotation.size() >= 4 ? t.rotation[3] : 1.0f);
        DirectX::XMFLOAT3 scl(t.scale[0], t.scale[1], t.scale[2]);
        // 剔除距离（MPD @CullFar，承载于变换组件；缩放联动见 TransformComponent::GetEffectiveCullDistance）
        registry->AddComponent<ECS::TransformComponent>(entity, pos, rot, scl, t.cullDistance);
    }

    // 2. Mesh
    if (eDesc.mesh) {
        const auto &m = *eDesc.mesh;
        auto it = geoMap.find(m.geometry);
        if (it != geoMap.end() && it->second.IsValid()) {
            GeometryHandle geoHandle = it->second;
            geoMgr->Retain(geoHandle);

            log->Debug("[SceneConstructor] Entity '{}' mesh geometry='{}' found in geoMap, handle(idx={})", eDesc.name,
                       m.geometry, geoHandle.index);

            ECS::MeshComponent meshComp;
            LODMesh lodMesh;
            lodMesh.lodChain = {geoHandle};
            meshComp.lodMeshHandle = context->LODSystem->RegisterLODMesh(lodMesh);
            meshComp.receivesShadow = m.receivesShadow;

            if (auto *bounds = geoMgr->GetBounds(geoHandle)) {
                meshComp.localBounds = *bounds;
            }

            // 渲染槽位：填充单个 RenderSlotComponent（§4.1b/c/d，Slot.shaderType = 渲染器标记）
            // 取代已废弃的三材质组件分发挂载（§4.1a PBRMaterialComponent 等）
            const std::vector<SubMeshInfo> *subMeshes = geoMgr->GetSubMeshInfo(geoHandle);
            log->Debug("[SceneConstructor] Entity '{}' GetSubMeshInfo: {} subMeshes (ptr={})", eDesc.name,
                       subMeshes ? subMeshes->size() : 0, (void *)subMeshes);
            ECS::RenderSlotComponent slotComp;
            // 按材质 key 聚合（定案 7.2a，2026-08-08）：同材质的多个子网格区间并入同一槽位，
            // 取消"槽位 i ↔ 子网格 i 一对一"——子网格不应决定槽位/桶粒度（否则桶=子网格数，
            // ExecuteIndirect 命令爆炸）。槽位粒度 = 材质（渲染器），subMeshRanges 聚合多段。
            for (size_t i = 0; i < m.materials.size(); ++i) {
                auto matIt = matMap.find(m.materials[i]);
                if (matIt == matMap.end() || !matIt->second.IsValid()) {
                    log->Warn("[SceneConstructor][Diag] Entity '{}' slot#{}: '{}' NOT FOUND in matMap", eDesc.name, i,
                              m.materials[i]);
                    continue;
                }

                // 路由键：MaterialData.name = .mat shader 字符串（MaterialLoadTask L78）
                const MaterialData *md =
                    context->MaterialMgr ? context->MaterialMgr->GetMaterial(matIt->second) : nullptr;
                ShaderType st = md ? ParseShaderType(md->name) : ShaderType::Unknown;

                // 查找同材质槽位（materials 数量小，线性查找；用索引避免 push_back 失效指针）
                int slotIdx = -1;
                for (size_t j = 0; j < slotComp.slots.size(); ++j) {
                    if (slotComp.slots[j].material == matIt->second) {
                        slotIdx = static_cast<int>(j);
                        break;
                    }
                }
                if (slotIdx < 0) {
                    ECS::RenderSlot newSlot;
                    newSlot.material = matIt->second;
                    newSlot.shaderType = st;
                    slotComp.slots.push_back(std::move(newSlot));
                    slotIdx = static_cast<int>(slotComp.slots.size() - 1);
                }

                log->Debug("[SceneConstructor] Entity '{}' slot#{}: material='{}' handleIdx={} shaderType={}",
                           eDesc.name, i, m.materials[i], matIt->second.index, static_cast<int>(st));

                // 子网格区间并入该材质槽（同材质多子网格 → subMeshRanges 多段聚合）
                if (subMeshes && i < subMeshes->size()) {
                    slotComp.slots[slotIdx].subMeshRanges.push_back(
                        {(*subMeshes)[i].startIndex, (*subMeshes)[i].indexCount});
                    log->Debug("[SceneConstructor] Entity '{}' slot#{}: subMeshRange({},{}) from GetSubMeshInfo",
                               eDesc.name, i, (*subMeshes)[i].startIndex, (*subMeshes)[i].indexCount);
                } else if (subMeshes && !subMeshes->empty()) {
                    slotComp.slots[slotIdx].subMeshRanges.push_back(
                        {(*subMeshes)[0].startIndex, (*subMeshes)[0].indexCount});
                } else {
                    log->Warn("[SceneConstructor] Entity '{}' slot#{}: NO submesh info, skipping", eDesc.name, i);
                }
                // [Diag] 槽位子网格区间统计（验证聚合是否生效：slotRanges>1 表示同材质多段聚合成功；
                // 恒 1 → 多子网格实体只画第 1 段 → 与 Builder multiSegBuckets=0 交叉印证）
                log->Info(
                    "[SceneConstructor][Diag] Entity '{}' slot#{}: '{}' -> handleIndex={} shaderType={} slotRanges={}",
                    eDesc.name, i, m.materials[i], matIt->second.index, static_cast<int>(st),
                    slotComp.slots[slotIdx].subMeshRanges.size());
            }
            if (slotComp.IsValid()) {
                registry->AddComponent<ECS::RenderSlotComponent>(entity, std::move(slotComp));
                // §4.1b/c/d：实体 CRUD → 缓存表 MarkDirty（下帧 Builder 检查后重建）
                if (context && context->RenderSlotCache)
                    context->RenderSlotCache->MarkDirty();
            }

            registry->AddComponent<ECS::MeshComponent>(entity, std::move(meshComp));
        } else {
            log->Warn("[SceneConstructor] Geometry '{}' not found for entity '{}'", m.geometry, eDesc.name);
        }
    }

    // 3. Light
    if (eDesc.light) {
        const auto &ld = *eDesc.light;
        ECS::LightComponent lightComp;
        if (ld.type == "directional")
            lightComp.type = 0.0f;
        else if (ld.type == "point")
            lightComp.type = 1.0f;
        else if (ld.type == "spot")
            lightComp.type = 2.0f;
        if (ld.color.size() >= 4) {
            lightComp.strength = {ld.color[0], ld.color[1], ld.color[2], ld.intensity};
        } else if (ld.color.size() >= 3) {
            lightComp.strength = {ld.color[0], ld.color[1], ld.color[2], ld.intensity};
        }
        lightComp.range = ld.range.value_or(lightComp.range);
        if (ld.falloffStart.has_value())
            lightComp.falloffStart = *ld.falloffStart;
        if (ld.falloffEnd.has_value())
            lightComp.falloffEnd = *ld.falloffEnd;
        if (ld.spotPower.has_value())
            lightComp.spotPower = *ld.spotPower;
        lightComp.castShadow = ld.castsShadow;
        if (ld.shadowBias.has_value())
            lightComp.shadowBias = *ld.shadowBias;
        registry->AddComponent<ECS::LightComponent>(entity, std::move(lightComp));
        log->Info("[SceneConstructor] LightComponent added to '{}': type={} intensity={} range={} falloff={}-{} "
                  "spotPower={} shadow={}",
                  eDesc.name, ld.type, ld.intensity, ld.range.value_or(lightComp.range),
                  ld.falloffStart.value_or(lightComp.falloffStart), ld.falloffEnd.value_or(lightComp.falloffEnd),
                  ld.spotPower.value_or(lightComp.spotPower), ld.castsShadow);
    }

    // 4. Camera
    if (eDesc.camera) {
        const auto &cd = *eDesc.camera;
        ECS::CameraComponent cameraComp;
        cameraComp.fov = cd.fov;
        cameraComp.orthoSize = cd.orthoSize;
        cameraComp.nearPlane = cd.nearPlane;
        cameraComp.farPlane = cd.farPlane;
        cameraComp.isMain = cd.isMain;
        cameraComp.projection =
            (cd.projection == "orthographic") ? ECS::ProjectionType::Orthographic : ECS::ProjectionType::Perspective;
        registry->AddComponent<ECS::CameraComponent>(entity, std::move(cameraComp));
        log->Info(
            "[SceneConstructor] CameraComponent added to '{}': fov={} orthoSize={} near={} far={} proj={} isMain={}",
            eDesc.name, cd.fov, cd.orthoSize, cd.nearPlane, cd.farPlane, cd.projection, cd.isMain);
    }

    // 4a. ReflectionProbe
    if (eDesc.reflectionProbe) {
        const auto &rd = *eDesc.reflectionProbe;
        ECS::ReflectionProbeComponent probeComp;
        probeComp.captureRange = rd.range;
        probeComp.resolution = rd.resolution;
        probeComp.updatePriority = rd.updatePriority;
        registry->AddComponent<ECS::ReflectionProbeComponent>(entity, std::move(probeComp));
        log->Info("[SceneConstructor] ReflectionProbeComponent added to '{}': range={} resolution={} priority={}",
                  eDesc.name, rd.range, rd.resolution, rd.updatePriority);
    }

    // 4b. Relationships（实体关系）
    for (const auto &rd : eDesc.relationships) {
        ECS::RelationshipKind kind = ECS::RelationshipKind::Parent;
        if (rd.kind == "socket")
            kind = ECS::RelationshipKind::Socket;
        else if (rd.kind == "group")
            kind = ECS::RelationshipKind::Group;
        else if (rd.kind == "follow")
            kind = ECS::RelationshipKind::Follow;

        ECS::RelationshipComponent relComp;
        relComp.targetId = rd.targetId;
        relComp.kind = kind;
        registry->AddComponent<ECS::RelationshipComponent>(entity, std::move(relComp));

        if (kind == ECS::RelationshipKind::Socket && !rd.socketName.empty()) {
            ECS::SocketAttachmentComponent socketComp;
            socketComp.socketName = rd.socketName;
            registry->AddComponent<ECS::SocketAttachmentComponent>(entity, std::move(socketComp));
        }
    }

    // 5. 水组件（WaterComponent）— 波浪参数直接写入 ECS，由 WaterManager::CollectFromECS 收集
    if (eDesc.water) {
        const auto &wd = *eDesc.water;

        // 调试：检查 mesh 和 materials 状态
        if (eDesc.mesh) {
            log->Debug("[SceneConstructor] Water '{}' mesh geometry='{}' materials.size={}", eDesc.name,
                       eDesc.mesh->geometry, eDesc.mesh->materials.size());
        } else {
            log->Debug("[SceneConstructor] Water '{}' has no mesh component", eDesc.name);
        }

        // 查找材质（MeshDesc 中有 materials[]，用第一个 key 查找 matMap）
        if (eDesc.mesh && !eDesc.mesh->materials.empty()) {
            auto matIt = matMap.find(eDesc.mesh->materials[0]);
            if (matIt != matMap.end() && matIt->second.IsValid()) {
                // 不再创建持久 ObjectConstants CB（§10.5 数据上传铁律：Builder 填 CPU 数据，
                // FrameSync 每帧 RingBuffer 上传，禁止 ConstructEntity 分配 GPU 资源）
                ECS::WaterComponent waterComp;
                waterComp.materialHandle = matIt->second;
                waterComp.amplitude = wd.amplitude;
                waterComp.frequency = wd.frequency;
                waterComp.speed = wd.speed;
                waterComp.direction = wd.direction;
                registry->AddComponent<ECS::WaterComponent>(entity, waterComp);
                log->Info("[SceneConstructor] WaterComponent added to '{}': wave=({},{},{},{})", eDesc.name,
                          wd.amplitude, wd.frequency, wd.speed, wd.direction);
            }
        } else {
            log->Warn("[SceneConstructor] Water '{}' has no material, skipped", eDesc.name);
        }
    }

    // 6. 标签组件（天空盒已移至 OnDependenciesLoaded 通过 SkyboxManager 设置，不在实体中）
    if (eDesc.opaque)
        registry->AddComponent<ECS::OpaqueTag>(entity);
    if (eDesc.transparent)
        registry->AddComponent<ECS::TransparentTag>(entity);

    // 7. 递归 children
    for (const auto &childDesc : eDesc.children) {
        ECS::Entity child = registry->CreateEntity();
        ConstructEntity(child, childDesc, geoMap, matMap, sceneEnv, registry, context);
    }
}

} // namespace DX12Engine::Scene
