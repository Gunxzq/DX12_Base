#include "SceneManager.h"
#include "Boot/GameContext.h"
#include "ECS/World.h"
#include "Renderer/Effects/AO/AmbientOcclusionManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/LightManager/LightManager.h"
#include "Renderer/Scene/ReflectionProbeManager/ReflectionProbeManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"

using namespace DX12Engine;
using namespace DX12Engine::Scene;

// ========================================================================
// 构造/析构
// ========================================================================

SceneManager::~SceneManager() { Shutdown(); }

// ========================================================================
// 初始化/销毁
// ========================================================================

void SceneManager::Initialize(ECS::World *world) {
    if (m_world)
        return;

    m_world = world;
    m_state = SceneState::None;

    // 创建 RenderScene 渲染上下文容器（不设指针，由 Bootstrap 后续配置）
    m_renderScene = std::make_unique<RenderScene>();
}

void SceneManager::Shutdown() {
    if (!m_world)
        return;

    // 销毁所有实体
    m_world->RemoveAllEntities();

    m_world = nullptr;
    m_state = SceneState::None;
}

// ========================================================================
// 实体管理（委托给 World）
// ========================================================================

uint64_t SceneManager::CreateEntity() {
    if (!m_world)
        return UINT64_MAX;

    auto entity = m_world->CreateEntity();
    return static_cast<uint64_t>(entity);
}

std::vector<uint64_t> SceneManager::CreateEntities(uint32_t count) {
    std::vector<uint64_t> handles;
    if (!m_world || count == 0)
        return handles;

    handles.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        handles.push_back(CreateEntity());
    }
    return handles;
}

void SceneManager::RemoveEntity(uint64_t entity) {
    if (!m_world)
        return;

    m_world->DestroyEntity(static_cast<ECS::Entity>(entity));
}

void SceneManager::RemoveAllEntities() {
    if (!m_world)
        return;

    m_world->RemoveAllEntities();
}

// ========================================================================
// 环境状态
// ========================================================================

void SceneManager::SetSkybox(const Resource::SkyboxDesc &skybox) { m_skybox = skybox; }

void SceneManager::SetEnvironment(const Resource::EnvironmentDesc &env) { m_environment = env; }

// ========================================================================
// 场景生命周期
// ========================================================================

void SceneManager::PrepareSceneSwitch(const std::string &newSceneName, SceneTransition transition) {
    if (!m_world)
        return;

    // 清除所有实体
    m_world->RemoveAllEntities();

    // 更新场景名称和状态
    m_sceneName = newSceneName;
    m_state = SceneState::Active;
}