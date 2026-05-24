#pragma once
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <vector>

namespace DX12Engine::Resource {

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

    // 非虚方法（可直接继承）
    void Initialize(const InitConfig &config = {}) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_initialized || m_capacity == 0 || m_types.empty() || !m_states) {
            uint32_t targetCapacity = config.maxTotalHandles > 0 ? config.maxTotalHandles : INITIAL_CAPACITY;
            if (targetCapacity > m_capacity) {
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
        if (!Validate(handle))
            return;
        m_states[handle.index].store(state, std::memory_order_release);
    }

    StateEnum GetState(HandleType handle) const {
        if (!Validate(handle))
            return StateEnum::Empty;
        return m_states[handle.index].load(std::memory_order_acquire);
    }

    void SetDataPtr(HandleType handle, void *ptr) {
        if (!Validate(handle))
            return;
        m_dataPtrs[handle.index].store(ptr, std::memory_order_release);
    }

    void *GetDataPtr(HandleType handle) const {
        if (!Validate(handle))
            return nullptr;
        return m_dataPtrs[handle.index].load(std::memory_order_acquire);
    }

    bool Validate(HandleType handle) const {
        if (!m_initialized || !m_states)
            return false;
        if (handle.index >= m_capacity)
            return false;

        uint32_t currentGen = m_generations[handle.index].load(std::memory_order_acquire);
        if (currentGen != handle.generation)
            return false;

        StateEnum state = m_states[handle.index].load(std::memory_order_acquire);
        if (state == StateEnum::Empty || state == StateEnum::PendingRelease)
            return false;

        currentGen = m_generations[handle.index].load(std::memory_order_acquire);
        return currentGen == handle.generation;
    }

    uint32_t GetActiveCount() const {
        uint32_t count = 0;
        for (uint32_t i = 0; i < m_capacity; ++i) {
            StateEnum s = m_states[i].load(std::memory_order_relaxed);
            if (s != StateEnum::Empty && s != StateEnum::PendingRelease) {
                count++;
            }
        }
        return count;
    }

protected:
    mutable std::mutex m_mutex;
    std::vector<TypeEnum> m_types;
    std::unique_ptr<std::atomic<StateEnum>[]> m_states;
    std::unique_ptr<std::atomic<uint32_t>[]> m_generations;
    std::unique_ptr<std::atomic<void *>[]> m_dataPtrs;
    std::vector<uint32_t> m_freeIndices;
    uint32_t m_capacity = 0;
    bool m_initialized = false;

    void ExpandCapacity() {
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

        auto newStates = std::make_unique<std::atomic<StateEnum>[]>(newCapacity);
        auto newDataPtrs = std::make_unique<std::atomic<void *>[]>(newCapacity);
        auto newGenerations = std::make_unique<std::atomic<uint32_t>[]>(newCapacity);

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

            auto newStates = std::make_unique<std::atomic<StateEnum>[]>(newCapacity);
            auto newDataPtrs = std::make_unique<std::atomic<void *>[]>(newCapacity);
            auto newGenerations = std::make_unique<std::atomic<uint32_t>[]>(newCapacity);

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

} // namespace DX12Engine::Resource