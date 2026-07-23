#include "GameSceneManager.h"
#include "Asset/IO/Loader/SceneLoader.h"
#include "Boot/GameContext.h"
#include "Core/SharedDataStore/SharedDataStore.h"
#include "ECS/Core/Registry.h"
#include "Event/EventTypes.h"
#include "Event/MessageDispatcher.h"
#include "Framework/SystemRegistry.h"
#include "Scene/SceneConstructor.h"

using namespace DX12Engine;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Event;
using namespace DX12Engine::Scheduler;

// ========================================================================
// 初始化/销毁
// ========================================================================

void GameSceneManager::Initialize(DX12Engine::Scene::SceneManager* sceneMgr,
                                  DX12Engine::Boot::GameContext* context) {
    if (m_initialized)
        return;

    m_sceneMgr = sceneMgr;
    m_context = context;
    m_initialized = true;
}

void GameSceneManager::Shutdown() {
    if (!m_initialized)
        return;

    m_sceneCtor.reset();
    m_sceneMgr = nullptr;
    m_context = nullptr;
    m_initialized = false;
}

// ========================================================================
// 场景构造系统注册
// ========================================================================

void GameSceneManager::RegisterSceneConstructSystem() {
    if (!m_initialized || !m_sceneMgr)
        return;

    SystemRegistry::Register(
        {.name = "SceneConstructSystem",
         .func =
             [this](const MessageContext &ctx) {
                 // payload: 高位 = 生成器类型, 低位 = 任务数据（sceneId）
                 uint32_t generatorType = static_cast<uint32_t>((ctx.payload >> 32) & 0xFFFFFFFF);
                 uint32_t sceneId = static_cast<uint32_t>(ctx.payload & 0xFFFFFFFF);

                 // 只处理 SceneConstructor 的完成事件
                 if (generatorType != DX12Engine::Scene::GENERATOR_TYPE_SCENE_CONSTRUCTOR)
                     return;

                 std::string storeKey = "scene_construct_" + std::to_string(sceneId);
                 m_context->Logging->Info("[SceneConstructSystem] Triggered (id={}, key={})", sceneId, storeKey);

                 auto &store = Core::SharedDataStore::GetInstance();
                 auto sceneData = store.GetTypedData<DX12Engine::Scene::SceneConstructData>(storeKey);
                 if (!sceneData) {
                     m_context->Logging->Error("[SceneConstructSystem] Scene data not found: {}", storeKey);
                     return;
                 }

                 m_context->Logging->Info("[SceneConstructSystem] Found data: entities={}, geoMap={}, matMap={}",
                                          sceneData->entities.size(), sceneData->geoMap.size(),
                                          sceneData->matMap.size());

                 // 构造所有实体
                 OnSceneConstructReady(*sceneData);

                 store.RemoveTypedData(storeKey);
             },
         .phase = TaskPhase::Render,
         .threadType = ThreadType::Main,
         .priority = TaskPriority::Normal,
         .interestedMessages = {static_cast<uint32_t>(Event::EventType::GeneratorTaskCompleteEvent)}});
}

// ========================================================================
// 异步场景加载
// ========================================================================

void GameSceneManager::LoadSceneAsync(const std::string& filePath) {
    if (!m_initialized || !m_context)
        return;

    m_context->Logging->Info("[GameSceneManager] Loading scene: {}", filePath);

    // Step 1: SceneLoader 解析 JSON → SceneDescription
    DX12Engine::Resource::SceneDescription desc;
    try {
        desc = DX12Engine::Resource::SceneLoader::LoadFromFile(std::filesystem::path(filePath));
    } catch (const std::exception &e) {
        m_context->Logging->Error("[GameSceneManager] Failed to load scene file: {}", e.what());
        return;
    }

    // Step 2: SceneConstructor 异步加载依赖
    // SceneConstructor 生命周期由 m_sceneCtor 持有，加载完成后自动释放
    m_sceneCtor = std::make_unique<DX12Engine::Scene::SceneConstructor>();
    m_sceneCtor->LoadScene(desc, m_context, DX12Engine::Resource::HeapTag::Default,
        [this](bool success) {
            m_context->Logging->Info("[GameSceneManager] Scene load {}", success ? "succeeded" : "failed");
            m_sceneCtor.reset();
        });
}

// ========================================================================
// 场景构造完成处理
// ========================================================================

void GameSceneManager::OnSceneConstructReady(const Scene::SceneConstructData& sceneData) {
    if (!m_sceneMgr)
        return;

    auto* registry = m_sceneMgr->GetRegistry();

    // 设置场景环境数据（天光盒、环境光）
    if (sceneData.skybox.has_value())
        m_sceneMgr->SetSkybox(*sceneData.skybox);
    if (sceneData.environment.has_value())
        m_sceneMgr->SetEnvironment(*sceneData.environment);

    uint32_t constructed = 0;
    for (const auto& eDesc : sceneData.entities) {
        // Step 1: 创建空实体
        uint64_t handle = m_sceneMgr->CreateEntity();
        auto entity = static_cast<ECS::Entity>(handle);

        // Step 2: 生成器创建组件
        Scene::SceneConstructor::ConstructEntity(entity, eDesc, sceneData.geoMap, sceneData.matMap, registry, m_context);

        // Step 3: 场景构造完成，继续下一个实体
        constructed++;
    }

    m_context->Logging->Info("[GameSceneManager] Scene '{}' constructed: {} entities",
                             sceneData.sceneName, constructed);
}