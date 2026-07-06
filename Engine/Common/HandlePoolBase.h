// HandlePoolBase.h — 通用句柄池系统（模板基类）
#pragma once
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <vector>

namespace DX12Engine {

template <typename HandleType, typename StateEnum, typename TypeEnum> class HandlePoolBase {
public:
    static constexpr uint32_t INITIAL_CAPACITY = 4096;

    struct InitConfig {
        uint32_t maxTotalHandles = 0;
        uint32_t initialFreeListReserve = 0;
    };

    virtual ~HandlePoolBase() = default;

    virtual HandleType AllocateSlot(TypeEnum type, uint8_t poolId, StateEnum initialState) = 0;
    virtual void FreeSlot(HandleType handle) = 0;

    void Initialize(const InitConfig &config = {}) {
        std::lock_guard<std::mutex> lock(m_mutex);
        uint32_t cap = m_capacity.load(std::memory_order_relaxed);
        if (!m_initialized || cap == 0 || m_types.empty() || !m_states) {
            uint32_t targetCapacity = config.maxTotalHandles > 0 ? config.maxTotalHandles : INITIAL_CAPACITY;
            if (targetCapacity > cap) {
                Preallocate_Locked(targetCapacity);
            }
            m_initialized = true;
        }
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_initialized = false;
        m_types.clear();
        m_freeIndices.clear();
        m_states.reset();
        m_dataPtrs.reset();
        m_generations.reset();
        m_capacity = 0;
    }

    void Preallocate(uint32_t targetCapacity) {
        std::lock_guard<std::mutex> lock(m_mutex);
        Preallocate_Locked(targetCapacity);
    }

    void SetState(HandleType handle, StateEnum state) {
        auto states = m_states;
        auto generations = m_generations;
        uint32_t cap = m_capacity.load(std::memory_order_relaxed);
        if (!m_initialized || !states || handle.index >= cap)
            return;
        if (generations[handle.index].load(std::memory_order_acquire) != handle.generation)
            return;
        states[handle.index].store(state, std::memory_order_release);
    }

    StateEnum GetState(HandleType handle) const {
        auto states = m_states;
        auto generations = m_generations;
        uint32_t cap = m_capacity.load(std::memory_order_relaxed);
        if (!m_initialized || !states || handle.index >= cap)
            return StateEnum::Empty;
        if (generations[handle.index].load(std::memory_order_acquire) != handle.generation)
            return StateEnum::Empty;
        return states[handle.index].load(std::memory_order_acquire);
    }

    void SetDataPtr(HandleType handle, void *ptr) {
        auto dataPtrs = m_dataPtrs;
        auto generations = m_generations;
        uint32_t cap = m_capacity.load(std::memory_order_relaxed);
        if (!m_initialized || !dataPtrs || handle.index >= cap)
            return;
        if (generations[handle.index].load(std::memory_order_acquire) != handle.generation)
            return;
        dataPtrs[handle.index].store(ptr, std::memory_order_release);
    }

    void *GetDataPtr(HandleType handle) const {
        auto dataPtrs = m_dataPtrs;
        auto generations = m_generations;
        uint32_t cap = m_capacity.load(std::memory_order_relaxed);
        if (!m_initialized || !dataPtrs || handle.index >= cap)
            return nullptr;
        if (generations[handle.index].load(std::memory_order_acquire) != handle.generation)
            return nullptr;
        return dataPtrs[handle.index].load(std::memory_order_acquire);
    }

    bool Validate(HandleType handle) const {
        auto states = m_states;
        auto generations = m_generations;
        uint32_t cap = m_capacity.load(std::memory_order_relaxed);
        if (!m_initialized || !states || handle.index >= cap)
            return false;

        uint32_t currentGen = generations[handle.index].load(std::memory_order_acquire);
        if (currentGen != handle.generation)
            return false;

        StateEnum state = states[handle.index].load(std::memory_order_acquire);
        if (state == StateEnum::Empty || state == StateEnum::PendingRelease)
            return false;

        currentGen = generations[handle.index].load(std::memory_order_acquire);
        return currentGen == handle.generation;
    }

    uint32_t GetActiveCount() const {
        auto states = m_states;
        uint32_t cap = m_capacity.load(std::memory_order_relaxed);
        if (!states)
            return 0;
        uint32_t count = 0;
        for (uint32_t i = 0; i < cap; ++i) {
            StateEnum s = states[i].load(std::memory_order_relaxed);
            if (s != StateEnum::Empty && s != StateEnum::PendingRelease) {
                count++;
            }
        }
        return count;
    }

protected:
    mutable std::mutex m_mutex;
    std::vector<TypeEnum> m_types;
    std::shared_ptr<std::atomic<StateEnum>[]> m_states;
    std::shared_ptr<std::atomic<uint32_t>[]> m_generations;
    std::shared_ptr<std::atomic<void *>[]> m_dataPtrs;
    std::vector<uint32_t> m_freeIndices;
    std::atomic<uint32_t> m_capacity = 0;
    bool m_initialized = false;

    void ExpandCapacity() {
        uint32_t oldCapacity;
        if (!m_states || m_types.empty()) {
            oldCapacity = 0;
        } else {
            oldCapacity = m_capacity.load(std::memory_order_relaxed);
        }

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

        auto newStates = std::make_shared<std::atomic<StateEnum>[]>(newCapacity);
        auto newDataPtrs = std::make_shared<std::atomic<void *>[]>(newCapacity);
        auto newGenerations = std::make_shared<std::atomic<uint32_t>[]>(newCapacity);

        std::vector<TypeEnum> newTypes;
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
            newTypes.push_back(TypeEnum::Unknown);
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

    void Preallocate_Locked(uint32_t targetCapacity) {
        while (m_capacity.load(std::memory_order_relaxed) < targetCapacity) {
            uint32_t oldCapacity = m_capacity.load(std::memory_order_relaxed);
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

            auto newStates = std::make_shared<std::atomic<StateEnum>[]>(newCapacity);
            auto newDataPtrs = std::make_shared<std::atomic<void *>[]>(newCapacity);
            auto newGenerations = std::make_shared<std::atomic<uint32_t>[]>(newCapacity);

            std::vector<TypeEnum> newTypes;
            newTypes.reserve(newCapacity);

            size_t estimatedFreeCount =
                oldCapacity > 0 ? m_freeIndices.size() + (newCapacity - oldCapacity) : newCapacity;
            std::vector<uint32_t> newFreeIndices;
            newFreeIndices.reserve(estimatedFreeCount);

            for (uint32_t i = 0; i < oldCapacity; ++i) {
                newStates[i].store(m_states[i].load(std::memory_order_acquire), std::memory_order_release);
                newDataPtrs[i].store(m_dataPtrs[i].load(std::memory_order_acquire), std::memory_order_release);
                newGenerations[i].store(m_generations[i].load(std::memory_order_acquire), std::memory_order_release);
                newTypes.push_back(m_types[i]);
            }

            for (uint32_t i = oldCapacity; i < newCapacity; ++i) {
                newTypes.push_back(TypeEnum::Unknown);
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
};

} // namespace DX12Engine
