#include "EditorSceneManager.h"
#include "Asset/IO/Loader/SceneDescription.h"
#include "Boot/GameContext.h"
#include "Core/SharedDataStore/SharedDataStore.h"
#include "ECS/Core/Components/Name.h"
#include "ECS/Core/Components/Render.h"
#include "ECS/Core/Components/Tags.h"
#include "ECS/Core/Components/Transform.h"
#include "ECS/Core/Registry.h"
#include "EditorStrings.h"
#include "Event/EventTypes.h"
#include "Event/MessageDispatcher.h"
#include "Framework/SystemRegistry.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/Material/MaterialManager.h"
#include "Renderer/Scene/CameraManager.h"
#include "Renderer/Scene/SkyboxManager.h"
#include "Renderer/Scene/WaterManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Texture/TextureManager.h"
#include "Scene/SceneConstructor.h"
#include "ThirdParty/imgui/imgui.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace DX12Engine;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Event;
using namespace DX12Engine::Scheduler;

// ========================================================================
// 初始化/销毁
// ========================================================================

void EditorSceneManager::Initialize(DX12Engine::Scene::SceneManager *sceneMgr, DX12Engine::Boot::GameContext *context) {
    if (m_initialized)
        return;

    m_sceneMgr = sceneMgr;
    m_context = context;

    // 初始化缓存根目录
    if (context && context->ProjectConfig) {
        m_cacheRoot = (std::filesystem::path(context->GetProjectConfig().Root) / "Content/Cache/Editor").string();
        std::error_code ec;
        std::filesystem::create_directories(m_cacheRoot, ec);
    }

    m_initialized = true;
    m_context->Logging->Info("[EditorSceneManager] Initialized (cache root: {})", m_cacheRoot.empty() ? "none" : m_cacheRoot);
}

void EditorSceneManager::Shutdown() {
    if (!m_initialized)
        return;

    m_entityDescs.clear();
    m_sceneMgr = nullptr;
    m_context = nullptr;
    m_initialized = false;
}

// ========================================================================
// 场景构造系统注册
// ========================================================================

void EditorSceneManager::RegisterSceneConstructSystem() {
    if (!m_initialized || !m_sceneMgr)
        return;

    SystemRegistry::Register(
        {.name = "EditorSceneConstructSystem",
         .func =
             [this](const MessageContext &ctx) {
                 // payload: 高位 = 生成器类型, 低位 = 任务数据（sceneId）
                 uint32_t generatorType = static_cast<uint32_t>((ctx.payload >> 32) & 0xFFFFFFFF);
                 uint32_t sceneId = static_cast<uint32_t>(ctx.payload & 0xFFFFFFFF);

                 // 只处理 SceneConstructor 的完成事件
                 if (generatorType != DX12Engine::Scene::GENERATOR_TYPE_SCENE_CONSTRUCTOR)
                     return;

                 std::string storeKey = "scene_construct_" + std::to_string(sceneId);
                 m_context->Logging->Info("[EditorSceneConstructSystem] Triggered (id={}, key={})", sceneId, storeKey);

                 auto &store = Core::SharedDataStore::GetInstance();
                 auto sceneData = store.GetTypedData<DX12Engine::Scene::SceneConstructData>(storeKey);
                 if (!sceneData) {
                     m_context->Logging->Error("[EditorSceneConstructSystem] Scene data not found: {}", storeKey);
                     return;
                 }

                 m_context->Logging->Info("[EditorSceneConstructSystem] Found data: entities={}, geoMap={}, matMap={}",
                                          sceneData->entities.size(), sceneData->geoMap.size(),
                                          sceneData->matMap.size());

                 // 构造所有实体
                 OnSceneConstructReady(*sceneData);

                 store.RemoveTypedData(storeKey);
             },
         .phase = TaskPhase::EarlyUpdate,
         .threadType = ThreadType::Main,
         .interestedMessages = {Event::GeneratorTaskCompleteEvent::StaticTypeHash}});
}

// ========================================================================
// 场景生命周期管理
// ========================================================================

bool EditorSceneManager::SwitchScene(const std::string &newSceneName, const std::filesystem::path &sceneFilePath) {
    if (!m_initialized || !m_sceneMgr)
        return false;

    // 递增场景切换序列号（用于检测过期异步回调）
    m_sceneSwitchId++;

    // 1. 保存当前场景的编辑器状态到快照
    SaveCurrentSnapshotToDisk();

    // 2. 更新 Tab 追踪
    if (sceneFilePath.empty()) {
        // 空路径 → 默认场景加载，不创建 Tab（已废弃，保留代码兼容）
        m_context->Logging->Warn("[SwitchScene] Empty path, setting activeTabIndex=0");
        m_activeTabIndex = 0;
    } else {
        // 检查是否已在 Tab 列表中
        auto it = std::find_if(m_openTabs.begin(), m_openTabs.end(),
                               [&](const SceneTab &tab) { return tab.filePath == sceneFilePath; });

        if (it != m_openTabs.end()) {
            // 已存在 → 不重复加载，不切换 Tab（由用户点击 Tab 触发切换）
            size_t newIdx = std::distance(m_openTabs.begin(), it);
            m_context->Logging->Info("[SwitchScene] EXISTS tab='{}' (idx={}, sceneId={}), skip (user click to switch)",
                                     it->name, newIdx, it->sceneId);
            return false;  // 返回 false，调用方应跳过加载
        }

        // 不存在 → 新建 Tab（仅累加，不自动切换，由用户点击 Tab 触发）
        SceneTab newTab;
        newTab.name = newSceneName;
        newTab.filePath = sceneFilePath;
        newTab.dirty = false;
        newTab.sceneId = m_nextSceneId++;
        m_openTabs.push_back(std::move(newTab));

        // 确保 m_snapshots 与 m_openTabs 对齐
        m_snapshots.resize(m_openTabs.size());

        // 标记异步加载中
        m_isLoading = true;

        // 只有无 Tab 时（首个 Tab）才自动激活
        size_t newIdx = m_openTabs.size() - 1;
        SceneSnapshot &snap = m_snapshots[newIdx];
        if (m_openTabs.size() == 1) {
            m_activeTabIndex = newIdx;
            m_context->Logging->Info("[SwitchScene] First tab, auto-activated: '{}' (idx={})", newSceneName, newIdx);
        } else {
            m_context->Logging->Info("[SwitchScene] Added tab '{}' (idx={}), keeping active tab idx={}",
                                     newSceneName, newIdx, m_activeTabIndex);
        }

        // 尝试从磁盘缓存加载快照，若无则使用默认空快照
        std::string cachePath = (std::filesystem::path(m_cacheRoot) /
            std::filesystem::path(sceneFilePath).filename().replace_extension(".snapshot.json")).string();
        if (!snap.LoadFrom(cachePath)) {
            snap = SceneSnapshot{};
            m_context->Logging->Info("[SwitchScene] No cache for '{}', using default empty snapshot", newSceneName);
        } else {
            m_context->Logging->Info("[SwitchScene] Loaded cache for '{}': camera({:.1f},{:.1f},{:.1f})",
                                     newSceneName, snap.cameraPosition.x, snap.cameraPosition.y, snap.cameraPosition.z);
        }

        m_context->Logging->Info("[SwitchScene] NEW tab='{}' (idx={}, sceneId={})", newSceneName, newIdx,
                                 m_openTabs[newIdx].sceneId);
    }

    // 3. 更新活跃场景追踪
    m_activeScenePath = sceneFilePath;
    m_sceneFilePath = sceneFilePath;
    m_dirty = false;
    return true;  // 返回 true，调用方应继续加载场景
}

// ========================================================================
// 场景文件管理
// ========================================================================

void EditorSceneManager::NewScene(const std::string &name) {
    if (!m_initialized)
        return;

    m_context->Logging->Info("[EditorSceneManager] Creating new scene: {}", name);
    m_sceneFilePath.clear();
    m_dirty = false;
    m_entityDescs.clear();

    // 清除所有现有实体
    if (m_sceneMgr)
        m_sceneMgr->RemoveAllEntities();

    // 重置相机到默认位置（新场景无缓存）
    ResetCameraToDefault();
}

void EditorSceneManager::SaveScene() {
    if (!m_initialized)
        return;

    if (m_sceneFilePath.empty()) {
        m_context->Logging->Warn("[EditorSceneManager] No scene file path set, cannot save");
        return;
    }
    SaveSceneAs(m_sceneFilePath);
}

void EditorSceneManager::SaveSceneAs(const std::filesystem::path &filePath) {
    if (!m_initialized)
        return;

    m_context->Logging->Info("[EditorSceneManager] Saving scene to: {}", filePath.string());

    // 导出当前场景为 SceneDescription
    auto desc = ExportToDescription();

    // 序列化为 JSON 并写入文件
    // TODO: 使用 SceneLoader::SaveToFile
    // SceneLoader::SaveToFile(desc, filePath);

    m_sceneFilePath = filePath;
    m_dirty = false;
}

// ========================================================================
// EntityDesc 编辑
// ========================================================================

Resource::EntityDesc *EditorSceneManager::GetMutableEntityDesc(uint64_t entity) {
    auto it = m_entityDescs.find(entity);
    if (it != m_entityDescs.end())
        return &it->second;
    return nullptr;
}

void EditorSceneManager::UpdateEntityDesc(uint64_t entity, const Resource::EntityDesc &newDesc) {
    if (!m_initialized || !m_sceneMgr)
        return;

    // 更新 EntityDesc 缓存
    m_entityDescs[entity] = newDesc;

    // 同步更新 ECS 组件
    auto *registry = m_sceneMgr->GetRegistry();
    if (!registry)
        return;

    auto e = static_cast<ECS::Entity>(entity);

    // 更新 Transform
    if (newDesc.transform.has_value()) {
        if (auto *transform = registry->TryGetComponent<TransformComponent>(e)) {
            const auto &t = newDesc.transform.value();
            if (t.position.size() >= 3)
                transform->position = XMFLOAT3(t.position[0], t.position[1], t.position[2]);
            if (t.rotation.size() >= 3)
                transform->rotation = XMFLOAT3(t.rotation[0], t.rotation[1], t.rotation[2]);
            if (t.scale.size() >= 3)
                transform->scale = XMFLOAT3(t.scale[0], t.scale[1], t.scale[2]);
        }
    }

    // 更新 Mesh
    if (newDesc.mesh.has_value()) {
        // TODO: 更新 MeshComponent 的 geometry/material handle
    }

    // 更新 Light
    if (newDesc.light.has_value()) {
        // TODO: 更新 LightComponent
    }

    m_dirty = true;
}

Resource::SceneDescription EditorSceneManager::ExportToDescription() const {
    Resource::SceneDescription desc;
    desc.version = 1;
    desc.metadata.name = m_sceneFilePath.stem().string();
    if (desc.metadata.name.empty())
        desc.metadata.name = "Untitled";

    // 从缓存的 EntityDesc 导出
    for (const auto &[handle, eDesc] : m_entityDescs) {
        desc.entities.push_back(eDesc);
    }

    // 收集依赖
    // TODO: 遍历实体，收集所有 mesh/texture 路径到 dependencies

    return desc;
}

Resource::SceneDescription EditorSceneManager::GetDefaultSceneDescription() {
    Resource::SceneDescription desc;
    desc.version = 1;
    desc.metadata.name = "EditorDefault";
    desc.metadata.description = "Editor default scene with skybox";
    desc.baseURL = "Content";
    desc.environment.ambientLight = {0.25f, 0.25f, 0.35f, 1.0f};
    desc.skybox = Resource::SkyboxDesc{};
    desc.skybox->texture = "default_sky";
    desc.skybox->geometry = "skybox_geo";
    desc.skybox->color = {0.5f, 0.5f, 0.8f, 1.0f};
    desc.dependencies.textures["default_sky"] = "Textures/StandardCubeMap.dds";
    desc.dependencies.meshes["skybox_geo"] = "Models/cube.dxmesh";
    return desc;
}

// ========================================================================
// 相机配置与默认位置
// ========================================================================

void EditorSceneManager::InitCameraConfig(DX12Engine::Boot::GameContext *context) {
    if (!context || !context->CameraMgr)
        return;

    uint32_t vpW = 1280;
    uint32_t vpH = 720;
    context->CameraMgr->Initialize(vpW, vpH);
    context->CameraMgr->SetGameContext(context);

    auto &mainCamera = context->CameraMgr->GetMainCamera();
    // 设置编辑器大远平面（防止网格被裁减）
    mainCamera.FarPlane = 5000.0f;
    mainCamera.CullFarPlane = 10000.0f;

    context->Logging->Info("[EditorSceneManager] Camera config initialized (FarPlane={})", mainCamera.FarPlane);
}

void EditorSceneManager::ResetCameraToDefault() {
    if (!m_context || !m_context->CameraMgr)
        return;

    auto &mainCamera = m_context->CameraMgr->GetMainCamera();
    mainCamera.Position = DirectX::XMFLOAT3(4.0f, 34.0f, -6.0f);
    mainCamera.Right = DirectX::XMFLOAT3(1.0f, 0.0f, 0.0f);
    mainCamera.Up = DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f);
    mainCamera.Forward = DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f);
    mainCamera.Rotation = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    m_context->CameraMgr->UpdateMainCamera();

    m_context->Logging->Info("[EditorSceneManager] Camera reset to default position ({:.1f},{:.1f},{:.1f})",
                             mainCamera.Position.x, mainCamera.Position.y, mainCamera.Position.z);
}

// ========================================================================
// 场景构造完成处理
// ========================================================================

void EditorSceneManager::OnSceneConstructReady(const Scene::SceneConstructData &sceneData) {
    if (!m_sceneMgr)
        return;

    // 捕获当前场景切换序列号，用于检测过期回调
    uint64_t captureSwitchId = m_sceneSwitchId;

    auto *registry = m_sceneMgr->GetRegistry();

    // 按文件路径匹配 Tab（而非 m_activeTabIndex 或 sceneName，避免过期回调写入错误 Tab）
    size_t tabIndex = SIZE_MAX;
    for (size_t i = 0; i < m_openTabs.size(); i++) {
        if (!sceneData.sceneFilePath.empty() &&
            std::filesystem::path(m_openTabs[i].filePath).lexically_normal() ==
            std::filesystem::path(sceneData.sceneFilePath).lexically_normal()) {
            tabIndex = i;
            break;
        }
    }

    if (tabIndex == SIZE_MAX) {
        m_context->Logging->Warn("[SceneConstruct] No matching tab for scene='{}' (path='{}'), discarding {} entities",
                                 sceneData.sceneName, sceneData.sceneFilePath, sceneData.entities.size());
        return;
    }

    // 在构造新场景实体之前，先恢复当前活跃 Tab 的全局管理器状态
    // 目的是抵消 SceneConstructor::OnDependenciesLoaded 中过早设置的
    // SkyboxManager/WaterManager 全局状态，避免环境光闪烁
    ApplyTabState(m_activeTabIndex);

    // 确保 m_snapshots 大小与 m_openTabs 对齐
    if (m_snapshots.size() <= tabIndex)
        m_snapshots.resize(m_openTabs.size());

    SceneSnapshot &snap = m_snapshots[tabIndex];

    // 缓存场景环境数据（天空盒、环境光）
    if (sceneData.skybox.has_value())
        snap.skybox = *sceneData.skybox;
    if (sceneData.environment.has_value())
        snap.environment = *sceneData.environment;

    // 缓存天空盒 GPU 句柄（Tab 切换时重建天空盒用）
    if (sceneData.skyboxTextureHandle.IsValid())
        snap.skyboxTextureHandle = sceneData.skyboxTextureHandle;
    if (sceneData.skyboxGeometryHandle.IsValid())
        snap.skyboxGeometryHandle = sceneData.skyboxGeometryHandle;

    // 缓存 GPU 资源映射
    snap.geoMap = sceneData.geoMap;
    snap.matMap = sceneData.matMap;

    // 日志：快照填充前状态
    m_context->Logging->Info("[SceneConstruct] Populating snapshot for tab='{}' (idx={}, sceneId={}): "
                             "skybox={}, env={}, geoMap={}, matMap={}",
                             m_openTabs[tabIndex].name, tabIndex,
                             m_openTabs[tabIndex].sceneId,
                             sceneData.skybox.has_value(), sceneData.environment.has_value(),
                             sceneData.geoMap.size(), sceneData.matMap.size());

    // 创建实体
    snap.entities.clear();
    snap.entityDescs.clear();
    uint32_t constructed = 0;

    for (const auto &eDesc : sceneData.entities) {
        // Step 1: 创建空实体
        uint64_t handle = m_sceneMgr->CreateEntity();
        auto entity = static_cast<ECS::Entity>(handle);

        // Step 2: 生成器创建组件
        Scene::SceneConstructor::ConstructEntity(entity, eDesc, sceneData.geoMap, sceneData.matMap, registry,
                                                 m_context);

        // 缓存 EntityDesc（用于编辑器导出）
        m_entityDescs[handle] = eDesc;

        // Step 3: 添加场景标记组件（用于 Builder 过滤和 Tab 切换时保留实体）
        uint64_t activeSceneId = m_openTabs[tabIndex].sceneId;
        registry->AddComponent<SceneTagComponent>(entity, SceneTagComponent{activeSceneId});

        // 记录到当前 Tab 的快照
        snap.entities.push_back(handle);
        snap.entityDescs.push_back(eDesc);

        constructed++;
    }

    m_context->Logging->Info("[SceneConstruct] Scene '{}' constructed: {} entities (switchId={})",
                             sceneData.sceneName, constructed, captureSwitchId);

    // 检测是否为过期回调（用户已切换到另一个场景）
    if (m_sceneSwitchId != captureSwitchId) {
        m_context->Logging->Warn(
            "[EditorSceneManager] Stale scene construct detected for '{}' (switchId {} != {}), discarding {} entities",
            sceneData.sceneName, captureSwitchId, m_sceneSwitchId, constructed);
        // 释放刚创建的实体，避免污染当前场景
        for (auto it = m_entityDescs.begin(); it != m_entityDescs.end();) {
            it = m_entityDescs.erase(it);
        }
        m_sceneMgr->RemoveAllEntities();
        // 清空当前 Tab 的快照
        snap = SceneSnapshot{};
        return;
    }

    // 应用当前 Tab 的完整状态到全局管理器（天空盒、环境光等）
    ApplyTabState(m_activeTabIndex);

    // 恢复编辑器状态（从快照读取相机，无缓存时使用默认位置）
    RestoreSnapshotCamera(m_activeTabIndex);

    // 加载完成，清除加载标志
    m_isLoading = false;

    // 日志：场景构造完成后的最终状态
    auto &cam = DX12Engine::Renderer::CameraManager::GetInstance().GetMainCamera();
    m_context->Logging->Info("[SceneConstruct] Completed scene='{}': {} entities, "
                             "camera({:.1f},{:.1f},{:.1f}), openTabs={}",
                             sceneData.sceneName, constructed,
                             cam.Position.x, cam.Position.y, cam.Position.z,
                             m_openTabs.size());
}

// ========================================================================
// Tab 状态应用（Clear + Rebuild）
// ========================================================================

void EditorSceneManager::ApplyTabState(size_t index) {
    if (!m_initialized || !m_sceneMgr)
        return;
    if (index >= m_snapshots.size())
        return;

    const SceneSnapshot &snap = m_snapshots[index];

    // 1. Clear 全局管理器状态
    Renderer::SkyboxManager::GetInstance().ClearSkybox();
    m_sceneMgr->SetSkybox({});
    m_sceneMgr->SetEnvironment({});
    Renderer::WaterManager::GetInstance().SetEnvironmentMap({});

    // 2. Rebuild 全局管理器状态
    m_sceneMgr->SetSkybox(snap.skybox);
    m_sceneMgr->SetEnvironment(snap.environment);

    if (snap.skyboxTextureHandle.IsValid() && snap.skyboxGeometryHandle.IsValid()) {
        Renderer::SkyboxManager::GetInstance().SetSkybox(snap.skyboxTextureHandle, snap.skyboxGeometryHandle);

        // 将环境贴图注入 WaterManager
        auto cubeSRV = Renderer::SkyboxManager::GetInstance().GetCubeSRV();
        if (cubeSRV.ptr != 0) {
            Renderer::WaterManager::GetInstance().SetEnvironmentMap(cubeSRV);
        }
    }

    m_context->Logging->Info("[ApplyTabState] Completed [{}]: skybox={}, env={}, entities={}",
                             index, snap.HasSkybox(), snap.HasEnvironment(), snap.entities.size());
}

// ========================================================================
// SceneSnapshot 序列化
// ========================================================================

bool EditorSceneManager::SceneSnapshot::SaveTo(const std::filesystem::path &path) const {
    nlohmann::json j;

    // skybox
    if (!skybox.texture.empty()) {
        nlohmann::json sb;
        sb["texture"] = skybox.texture;
        if (!skybox.geometry.empty())
            sb["geometry"] = skybox.geometry;
        if (skybox.color.size() >= 4)
            sb["color"] = {skybox.color[0], skybox.color[1], skybox.color[2], skybox.color[3]};
        j["skybox"] = std::move(sb);
    }

    // environment
    if (!environment.ambientLight.empty()) {
        nlohmann::json env;
        env["ambientLight"] = environment.ambientLight;
        j["environment"] = std::move(env);
    }

    // camera
    nlohmann::json cam;
    cam["position"] = {cameraPosition.x, cameraPosition.y, cameraPosition.z};
    cam["forward"] = {cameraForward.x, cameraForward.y, cameraForward.z};
    j["camera"] = std::move(cam);

    // hierarchy (parentMap)
    if (!parentMap.empty()) {
        nlohmann::json pm;
        for (auto &[entityId, parentId] : parentMap)
            pm[entityId] = parentId;
        nlohmann::json hier;
        hier["parentMap"] = std::move(pm);
        j["hierarchy"] = std::move(hier);
    }

    // selection
    if (selectedEntity != 0) {
        j["selection"] = selectedEntity;
    }

    // entityDescs — 暂不序列化（由 .scene.json 提供完整数据）
    // 后续可扩展为增量缓存

    // 原子写入
    std::string tmpPath = path.string() + ".tmp";
    {
        std::ofstream ofs(tmpPath);
        if (!ofs.is_open())
            return false;
        ofs << j.dump(2);
        ofs.close();
    }
    std::error_code ec;
    std::filesystem::rename(tmpPath, path, ec);
    return !ec;
}

bool EditorSceneManager::SceneSnapshot::LoadFrom(const std::filesystem::path &path) {
    std::ifstream ifs(path);
    if (!ifs.is_open())
        return false;

    nlohmann::json j;
    try {
        ifs >> j;
    } catch (const nlohmann::json::parse_error &) {
        return false;
    }

    // skybox
    if (j.contains("skybox") && j["skybox"].is_object()) {
        auto &sb = j["skybox"];
        if (sb.contains("texture") && sb["texture"].is_string())
            skybox.texture = sb["texture"].get<std::string>();
        if (sb.contains("geometry") && sb["geometry"].is_string())
            skybox.geometry = sb["geometry"].get<std::string>();
        if (sb.contains("color") && sb["color"].is_array() && sb["color"].size() >= 4)
            skybox.color = {sb["color"][0].get<float>(), sb["color"][1].get<float>(),
                            sb["color"][2].get<float>(), sb["color"][3].get<float>()};
    }

    // environment
    if (j.contains("environment") && j["environment"].is_object()) {
        auto &env = j["environment"];
        if (env.contains("ambientLight") && env["ambientLight"].is_array())
            environment.ambientLight = env["ambientLight"].get<std::vector<float>>();
    }

    // camera
    if (j.contains("camera") && j["camera"].is_object()) {
        auto &cj = j["camera"];
        if (cj.contains("position") && cj["position"].is_array() && cj["position"].size() >= 3) {
            cameraPosition.x = cj["position"][0].get<float>();
            cameraPosition.y = cj["position"][1].get<float>();
            cameraPosition.z = cj["position"][2].get<float>();
        }
        if (cj.contains("forward") && cj["forward"].is_array() && cj["forward"].size() >= 3) {
            cameraForward.x = cj["forward"][0].get<float>();
            cameraForward.y = cj["forward"][1].get<float>();
            cameraForward.z = cj["forward"][2].get<float>();
        }
    }

    // hierarchy
    if (j.contains("hierarchy") && j["hierarchy"].is_object()) {
        auto &hj = j["hierarchy"];
        if (hj.contains("parentMap") && hj["parentMap"].is_object()) {
            for (auto &[entityId, parentId] : hj["parentMap"].items()) {
                if (parentId.is_string())
                    parentMap[entityId] = parentId.get<std::string>();
            }
        }
    }

    // selection
    if (j.contains("selection") && j["selection"].is_number_unsigned()) {
        selectedEntity = j["selection"].get<uint64_t>();
    }

    return true;
}
// ========================================================================

// ========================================================================
// 快照保存/恢复（替代旧 EditorStateFile 方法）
// ========================================================================

void EditorSceneManager::SaveCurrentSnapshotToDisk() {
    if (m_cacheRoot.empty() || m_activeScenePath.empty() || m_activeTabIndex >= m_snapshots.size())
        return;

    SceneSnapshot &snap = m_snapshots[m_activeTabIndex];

    // 从 CameraManager 读取当前相机状态写入快照
    auto &camera = DX12Engine::Renderer::CameraManager::GetInstance().GetMainCamera();
    snap.cameraPosition = camera.Position;
    snap.cameraForward = camera.Forward;

    // 写入磁盘
    std::string cachePath = (std::filesystem::path(m_cacheRoot) /
                             std::filesystem::path(m_activeScenePath).filename().replace_extension(".snapshot.json")).string();
    if (snap.SaveTo(cachePath)) {
        m_context->Logging->Info("[SaveSnapshot] Wrote to '{}'", cachePath);
    } else {
        m_context->Logging->Warn("[SaveSnapshot] FAILED to write '{}'", cachePath);
    }
}

void EditorSceneManager::RestoreSnapshotCamera(size_t index) {
    if (index >= m_snapshots.size()) {
        m_context->Logging->Warn("[RestoreCamera] index={} >= m_snapshots.size()={}, fallback to default", index, m_snapshots.size());
        ResetCameraToDefault();
        return;
    }

    const SceneSnapshot &snap = m_snapshots[index];

    // 检查快照是否有有效的相机数据
    if (!snap.HasCamera()) {
        m_context->Logging->Info("[RestoreCamera] No camera data in snapshot[{}] (pos=0,0,0), fallback to default", index);
        ResetCameraToDefault();
        return;
    }

    auto &camera = DX12Engine::Renderer::CameraManager::GetInstance().GetMainCamera();
    camera.Position = snap.cameraPosition;
    camera.Forward = snap.cameraForward;

    // 从 Forward 重建 Right 和 Up 基向量（龙书风格正交基）
    XMVECTOR L = XMLoadFloat3(&camera.Forward);
    L = XMVector3Normalize(L);
    XMStoreFloat3(&camera.Forward, L);

    XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMVECTOR R = XMVector3Cross(worldUp, L);
    R = XMVector3Normalize(R);
    XMStoreFloat3(&camera.Right, R);

    XMVECTOR U = XMVector3Cross(L, R);
    U = XMVector3Normalize(U);
    XMStoreFloat3(&camera.Up, U);

    DX12Engine::Renderer::CameraManager::GetInstance().UpdateMainCamera();
}

// ========================================================================
// 多 Tab 管理
// ========================================================================

void EditorSceneManager::DrawTabBar(ImTextureID viewportSRV, ImVec2 *outImageMin, ImVec2 *outImageMax) {
    if (m_openTabs.empty()) {
        // 无 Tab 时显示提示
        ImVec2 contentSize = ImGui::GetContentRegionAvail();
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        ImVec2 posMin = ImGui::GetCursorScreenPos();
        ImVec2 posMax = ImVec2(posMin.x + contentSize.x, posMin.y + contentSize.y);
        drawList->AddRectFilled(posMin, posMax, IM_COL32(40, 40, 50, 255));
        ImGui::Dummy(contentSize);

        const char *hint = EditorStrings::Get("viewport_empty_hint", "Open a scene file to start editing");
        ImVec2 textSize = ImGui::CalcTextSize(hint);
        ImVec2 textPos =
            ImVec2(posMin.x + (contentSize.x - textSize.x) * 0.5f, posMin.y + (contentSize.y - textSize.y) * 0.5f);
        drawList->AddText(textPos, IM_COL32(120, 120, 140, 255), hint);
        return;
    }

    // 使用 ImGui TabBar 绘制场景切换标签，图像在 Tab 内容区内渲染
    // 不设置 SetSelected，让 ImGui 内部管理选中状态
    ImGuiTabBarFlags tabBarFlags = ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_NoTabListScrollingButtons |
                                   ImGuiTabBarFlags_NoCloseWithMiddleMouseButton;

    if (ImGui::BeginTabBar("SceneTabs", tabBarFlags)) {
        for (size_t i = 0; i < m_openTabs.size(); i++) {
            auto &tab = m_openTabs[i];
            std::string label = tab.name;
            if (tab.dirty)
                label += " *";

            // 所有 Tab 均可关闭
            bool opened = true;
            bool isVisible = ImGui::BeginTabItem(label.c_str(), &opened, ImGuiTabItemFlags_NoPushId);

            if (!opened) {
                // 用户点击了 X 关闭按钮
                CloseTab(i);
                ImGui::EndTabItem();
                ImGui::EndTabBar();
                return;
            }

            if (isVisible && i != m_activeTabIndex) {
                // 用户点击了非活跃 Tab → 标记切换请求，延迟到帧结束后执行
                m_pendingSwitchTab = i;
            }

            // 在 Tab 内容区内渲染视口图像（只有当前活跃 Tab 的内容区可见）
            if (isVisible && viewportSRV != ImTextureID_Invalid) {
                ImVec2 contentSize = ImGui::GetContentRegionAvail();
                ImGui::Image(viewportSRV, contentSize);

                // 记录图像位置（供工具栏/ImGuizmo 定位使用）
                ImVec2 imageMin = ImGui::GetItemRectMin();
                ImVec2 imageMax = ImGui::GetItemRectMax();
                if (outImageMin) *outImageMin = imageMin;
                if (outImageMax) *outImageMax = imageMax;

                // 异步加载中，显示加载指示器（仅当活跃 Tab 快照为空时，避免全局 m_isLoading 误判）
                if (m_isLoading && i == m_activeTabIndex && m_snapshots.size() > i) {
                    const auto &snap = m_snapshots[i];
                    bool hasContent = !snap.entities.empty() || snap.HasSkybox();
                    if (!hasContent) {
                        ImDrawList *drawList = ImGui::GetWindowDrawList();
                        // 使用上面记录的 imageMin/imageMax
                        drawList->AddRectFilled(imageMin, imageMax, IM_COL32(0, 0, 0, 120));
                        const char *loadingText = "Loading...";
                        ImVec2 textSize = ImGui::CalcTextSize(loadingText);
                        ImVec2 textPos = ImVec2(
                            imageMin.x + (imageMax.x - imageMin.x - textSize.x) * 0.5f,
                            imageMin.y + (imageMax.y - imageMin.y - textSize.y) * 0.5f);
                        drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), loadingText);
                    }
                }
            }

            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void EditorSceneManager::ProcessPendingTabSwitch() {
    if (m_pendingSwitchTab == SIZE_MAX || m_pendingSwitchTab >= m_openTabs.size()) {
        m_pendingSwitchTab = SIZE_MAX;
        return;
    }

    auto &tab = m_openTabs[m_pendingSwitchTab];
    size_t switchIndex = m_pendingSwitchTab;
    m_pendingSwitchTab = SIZE_MAX;

    if (switchIndex == m_activeTabIndex)
        return; // 已在该 Tab 上

    // 保存当前场景的编辑器状态到快照
    SaveCurrentSnapshotToDisk();

    // 更新活跃 Tab 索引（不清除实体，实体通过 SceneTagComponent 标记）
    m_activeTabIndex = switchIndex;
    m_activeScenePath = tab.filePath;
    m_sceneFilePath = tab.filePath;
    m_dirty = tab.dirty;

    // 确保 m_snapshots 与 m_openTabs 对齐
    if (m_snapshots.size() <= m_activeTabIndex) {
        m_snapshots.resize(m_openTabs.size());
    }

    // 如果目标 Tab 无快照，尝试从缓存加载或创建默认空快照
    SceneSnapshot &snap = m_snapshots[m_activeTabIndex];
    if (snap.entities.empty() && !snap.HasSkybox()) {
        // 快照为空（默认构造），尝试从磁盘缓存加载
        if (!tab.filePath.empty()) {
            std::string cachePath = (std::filesystem::path(m_cacheRoot) /
                std::filesystem::path(tab.filePath).filename().replace_extension(".snapshot.json")).string();
            if (snap.LoadFrom(cachePath)) {
            }
        }
    }

    // 应用新 Tab 的完整状态到全局管理器（Clear + Rebuild 天空盒、环境光等）
    ApplyTabState(m_activeTabIndex);

    // 恢复编辑器状态（从快照读取相机，无缓存时使用默认位置）
    RestoreSnapshotCamera(m_activeTabIndex);

    // 广播 Tab 切换事件
    Event::MessageDispatcher::GetInstance()->PostEvent(
        Event::TabSwitchedEvent::StaticTypeHash, 0,
        tab.sceneId, Event::EventPriority::P2_Normal);
}

void EditorSceneManager::SetOnLoadSceneCallback(
    std::function<void(const std::string &, const std::filesystem::path &)> callback) {
    m_onLoadScene = std::move(callback);
}

void EditorSceneManager::CloseTab(size_t index) {
    if (index >= m_openTabs.size())
        return;

    auto &tab = m_openTabs[index];

    m_context->Logging->Info("[EditorSceneManager] Closing tab: '{}' (path: {})", tab.name, tab.filePath.string());

    // 保存被关闭 Tab 的快照到磁盘缓存（确保相机等状态持久化）
    if (m_snapshots.size() > index && !m_cacheRoot.empty() && !tab.filePath.empty()) {
        auto &snap = m_snapshots[index];
        std::string cachePath = (std::filesystem::path(m_cacheRoot) /
            std::filesystem::path(tab.filePath).filename().replace_extension(".snapshot.json")).string();
        if (snap.SaveTo(cachePath)) {
            m_context->Logging->Info("[CloseTab] Saved snapshot for '{}' to '{}'", tab.name, cachePath);
        }
    }

    // 如果关闭的是当前活跃 Tab，切换到上一个 Tab（实体已在 Registry 中，不清除）
    if (index == m_activeTabIndex && m_openTabs.size() > 1) {
        size_t newIndex = (index == 0) ? 1 : index - 1;
        // 保存当前场景的快照到磁盘
        SaveCurrentSnapshotToDisk();
        // 更新活跃 Tab（不清除实体，SceneTagComponent 过滤由 Builder 负责）
        m_activeTabIndex = newIndex;
        m_activeScenePath = m_openTabs[newIndex].filePath;
        m_sceneFilePath = m_openTabs[newIndex].filePath;
        m_dirty = m_openTabs[newIndex].dirty;
        m_context->Logging->Info("[EditorSceneManager] Switched to tab '{}' after closing current",
                                 m_openTabs[newIndex].name);
    }

    // 从列表中移除
    m_openTabs.erase(m_openTabs.begin() + index);

    // 清理该 Tab 的缓存状态
    if (m_snapshots.size() > index)
        m_snapshots.erase(m_snapshots.begin() + index);

    // 调整 m_activeTabIndex
    if (index < m_activeTabIndex) {
        m_activeTabIndex--;
    } else if (m_openTabs.empty()) {
        // 最后一个 Tab 被关闭 → 清除所有实体，回到空状态
        m_activeTabIndex = 0;
        m_sceneMgr->RemoveAllEntities();
        m_entityDescs.clear();
        // 清除全局管理器状态
        DX12Engine::Renderer::SkyboxManager::GetInstance().ClearSkybox();
        m_sceneMgr->SetSkybox({});
        m_sceneMgr->SetEnvironment({});
        m_activeScenePath.clear();
        m_sceneFilePath.clear();
        m_dirty = false;
        m_context->Logging->Info("[EditorSceneManager] Last tab closed, back to empty state");
    }
    // 如果关闭的是当前活跃 Tab 且已在 SwitchScene 中处理，无需额外操作
}

// ========================================================================
// 获取活跃 Tab 的实体列表
// ========================================================================

const std::vector<uint64_t> &EditorSceneManager::GetActiveEntities() const {
    static std::vector<uint64_t> s_empty;
    if (m_snapshots.size() <= m_activeTabIndex)
        return s_empty;
    return m_snapshots[m_activeTabIndex].entities;
}

uint64_t EditorSceneManager::GetActiveSceneId() const {
    if (m_openTabs.size() <= m_activeTabIndex)
        return 0;
    return m_openTabs[m_activeTabIndex].sceneId;
}