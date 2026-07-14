// DataSlotPool.cpp — SharedDataStore 专用的槽位池实现
#include "DataSlotPool.h"
#include <mutex>

namespace DX12Engine::Core {

struct TLSCache {
    std::vector<uint32_t> freeIndices;
    static constexpr size_t BATCH_SIZE = 64;
    static constexpr size_t HIGH_WATER_MARK = BATCH_SIZE * 4;
    DataSlotPool *owner = nullptr;

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

DataSlotHandle DataSlotPool::AllocateSlot(uint8_t poolId, DataSlotState initialState) {
    uint32_t index = 0;

    t_tlsCache.owner = this;

    // 1. Try TLS cache first (lock-free)
    if (!t_tlsCache.freeIndices.empty()) {
        index = t_tlsCache.freeIndices.back();
        t_tlsCache.freeIndices.pop_back();

        if (index >= this->m_capacity || !this->m_initialized) {
            if (this->m_initialized) {
                t_tlsCache.freeIndices.push_back(index);
            }
            std::lock_guard<std::mutex> lock(this->m_mutex);
            if (!this->m_initialized || this->m_freeIndices.empty()) {
                if (!this->m_initialized) {
                    return DataSlotHandle::Invalid();
                }
                this->ExpandCapacity();
            }
            index = this->m_freeIndices.back();
            this->m_freeIndices.pop_back();
        }
    } else {
        // 2. TLS empty, get from global pool (locked)
        std::lock_guard<std::mutex> lock(this->m_mutex);

        if (!this->m_initialized) {
            return DataSlotHandle::Invalid();
        }

        if (this->m_freeIndices.empty()) {
            this->ExpandCapacity();
        }

        size_t count = std::min<size_t>(TLSCache::BATCH_SIZE, this->m_freeIndices.size());

        index = this->m_freeIndices.back();
        this->m_freeIndices.pop_back();
        count--;

        for (size_t i = 0; i < count; ++i) {
            t_tlsCache.freeIndices.push_back(this->m_freeIndices.back());
            this->m_freeIndices.pop_back();
        }
    }

    // snapshot 持有 shared_ptr 引用，防止 ExpandCapacity 替换数组时访问已释放内存
    auto states = this->m_states;
    auto dataPtrs = this->m_dataPtrs;
    auto generations = this->m_generations;

    states[index].store(initialState, std::memory_order_relaxed);
    dataPtrs[index].store(nullptr, std::memory_order_relaxed);

    DataSlotHandle handle;
    handle.index = index;
    handle.generation = generations[index].load(std::memory_order_relaxed);
    handle.poolId = poolId;

    return handle;
}

void DataSlotPool::FreeSlot(DataSlotHandle handle) {
    if (!this->Validate(handle)) {
        return;
    }

    uint32_t index = handle.index;

    this->m_dataPtrs[index].store(nullptr, std::memory_order_relaxed);
    this->m_generations[index].fetch_add(1, std::memory_order_acq_rel);
    this->m_states[index].store(DataSlotState::Empty, std::memory_order_release);

    t_tlsCache.freeIndices.push_back(index);

    if (t_tlsCache.freeIndices.size() >= TLSCache::HIGH_WATER_MARK) {
        std::lock_guard<std::mutex> lock(this->m_mutex);

        size_t returnCount = t_tlsCache.freeIndices.size() / 2;
        for (size_t i = 0; i < returnCount; ++i) {
            this->m_freeIndices.push_back(t_tlsCache.freeIndices.back());
            t_tlsCache.freeIndices.pop_back();
        }
    }
}

} // namespace DX12Engine::Core
