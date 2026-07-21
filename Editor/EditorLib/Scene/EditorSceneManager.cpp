#include "EditorSceneManager.h"
#include "Asset/IO/Loader/SceneDescription.h"
#include "Boot/GameContext.h"
#include "Core/SharedDataStore/SharedDataStore.h"
#include "ECS/Core/Components/Name.h"
#include "ECS/Core/Components/Render.h"
#include "ECS/Core/Components/Tags.h"
#include "ECS/Core/Components/Transform.h"
#include "ECS/Core/Registry.h"
#include "Event/EventTypes.h"
#include "Event/MessageDispatcher.h"
#include "Framework/SystemRegistry.h"
#include "Scene/SceneConstructor.h"
#include <filesystem>
#include <fstream>

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
    m_initialized = true;
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

    // 注意：默认场景（天空盒）由 Editor::Initialize() 在启动时加载，
    // NewScene 不清除异步加载的天空盒数据，仅清除实体列表
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
            const auto& t = newDesc.transform.value();
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
// 场景构造完成处理
// ========================================================================

void EditorSceneManager::OnSceneConstructReady(const Scene::SceneConstructData &sceneData) {
    if (!m_sceneMgr)
        return;

    auto *registry = m_sceneMgr->GetRegistry();
    auto *geoMgr = m_context->GeometryResourceManager;

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

        // Step 3: 注册到管理器
        m_sceneMgr->RegisterEntity(handle);

        constructed++;
    }

    m_context->Logging->Info("[EditorSceneManager] Scene '{}' constructed: {} entities", sceneData.sceneName,
                             constructed);
}