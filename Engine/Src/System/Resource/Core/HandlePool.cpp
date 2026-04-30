// HandlePool.cpp
#include "System/Resource/Core/HandlePool.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <iostream>
#include <mutex>
#include <thread>

namespace DX12Engine {
namespace System {
namespace Resource {

struct ThreadLocalCache {
    std::vector<uint32_t> freeIndices;
    static constexpr size_t BATCH_SIZE = 64;
    static constexpr size_t HIGH_WATER_MARK = BATCH_SIZE * 4;

    ~ThreadLocalCache() {
        if (!freeIndices.empty()) {
            freeIndices.clear();
        }
    }
};

static thread_local ThreadLocalCache t_tlsCache;

HandlePool::HandlePool() {
}

HandlePool::~HandlePool() { Shutdown(); }

void HandlePool::Initialize() {
    std::cout << "[HandlePool] Initialize: acquiring lock..." << std::endl;
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cout << "[HandlePool] Initialize: lock acquired." << std::endl;

    if (!m_initialized || m_capacity == 0 || m_types.empty() || !m_states) {
        std::cout << "[HandlePool] Initialize: expanding capacity..." << std::endl;
        ExpandCapacity();
        std::cout << "[HandlePool] Initialize: capacity expanded to " << m_capacity << std::endl;
        m_initialized = true;
    }
    std::cout << "[HandlePool] Initialize: done, releasing lock." << std::endl;
}

void HandlePool::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_initialized = false;
    m_types.clear();
    m_freeIndices.clear();
    m_states.reset();
    m_dataPtrs.reset();
    m_generations.reset();
    m_capacity = 0;
}

void HandlePool::Preallocate(uint32_t targetCapacity) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    while (m_capacity < targetCapacity) {
        uint32_t oldCapacity = m_capacity;
        uint32_t newCapacity;
        if (oldCapacity == 0) {
            newCapacity = INITIAL_CAPACITY;
        } else if (oldCapacity < 65536) {
            newCapacity = oldCapacity * 2;
        } else {
            newCapacity = oldCapacity + 65536;
        }
        
        if (newCapacity < targetCapacity) {
            newCapacity = targetCapacity;
        }
        
        std::cout << "[HandlePool] Preallocate: " << oldCapacity << " -> " << newCapacity << std::endl;
        
        auto newStates = std::make_unique<std::atomic<ResourceState>[]>(newCapacity);
        auto newDataPtrs = std::make_unique<std::atomic<void *>[]>(newCapacity);
        auto newGenerations = std::make_unique<std::atomic<uint32_t>[]>(newCapacity);
        
        std::vector<ResourceType> newTypes;
        newTypes.reserve(newCapacity);
        
        size_t estimatedFreeCount = oldCapacity > 0 ? m_freeIndices.size() + (newCapacity - oldCapacity) : newCapacity;
        std::vector<uint32_t> newFreeIndices;
        newFreeIndices.reserve(estimatedFreeCount);

        for (uint32_t i = 0; i < oldCapacity; ++i) {
            newStates[i].store(m_states[i].load(std::memory_order_acquire), std::memory_order_release);
            newDataPtrs[i].store(m_dataPtrs[i].load(std::memory_order_acquire), std::memory_order_release);
            newGenerations[i].store(m_generations[i].load(std::memory_order_acquire), std::memory_order_release);
            newTypes.push_back(m_types[i]);
        }

        for (uint32_t i = oldCapacity; i < newCapacity; ++i) {
            newTypes.push_back(ResourceType::Unknown);
            newFreeIndices.push_back(i);
        }

        m_states = std::move(newStates);
        m_dataPtrs = std::move(newDataPtrs);
        m_generations = std::move(newGenerations);
        m_types = std::move(newTypes);
        
        for (uint32_t idx : m_freeIndices) {
            newFreeIndices.push_back(idx);
        }
        m_freeIndices.swap(newFreeIndices);

        m_capacity = newCapacity;
    }
}

void HandlePool::ExpandCapacity() {
    uint32_t oldCapacity = (!m_states || m_types.empty()) ? 0 : m_capacity;
    
    uint32_t newCapacity;
    if (oldCapacity == 0) {
        newCapacity = INITIAL_CAPACITY;
    } else if (oldCapacity < 65536) {
        newCapacity = oldCapacity * 2;
    } else {
        newCapacity = oldCapacity + 65536;
    }

    assert(newCapacity > oldCapacity && "ExpandCapacity: newCapacity overflow!");
    assert(newCapacity <= 10000000 && "ExpandCapacity: newCapacity too large!");
    (void)newCapacity;

    std::cout << "[HandlePool] ExpandCapacity: " << oldCapacity << " -> " << newCapacity << std::endl;

    auto newStates = std::make_unique<std::atomic<ResourceState>[]>(newCapacity);
    auto newDataPtrs = std::make_unique<std::atomic<void *>[]>(newCapacity);
    auto newGenerations = std::make_unique<std::atomic<uint32_t>[]>(newCapacity);
    
    std::vector<ResourceType> newTypes;
    newTypes.reserve(newCapacity);
    
    size_t estimatedFreeCount = oldCapacity > 0 ? m_freeIndices.size() + (newCapacity - oldCapacity) : newCapacity;
    std::vector<uint32_t> newFreeIndices;
    newFreeIndices.reserve(estimatedFreeCount);

    for (uint32_t i = 0; i < oldCapacity; ++i) {
        newStates[i].store(m_states[i].load(std::memory_order_acquire), std::memory_order_release);
        newDataPtrs[i].store(m_dataPtrs[i].load(std::memory_order_acquire), std::memory_order_release);
        newGenerations[i].store(m_generations[i].load(std::memory_order_acquire), std::memory_order_release);
        newTypes.push_back(m_types[i]);
    }

    for (uint32_t i = oldCapacity; i < newCapacity; ++i) {
        newTypes.push_back(ResourceType::Unknown);
        newFreeIndices.push_back(i);
    }

    m_states = std::move(newStates);
    m_dataPtrs = std::move(newDataPtrs);
    m_generations = std::move(newGenerations);
    m_types = std::move(newTypes);
    
    for (uint32_t idx : m_freeIndices) {
        newFreeIndices.push_back(idx);
    }
    m_freeIndices.swap(newFreeIndices);

    m_capacity = newCapacity;
}

ResourceHandle HandlePool::AllocateSlot(ResourceType type) {
    uint32_t index = 0;

    // 1. Try TLS cache first (lock-free)
    if (!t_tlsCache.freeIndices.empty()) {
        index = t_tlsCache.freeIndices.back();
        t_tlsCache.freeIndices.pop_back();

        if (index >= m_capacity || !m_initialized) {
            if (m_initialized) {
                t_tlsCache.freeIndices.push_back(index);
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_initialized || m_freeIndices.empty()) {
                if (!m_initialized) {
                    ResourceHandle invalidHandle;
                    invalidHandle.index = UINT32_MAX;
                    invalidHandle.generation = 0;
                    return invalidHandle;
                }
                ExpandCapacity();
            }
            index = m_freeIndices.back();
            m_freeIndices.pop_back();
        }
    } else {
        // 2. TLS empty, get from global pool (locked)
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_initialized) {
            ResourceHandle invalidHandle;
            invalidHandle.index = UINT32_MAX;
            invalidHandle.generation = 0;
            return invalidHandle;
        }

        if (m_freeIndices.empty()) {
            ExpandCapacity();
        }

        size_t count = std::min<size_t>(ThreadLocalCache::BATCH_SIZE, m_freeIndices.size());

        index = m_freeIndices.back();
        m_freeIndices.pop_back();
        count--;

        for (size_t i = 0; i < count; ++i) {
            t_tlsCache.freeIndices.push_back(m_freeIndices.back());
            m_freeIndices.pop_back();
        }
    }

    m_types[index] = type;
    m_states[index].store(ResourceState::Loading, std::memory_order_relaxed);
    m_dataPtrs[index].store(nullptr, std::memory_order_relaxed);

    ResourceHandle handle;
    handle.index = index;
    handle.generation = m_generations[index].load(std::memory_order_relaxed);

    return handle;
}

void HandlePool::FreeSlot(ResourceHandle handle) {
    if (!Validate(handle)) {
        return;
    }

    uint32_t index = handle.index;

    m_dataPtrs[index].store(nullptr, std::memory_order_relaxed);
    m_generations[index].fetch_add(1, std::memory_order_acq_rel);
    m_states[index].store(ResourceState::Empty, std::memory_order_release);

    t_tlsCache.freeIndices.push_back(index);

    if (t_tlsCache.freeIndices.size() >= ThreadLocalCache::HIGH_WATER_MARK) {
        std::lock_guard<std::mutex> lock(m_mutex);

        size_t returnCount = t_tlsCache.freeIndices.size() / 2;
        for (size_t i = 0; i < returnCount; ++i) {
            m_freeIndices.push_back(t_tlsCache.freeIndices.back());
            t_tlsCache.freeIndices.pop_back();
        }
    }
}

void HandlePool::SetState(ResourceHandle handle, ResourceState state) {
    if (!Validate(handle))
        return;
    m_states[handle.index].store(state, std::memory_order_release);
}

ResourceState HandlePool::GetState(ResourceHandle handle) const {
    if (!Validate(handle))
        return ResourceState::Empty;
    return m_states[handle.index].load(std::memory_order_acquire);
}

void HandlePool::SetDataPtr(ResourceHandle handle, void *ptr) {
    if (!Validate(handle))
        return;
    m_dataPtrs[handle.index].store(ptr, std::memory_order_release);
}

void *HandlePool::GetDataPtr(ResourceHandle handle) const {
    if (!Validate(handle))
        return nullptr;
    return m_dataPtrs[handle.index].load(std::memory_order_acquire);
}

bool HandlePool::Validate(ResourceHandle handle) const {
    if (!m_initialized || !m_states) {
        return false;
    }

    if (handle.index >= m_capacity) {
        return false;
    }

    uint32_t currentGen = m_generations[handle.index].load(std::memory_order_acquire);
    if (currentGen != handle.generation) {
        return false;
    }

    ResourceState state = m_states[handle.index].load(std::memory_order_acquire);

    if (state == ResourceState::Empty || state == ResourceState::PendingRelease) {
        return false;
    }

    currentGen = m_generations[handle.index].load(std::memory_order_acquire);
    if (currentGen != handle.generation) {
        return false;
    }

    return true;
}

uint32_t HandlePool::GetActiveCount() const {
    uint32_t count = 0;
    for (uint32_t i = 0; i < m_capacity; ++i) {
        ResourceState s = m_states[i].load(std::memory_order_relaxed);
        if (s != ResourceState::Empty && s != ResourceState::PendingRelease) {
            count++;
        }
    }
    return count;
}

} // namespace Resource
} // namespace System
} // namespace DX12Engine
