#include "SceneConstructor.h"
#include "Boot/GameContext.h"
#include "Common/d3dUtil.h"
#include "Core/SharedDataStore/SharedDataStore.h"
#include "ECS/Core/Components/Render.h"
#include "ECS/Core/Components/Tags.h"
#include "ECS/Core/Registry.h"
#include "Event/EventRegistry.h"
#include "Event/MessageDispatcher.h"
#include "Logger/Logger.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "Renderer/Material/MaterialManager.h"
#include "Renderer/Material/MaterialResource.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/SkyboxManager.h"
#include "Renderer/Scene/WaterManager.h"
#include "Resource/AssetManager/AssetManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Texture/TextureManager.h"
#include <filesystem>

namespace DX12Engine::Scene {

using namespace DX12Engine::Resource;

void SceneConstructor::LoadScene(const SceneDescription &desc, Boot::GameContext *context, Callback onComplete) {
    m_desc = desc;
    m_context = context;
    m_onComplete = std::move(onComplete);
    m_loading = true;

    auto *log = context ? context->Logging : nullptr;

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
    std::vector<std::pair<std::string, AssetType>> assets;
    std::string root = context->GetProjectConfig().Root;
    std::string baseURL = desc.baseURL;

    auto resolve = [&root, &baseURL](const std::string &path) -> std::string {
        if (path.empty())
            return path;
        if (std::filesystem::path(path).is_absolute())
            return path;
        if (!baseURL.empty())
            return (std::filesystem::path(root) / baseURL / path).string();
        return (std::filesystem::path(root) / path).string();
    };

    for (const auto &[key, path] : desc.dependencies.meshes) {
        assets.emplace_back(resolve(path), AssetType::Mesh);
    }
    // 材质不加入 batch——等纹理加载完成后在 OnDependenciesLoaded 中统一解析注册
    for (const auto &[key, path] : desc.dependencies.textures) {
        assets.emplace_back(resolve(path), AssetType::Texture);
    }
    for (const auto &[key, path] : desc.dependencies.terrains) {
        assets.emplace_back(resolve(path), AssetType::Terrain);
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
            // log->Info("[SceneConstructor] Material '{}' registered: handle={} texSRV={}", key,
            //           static_cast<uint32_t>(matHandle.index), data.baseColorTextureId);
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
    if (m_desc.skybox) {
        const auto &sb = *m_desc.skybox;
        GpuResourceHandle texRes;
        auto texIt = texResourceMap.find(sb.texture);
        if (texIt != texResourceMap.end() && texIt->second.IsValid())
            texRes = texIt->second;
        else
            log->Error("[SceneConstructor] Skybox texture '{}' not found", sb.texture);

        GeometryHandle skyGeo;
        if (!sb.geometry.empty()) {
            auto geoIt = geoMap.find(sb.geometry);
            if (geoIt != geoMap.end() && geoIt->second.IsValid()) {
                skyGeo = geoIt->second;
                m_context->GeometryResourceManager->Retain(skyGeo);
            }
        }

        if (texRes.IsValid() && skyGeo.IsValid()) {
            Renderer::SkyboxManager::GetInstance().SetSkybox(texRes, skyGeo);
            log->Info("[SceneConstructor] Skybox set via SkyboxManager: tex='{}' geo='{}'", sb.texture, sb.geometry);

            // 将环境贴图注入 WaterManager（水体共享反射用）
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

    // ================================================================
    // Step 6: 场景数据存入 SharedDataStore + 材质 buffer 后台上传
    // ================================================================

    // 组装场景数据
    auto sceneData = std::make_shared<SceneConstructData>();
    sceneData->sceneName = m_desc.metadata.name;
    sceneData->entities = m_desc.entities;
    sceneData->geoMap = std::move(geoMap);
    sceneData->matMap = std::move(matMap);

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

            // DEFAULT 堆，初始 COPY_DEST 状态（COPY 队列可直接写入）
            auto bufH = gpuMgr.CreateBuffer(device, bufSize, L"Scene_MatBuf", D3D12_HEAP_TYPE_DEFAULT,
                                            D3D12_RESOURCE_STATE_COPY_DEST);
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

            // DIRECT 屏障命令（COPY_DEST → SRV）
            uint64_t directCompleted = cmdMgr->GetCompletedFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
            uint64_t directFence = cmdMgr->GetNextSequence();
            auto drAllocH = cmdMgr->AcquireAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(directCompleted);
            auto *drAlloc = cmdMgr->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(drAllocH);
            auto drCmdH = cmdMgr->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(drAlloc);
            auto drCmd = cmdMgr->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(drCmdH);
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                gpuMgr.GetResource(bufH), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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
                uint32_t srvIdx = m_context->DescriptorHeaps->Allocate(Resource::PartitionType::Buffer);
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

                    D3D12_CPU_DESCRIPTOR_HANDLE cpuH =
                        m_context->DescriptorHeaps->GetPartitionCpuHandle(Resource::PartitionType::Buffer, srvIdx);
                    device->CreateShaderResourceView(bufRes, &srvDesc, cpuH);

                    D3D12_GPU_DESCRIPTOR_HANDLE gpuH =
                        m_context->DescriptorHeaps->GetPartitionGpuHandle(Resource::PartitionType::Buffer, srvIdx);
                    m_context->MaterialMgr->SetMaterialBufferSRV(gpuH);
                    m_context->Logging->Info("[SceneConstructor] SRV created for scene material buffer");
                } else {
                    m_context->Logging->Warn("[SceneConstructor] SRV allocation failed for scene '{}'",
                                             m_desc.metadata.name);
                }

                // 发事件通知 SceneConstructSystem
                Event::MessageDispatcher::GetInstance()->PostEvent(
                    static_cast<uint32_t>(Event::EventType::SceneConstructReadyEvent), 0,
                    static_cast<uint64_t>(sceneId), Event::EventPriority::P2_Normal);

                // 通知 batch 子任务完成（onAllComplete 已触发，此处仅递减计数）
                if (batch)
                    batch->OnSubTaskEnd();

                m_context->Logging->Info("[SceneConstructor] Scene '{}' ready (id={})", m_desc.metadata.name, sceneId);
                if (m_onComplete)
                    m_onComplete(true);
                m_loading = false;
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
        Event::MessageDispatcher::GetInstance()->PostEvent(
            static_cast<uint32_t>(Event::EventType::SceneConstructReadyEvent), 0, static_cast<uint64_t>(sceneId),
            Event::EventPriority::P2_Normal);
        m_context->Logging->Info("[SceneConstructor] Scene '{}' ready (no materials, id={})", m_desc.metadata.name,
                                 sceneId);
        if (m_onComplete)
            m_onComplete(true);
        m_loading = false;
    }
}

void SceneConstructor::ConstructEntity(ECS::Entity entity, const Resource::EntityDesc &eDesc,
                                       const std::unordered_map<std::string, GeometryHandle> &geoMap,
                                       const std::unordered_map<std::string, MaterialHandle> &matMap,
                                       ECS::Registry *registry, Boot::GameContext *context) {
    auto *log = context->Logging;
    auto *geoMgr = context->GeometryResourceManager;

    // 1. Transform
    if (eDesc.transform) {
        const auto &t = *eDesc.transform;
        DirectX::XMFLOAT3 pos(t.position[0], t.position[1], t.position[2]);
        DirectX::XMFLOAT3 rot(t.rotation[0], t.rotation[1], t.rotation[2]);
        DirectX::XMFLOAT3 scl(t.scale[0], t.scale[1], t.scale[2]);
        registry->AddComponent<ECS::TransformComponent>(entity, pos, rot, scl);
    }

    // 2. Mesh
    if (eDesc.mesh) {
        const auto &m = *eDesc.mesh;
        auto it = geoMap.find(m.geometry);
        if (it != geoMap.end() && it->second.IsValid()) {
            GeometryHandle geoHandle = it->second;
            geoMgr->Retain(geoHandle);

            ECS::MeshComponent meshComp;
            LODMesh lodMesh;
            lodMesh.lodChain = {geoHandle};
            meshComp.lodMeshHandle = context->LODSystem->RegisterLODMesh(lodMesh);
            meshComp.receivesShadow = m.receivesShadow;

            if (auto *bounds = geoMgr->GetBounds(geoHandle)) {
                meshComp.localBounds = *bounds;
            }

            // 查找材质
            if (!m.material.empty()) {
                auto matIt = matMap.find(m.material);
                if (matIt != matMap.end() && matIt->second.IsValid()) {
                    meshComp.materialHandle = matIt->second;
                }
            }

            registry->AddComponent<ECS::MeshComponent>(entity, std::move(meshComp));
        } else {
            log->Warn("[SceneConstructor] Geometry '{}' not found for entity '{}'", m.geometry, eDesc.name);
        }
    }

    // 3. Light
    if (eDesc.light) {
        // TODO: LightComponent
        log->Info("[SceneConstructor] Light component skipped (TODO) for '{}'", eDesc.name);
    }

    // 4. Camera
    if (eDesc.camera) {
        // TODO: CameraComponent
        log->Info("[SceneConstructor] Camera component skipped (TODO) for '{}'", eDesc.name);
    }

    // 5. 水组件（WaterComponent）
    if (eDesc.water) {
        const auto &wd = *eDesc.water;

        // 查找材质（MeshDesc 中有 material key，用其查找 matMap）
        if (eDesc.mesh && !eDesc.mesh->material.empty()) {
            auto matIt = matMap.find(eDesc.mesh->material);
            if (matIt != matMap.end() && matIt->second.IsValid()) {
                // 注册波浪参数到 WaterManager
                Renderer::WaveParams waveParams;
                waveParams.amplitude = wd.amplitude;
                waveParams.frequency = wd.frequency;
                waveParams.speed = wd.speed;
                waveParams.direction = wd.direction;
                uint32_t wpIdx = Renderer::WaterManager::GetInstance().RegisterWaveParams(waveParams);

                // 分配持久 ObjectConstants CB（UPLOAD 堆，贯穿生命周期）
                D3D12_GPU_VIRTUAL_ADDRESS cbAddr = 0;
                if (context && context->DeviceContext) {
                    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
                    ID3D12Device *device = context->DeviceContext->GetDevice();
                    auto cbBuf =
                        gpuMgr.CreateBuffer(device, sizeof(Renderer::ObjectConstants), L"WaterObjCB_Persistent",
                                            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                    if (cbBuf.IsValid()) {
                        ID3D12Resource *cbRes = gpuMgr.GetResource(cbBuf);
                        // 不需要写入内容——世界矩阵在 Builder 中已经通过 PendingBatch 传递，
                        // 但这里是持久分配的，所以填默认值（单位矩阵），实际 PerObject 由 Builder 覆盖
                        // 实际上 Builder 不再分配，所以这里直接填正确的世界矩阵
                        // 但现在 Builder 中我们直接使用这个持久地址，所以填入单位矩阵占位
                        void *mapped = nullptr;
                        cbRes->Map(0, nullptr, &mapped);
                        memset(mapped, 0, sizeof(Renderer::ObjectConstants));
                        cbRes->Unmap(0, nullptr);
                        cbAddr = cbRes->GetGPUVirtualAddress();
                    }
                }

                ECS::WaterComponent waterComp;
                waterComp.materialHandle = matIt->second;
                waterComp.waveParamIndex = wpIdx;
                waterComp.objectCBAddress = cbAddr;
                registry->AddComponent<ECS::WaterComponent>(entity, waterComp);
                log->Info("[SceneConstructor] WaterComponent added to '{}': waveIdx={} cbAddr={:#x}", eDesc.name, wpIdx,
                          cbAddr);
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
        ConstructEntity(child, childDesc, geoMap, matMap, registry, context);
    }
}

} // namespace DX12Engine::Scene
