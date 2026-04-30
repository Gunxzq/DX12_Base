// ResourceManager.cpp
#include "System/Resource/ResourceManager.h"
#include <algorithm>
#include <cassert>
#include <iostream>

namespace DX12Engine {
namespace System {
namespace Resource {

// Global frame counter for simulating engine main loop frame growth
// In production, this should reference Engine::GetFrameCount()
static uint64_t s_globalFrameCount = 0;

ResourceManager &ResourceManager::GetInstance() {
    static ResourceManager instance;
    return instance;
}

void ResourceManager::Initialize() {
    if (m_initialized) {
        Shutdown();
    }

    m_handlePool.Initialize();
    m_dataPool.Initialize();

    m_pendingReleases.reserve(1024);
    s_globalFrameCount = 0;
    m_initialized = true;
}

void ResourceManager::Shutdown() {
    ForceCleanupForTesting();

    m_dataPool.Shutdown();
    m_handlePool.Shutdown();

    m_initialized = false;
}

void ResourceManager::ForceCleanupForTesting() {
    std::vector<PendingRelease> pending;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        pending = std::move(m_pendingReleases);
        m_pendingReleases.clear();
        m_pendingReleases.shrink_to_fit();
    }

    for (auto &pr : pending) {
        void *ptr = m_handlePool.GetDataPtr(pr.handle);
        if (ptr) {
            m_dataPool.Free(ptr);
        }
        m_handlePool.FreeSlot(pr.handle);
    }
}

// --- Passive Interface ---

void ResourceManager::Preallocate(uint32_t targetCapacity) { m_handlePool.Preallocate(targetCapacity); }

ResourceHandle ResourceManager::AllocateSlot(ResourceType type) { return m_handlePool.AllocateSlot(type); }

void ResourceManager::RegisterData(ResourceHandle handle, void *dataPtr, size_t size) {
    if (!m_handlePool.Validate(handle)) {
        assert(false && "RegisterData: Invalid Handle");
        return;
    }

    ResourceState currentState = m_handlePool.GetState(handle);
    if (currentState != ResourceState::Loading) {
        if (currentState == ResourceState::Ready) {

            return;
        }
        assert(false && "RegisterData: Handle is not in Loading state");
        return;
    }

#ifdef _DEBUG
    if (dataPtr) {
        bool inRange = m_dataPool.Contains(dataPtr);
        assert(inRange && "RegisterData: Pointer is not managed by DataPool!");
        (void)inRange;
    }
#endif

    m_handlePool.SetDataPtr(handle, dataPtr);
    m_handlePool.SetState(handle, ResourceState::Ready);
}

void *ResourceManager::GetData(ResourceHandle handle) const {
    if (!m_handlePool.Validate(handle)) {
        return nullptr;
    }

    if (m_handlePool.GetState(handle) != ResourceState::Ready) {
        return nullptr;
    }

    return m_handlePool.GetDataPtr(handle);
}

void ResourceManager::Release(ResourceHandle handle) {
    if (!m_handlePool.Validate(handle)) {
        return;
    }

    ResourceState state = m_handlePool.GetState(handle);

    if (state == ResourceState::PendingRelease || state == ResourceState::Empty) {
        return;
    }

    PendingRelease pr;
    pr.handle = handle;
    pr.releaseFrame = GetCurrentFrame() + 3;

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingReleases.push_back(pr);
    }

    m_handlePool.SetState(handle, ResourceState::PendingRelease);
}

void ResourceManager::Update(float deltaTime) {
    s_globalFrameCount++;
    uint64_t currentFrame = GetCurrentFrame();

    std::vector<PendingRelease> toRelease;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);

        auto it = m_pendingReleases.begin();
        while (it != m_pendingReleases.end()) {
            if (currentFrame >= it->releaseFrame) {
                toRelease.push_back(*it);
                it = m_pendingReleases.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto &pr : toRelease) {
        void *ptr = m_handlePool.GetDataPtr(pr.handle);
        if (ptr) {
            m_dataPool.Free(ptr);
        }
        m_handlePool.FreeSlot(pr.handle);
    }
}

uint32_t ResourceManager::GetActiveCount() const { return m_handlePool.GetActiveCount(); }

size_t ResourceManager::GetMemoryUsage() const { return m_dataPool.GetTotalAllocatedSize(); }

uint64_t ResourceManager::GetCurrentFrame() const { return s_globalFrameCount; }

} // namespace Resource
} // namespace System
} // namespace DX12Engine
