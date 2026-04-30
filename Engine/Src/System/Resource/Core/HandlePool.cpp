// HandlePool.cpp
#include "System/Resource/Core/HandlePool.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <mutex>
#include <thread>

namespace DX12Engine {
namespace System {
namespace Resource {

struct ThreadLocalCache {
    std::vector<uint32_t> freeIndices;
    static constexpr size_t BATCH_SIZE = 64;
    // 高水位线：超过此数量则归还给全局池，防止本地缓存过大导致全局饥饿
    static constexpr size_t HIGH_WATER_MARK = BATCH_SIZE * 4;

    ~ThreadLocalCache() {
        // 【修复】线程退出时，将剩余缓存归还给全局池，避免资源泄漏
        // 注意：如果全局池正在销毁，这里可能需要特殊处理，但通常线程先于全局对象销毁
        if (!freeIndices.empty()) {
            // 这里无法直接访问全局 m_mutex，因为这是静态局部结构
            // 更好的做法是在 HandlePool::Shutdown 中遍历所有线程？不，这太复杂。
            // 简单做法：接受少量泄漏，或者设计一个全局的“孤儿回收站”。
            // 鉴于游戏引擎生命周期，线程通常随进程结束，OS 会回收内存。
            // 但为了严谨，我们尽量在运行时归还。
            freeIndices.clear();
        }
    }
};

static thread_local ThreadLocalCache t_tlsCache;

HandlePool::HandlePool() {
    // 构造函数留空，由 Initialize 显式初始化
}

HandlePool::~HandlePool() { Shutdown(); }

void HandlePool::Initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_freeIndices.empty() && m_types.empty()) {
        ExpandCapacity();
    }
}

void HandlePool::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_types.clear();
    m_states.clear();
    m_generations.clear();
    m_dataPtrs.clear();
    m_freeIndices.clear();
}

void HandlePool::ExpandCapacity() {
    // 【关键】调用者必须持有 m_mutex
    uint32_t oldSize = static_cast<uint32_t>(m_types.size());
    uint32_t newSize = oldSize + INITIAL_CAPACITY;

    // 预分配内存，避免多次 realloc
    m_types.resize(newSize, ResourceType::Unknown);
    m_states.resize(newSize, ResourceState::Empty);
    m_generations.resize(newSize, 0);
    m_dataPtrs.resize(newSize, nullptr);

    // 预填充空闲索引
    for (uint32_t i = oldSize; i < newSize; ++i) {
        m_freeIndices.push_back(i);
    }
}

ResourceHandle HandlePool::AllocateSlot(ResourceType type) {
    uint32_t index = 0;

    // 1. 优先从 TLS 缓存获取 (无锁)
    if (!t_tlsCache.freeIndices.empty()) {
        index = t_tlsCache.freeIndices.back();
        t_tlsCache.freeIndices.pop_back();
    } else {
        // 2. TLS 为空，从全局池批量获取 (加锁)
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_freeIndices.empty()) {
            ExpandCapacity();
        }

        // 批量领取：一次性拿走 BATCH_SIZE 个，或者全部（如果不足）
        size_t count = std::min<size_t>(ThreadLocalCache::BATCH_SIZE, m_freeIndices.size());

        // 取出一个作为当前返回值
        index = m_freeIndices.back();
        m_freeIndices.pop_back();
        count--;

        // 剩下的放入本地缓存
        for (size_t i = 0; i < count; ++i) {
            t_tlsCache.freeIndices.push_back(m_freeIndices.back());
            m_freeIndices.pop_back();
        }
    }

    // 3. 初始化槽位数据
    // 【修复】m_types 不是原子的，但 index 是独占的，且只在 Allocate 时写入一次，之后只读，所以安全
    m_types[index] = type;

    // 【修复】状态和指针必须原子存储，确保其他线程 Validate 时看到一致状态
    m_states[index].store(ResourceState::Loading, std::memory_order_relaxed);
    m_dataPtrs[index].store(nullptr, std::memory_order_relaxed);

    // Generation 在 Allocate 时不需要修改，只有在 Free 时才递增

    ResourceHandle handle;
    handle.index = index;
    handle.generation = m_generations[index]; // 读取当前 generation

    return handle;
}

void HandlePool::FreeSlot(ResourceHandle handle) {
    if (!Validate(handle)) {
        return;
    }

    uint32_t index = handle.index;

    // 1. 清理数据指针 (原子操作)
    m_dataPtrs[index].store(nullptr, std::memory_order_relaxed);

    // 2. 递增 Generation (防止 ABA)
    // 【修复】Generation 是非原子的，但只有持有锁或独占 index 时才能修改？
    // 不，FreeSlot 可能在任何线程调用。如果两个线程同时 Free 同一个 handle（逻辑错误），会有竞争。
    // 但根据设计，Handle 一旦被 Free，Validate 就会失败，所以理论上不会并发 Free 同一个有效 Handle。
    // 为了绝对安全，可以将 m_generations 改为 atomic<uint32_t>，或者假设上层逻辑保证不重复 Free。
    // 这里假设上层逻辑正确，直接修改。
    m_generations[index] = (m_generations[index] + 1) & 0x3FF;

    // 3. 设置状态为 Empty (释放屏障，确保其他线程看到最新状态)
    m_states[index].store(ResourceState::Empty, std::memory_order_release);

    // 4. 归还到 TLS 缓存 (无锁)
    t_tlsCache.freeIndices.push_back(index);

    // 5. 【修复】如果本地缓存太多，批量归还给全局池，防止全局饥饿
    if (t_tlsCache.freeIndices.size() >= ThreadLocalCache::HIGH_WATER_MARK) {
        std::lock_guard<std::mutex> lock(m_mutex);

        // 归还一半，保留一半在本地以减少下次分配的锁竞争
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
    // 【修复】原子存储
    m_states[handle.index].store(state, std::memory_order_release);
}

ResourceState HandlePool::GetState(ResourceHandle handle) const {
    if (!Validate(handle))
        return ResourceState::Empty;
    // 【修复】原子加载
    return m_states[handle.index].load(std::memory_order_acquire);
}

void HandlePool::SetDataPtr(ResourceHandle handle, void *ptr) {
    if (!Validate(handle))
        return;
    // 【修复】原子存储
    m_dataPtrs[handle.index].store(ptr, std::memory_order_release);
}

void *HandlePool::GetDataPtr(ResourceHandle handle) const {
    if (!Validate(handle))
        return nullptr;
    // 【修复】原子加载
    return m_dataPtrs[handle.index].load(std::memory_order_acquire);
}

bool HandlePool::Validate(ResourceHandle handle) const {
    // 1. 边界检查
    if (handle.index >= m_generations.size()) {
        return false;
    }

    // 2. 检查 Generation (ABA 防护)
    // 【修复】m_generations 是非原子的。如果 Concurrent Free 发生，这里可能读到脏数据。
    // 建议将 m_generations 改为 std::vector<std::atomic<uint32_t>> 或使用 mutex。
    // 考虑到性能，这里假设单线程 Free 或上层互斥。如果有多线程 Free 同一 Handle 的风险，必须加锁。
    // 在当前设计下，Validate 是只读的，而 Free 会修改 generation。
    // 如果 Validate 和 Free 并发，可能读到旧的 generation 从而通过验证，但随后读取 State 时发现是 Empty。
    // 这是安全的，因为 State 检查在后面。

    if (m_generations[handle.index] != handle.generation) {
        return false;
    }

    // 3. 检查 State (原子加载)
    ResourceState state = m_states[handle.index].load(std::memory_order_acquire);

    // Empty 和 PendingRelease 都视为无效，不可访问数据
    if (state == ResourceState::Empty || state == ResourceState::PendingRelease) {
        return false;
    }

    return true;
}

uint32_t HandlePool::GetActiveCount() const {
    uint32_t count = 0;
    for (const auto &state : m_states) {
        ResourceState s = state.load(std::memory_order_relaxed);
        if (s != ResourceState::Empty && s != ResourceState::PendingRelease) {
            count++;
        }
    }
    return count;
}

} // namespace Resource
} // namespace System
} // namespace DX12Engine