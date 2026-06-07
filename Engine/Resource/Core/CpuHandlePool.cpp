#include "CpuHandlePool.h"
#include <mutex>

namespace DX12Engine::Resource {

struct TLSCache {
    std::vector<uint32_t> freeIndices;
    static constexpr size_t BATCH_SIZE = 64;
    static constexpr size_t HIGH_WATER_MARK = BATCH_SIZE * 4;
    CpuHandlePool *owner = nullptr;

    ~TLSCache() {
        if (owner && !freeIndices.empty()) {
            ReturnToGlobal();
        }
        freeIndices.clear();
    };
    void ReturnToGlobal() {
        if (!owner || !owner->m_initialized || freeIndices.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(owner->m_mutex);
        if (owner->m_initialized) {
            owner->m_freeIndices.insert(owner->m_freeIndices.end(), freeIndices.begin(), freeIndices.end());
            freeIndices.clear();
        }
    };
};

static thread_local TLSCache t_tlsCache;

CpuResourceHandle CpuHandlePool::AllocateSlot(CpuResourceType type, uint8_t poolId, CpuResourceState initialState) {
    uint32_t index = 0;

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
                    return CpuResourceHandle::Invalid();
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
            return CpuResourceHandle::Invalid();
        }

        if (m_freeIndices.empty()) {
            ExpandCapacity();
        }

        size_t count = std::min<size_t>(TLSCache::BATCH_SIZE, m_freeIndices.size());

        index = m_freeIndices.back();
        m_freeIndices.pop_back();
        count--;

        for (size_t i = 0; i < count; ++i) {
            t_tlsCache.freeIndices.push_back(m_freeIndices.back());
            m_freeIndices.pop_back();
        }
    }

    // snapshot 持有 shared_ptr 引用，防止 ExpandCapacity 替换数组时访问已释放内存
    auto states = m_states;
    auto dataPtrs = m_dataPtrs;
    auto generations = m_generations;

    m_types[index] = type;
    states[index].store(initialState, std::memory_order_relaxed);
    dataPtrs[index].store(nullptr, std::memory_order_relaxed);

    CpuResourceHandle handle;
    handle.index = index;
    handle.generation = generations[index].load(std::memory_order_relaxed);
    handle.poolId = poolId;

    return handle;
}

void CpuHandlePool::FreeSlot(CpuResourceHandle handle) {
    if (!Validate(handle)) {
        return;
    }

    uint32_t index = handle.index;

    m_dataPtrs[index].store(nullptr, std::memory_order_relaxed);
    m_generations[index].fetch_add(1, std::memory_order_acq_rel);
    m_states[index].store(CpuResourceState::Empty, std::memory_order_release);

    t_tlsCache.freeIndices.push_back(index);

    if (t_tlsCache.freeIndices.size() >= TLSCache::HIGH_WATER_MARK) {
        std::lock_guard<std::mutex> lock(m_mutex);

        size_t returnCount = t_tlsCache.freeIndices.size() / 2;
        for (size_t i = 0; i < returnCount; ++i) {
            m_freeIndices.push_back(t_tlsCache.freeIndices.back());
            t_tlsCache.freeIndices.pop_back();
        }
    }
}

} // namespace DX12Engine::Resource