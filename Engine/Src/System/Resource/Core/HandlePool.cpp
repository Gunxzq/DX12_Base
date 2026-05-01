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

    // 指向拥有此缓存的 HandlePool 实例
    HandlePool *owner = nullptr;

    ~ThreadLocalCache() {
        // 线程退出时，必须归还所有缓存的索引
        // 否则这些索引会永久丢失，导致 GetActiveCount 不归零
        if (owner && !freeIndices.empty()) {
            ReturnToGlobal();
        }
        freeIndices.clear();
    }

    void ReturnToGlobal() {
        if (!owner || !owner->m_initialized || freeIndices.empty()) {
            return;
        }
        // 直接归还到全局空闲列表（持有 owner 的锁）
        std::lock_guard<std::mutex> lock(owner->m_mutex);
        if (owner->m_initialized) {
            owner->m_freeIndices.insert(owner->m_freeIndices.end(), freeIndices.begin(), freeIndices.end());
            freeIndices.clear();
        }
    }
};

static thread_local ThreadLocalCache t_tlsCache;

HandlePool::HandlePool() {}

HandlePool::~HandlePool() { Shutdown(); }

void HandlePool::Initialize(const InitConfig &config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized || m_capacity == 0 || m_types.empty() || !m_states) {
        // 内部调用 Preallocate 预分配到目标容量
        uint32_t targetCapacity = config.maxTotalHandles > 0 ? config.maxTotalHandles : INITIAL_CAPACITY;
        if (targetCapacity > m_capacity) {
            Preallocate_Locked(targetCapacity);
        }
        m_initialized = true;
    }
}

void HandlePool::HarvestTLSCaches() {
    // 显式收割当前线程的 TLS 缓存中的索引
    // 由 ResourceManager::ForceCleanupForTesting() 调用，确保测试结束时回收所有借用的索引
    if (t_tlsCache.owner != this) {
        return;
    }

    if (!t_tlsCache.freeIndices.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_freeIndices.insert(m_freeIndices.end(), t_tlsCache.freeIndices.begin(), t_tlsCache.freeIndices.end());
        t_tlsCache.freeIndices.clear();
    }
    // 置空 owner 防止 ThreadLocalCache 析构时重复归还
    t_tlsCache.owner = nullptr;
}

void HandlePool::ForceResetForTesting() {
    // 强制重置池子状态，绕过所有检查
    // 仅用于测试环境：将所有槽位强制标记为 Empty，使 GetActiveCount() 返回 0
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_states) {
        return;
    }

    // 1. 把所有槽位强制标记为空闲态（绕过 Validate 检查）
    for (uint32_t i = 0; i < m_capacity; ++i) {
        m_states[i].store(ResourceState::Empty, std::memory_order_release);
        m_dataPtrs[i].store(nullptr, std::memory_order_relaxed);
        // generation 保留不变，避免已分配句柄的代数校验出问题
    }

    // 2. 重建空闲列表（所有槽位都可分配）
    m_freeIndices.clear();
    m_freeIndices.reserve(m_capacity);
    for (uint32_t i = 0; i < m_capacity; ++i) {
        m_freeIndices.push_back(i);
    }

    // 3. 收割 TLS 缓存并置空
    if (t_tlsCache.owner == this && !t_tlsCache.freeIndices.empty()) {
        m_freeIndices.insert(m_freeIndices.end(), t_tlsCache.freeIndices.begin(), t_tlsCache.freeIndices.end());
        t_tlsCache.freeIndices.clear();
    }
    if (t_tlsCache.owner == this) {
        t_tlsCache.owner = nullptr;
    }
}

void HandlePool::Shutdown() {
    // 先收割当前线程的 TLS 缓存
    HarvestTLSCaches();

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
    Preallocate_Locked(targetCapacity);
}

void HandlePool::Preallocate_Locked(uint32_t targetCapacity) {
    // REQUIRES: Caller must hold m_mutex.

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

ResourceHandle HandlePool::AllocateSlot(ResourceType type, uint8_t poolId) {
    uint32_t index = 0;

    // 绑定当前 HandlePool 实例到 TLS 缓存
    // 这样线程退出时析构函数能正确归还索引
    t_tlsCache.owner = this;

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
                    return ResourceHandle::Invalid();
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
            return ResourceHandle::Invalid();
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
    handle.poolId = poolId;

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
