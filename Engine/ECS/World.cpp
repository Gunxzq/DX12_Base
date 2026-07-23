#include "World.h"

using namespace DX12Engine;
using namespace DX12Engine::ECS;

// ========================================================================
// 构造/析构
// ========================================================================

World::~World() { Shutdown(); }

// ========================================================================
// 初始化/销毁
// ========================================================================

void World::Initialize() {
    if (m_registry)
        return;
    m_registry = std::make_unique<Registry>();
}

void World::Shutdown() {
    if (!m_registry)
        return;
    RemoveAllEntities();
    m_registry.reset();
}

// ========================================================================
// 实体生命周期
// ========================================================================

Entity World::CreateEntity() {
    if (!m_registry)
        return INVALID_ENTITY;
    return m_registry->CreateEntity();
}

std::vector<Entity> World::CreateEntities(uint32_t count) {
    std::vector<Entity> entities;
    if (!m_registry || count == 0)
        return entities;
    entities.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        entities.push_back(CreateEntity());
    }
    return entities;
}

void World::DestroyEntity(Entity entity) {
    if (!m_registry)
        return;
    m_registry->DestroyEntity(entity);
}

void World::RemoveAllEntities() {
    if (!m_registry)
        return;
    // 遍历所有有效实体并销毁
    for (auto entity : m_registry->AllEntities()) {
        m_registry->DestroyEntity(entity);
    }
}

bool World::IsValid(Entity entity) const { return m_registry && m_registry->IsValid(entity); }