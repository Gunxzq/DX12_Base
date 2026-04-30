// DataPool.cpp
#include "System/Resource/Core/DataPool.h"
#include <algorithm>
#include <cassert>
#include <cstdlib> // malloc/free
#include <cstring> // memset

namespace DX12Engine {
namespace System {
namespace Resource {

// 【关键】线程本地 Arena
// 每个线程拥有独立的分配指针，分配时无需加锁
static thread_local DataPool::ThreadLocalArena t_tlsArena;

void DataPool::Initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_blocks.empty()) {
        AllocateRaw(BLOCK_SIZE, 16); // 预分配第一个大块
    }
}

void DataPool::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto &block : m_blocks) {
        free(block.start);
    }
    m_blocks.clear();

    // 清理 TLS 状态（虽然线程退出会自动清理，但显式重置更安全）
    t_tlsArena.currentPtr = nullptr;
    t_tlsArena.endPtr = nullptr;
    t_tlsArena.initialized = false;
}

void DataPool::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // 线性分配器的优势：重置只需将所有块的 used 归零
    for (auto &block : m_blocks) {
        block.used = 0;

// 【修复点 3】Debug 模式下清理内存，暴露野指针读取旧数据的问题
#ifdef _DEBUG
        // 使用 0xCD 填充，这是常见的未初始化/已释放内存标记
        memset(block.start, 0xCD, block.size);
#endif
    }

    // 重置 TLS Arena，迫使下一个分配重新从全局获取新的干净段
    t_tlsArena.currentPtr = nullptr;
    t_tlsArena.endPtr = nullptr;
    t_tlsArena.initialized = false;
}

void *DataPool::Allocate(size_t size, size_t alignment) {
    if (size == 0)
        return nullptr;

    // 1. 快速路径：尝试从 TLS Arena 分配 (无锁)
    if (t_tlsArena.initialized) {
        uintptr_t currentAddr = reinterpret_cast<uintptr_t>(t_tlsArena.currentPtr);
        uintptr_t alignedAddr = (currentAddr + alignment - 1) & ~(alignment - 1);
        size_t padding = alignedAddr - currentAddr;

        // 检查剩余空间
        if (t_tlsArena.currentPtr + padding + size <= t_tlsArena.endPtr) {
            void *ptr = reinterpret_cast<void *>(alignedAddr);
            t_tlsArena.currentPtr += padding + size;
            return ptr;
        }
    }

    // 2. 慢速路径：TLS 未初始化或空间不足，回退到全局分配
    return AllocateRaw(size, alignment);
}

void *DataPool::AllocateRaw(size_t size, size_t alignment) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 确保有可用的块
    if (m_blocks.empty()) {
        AllocateBlockInternal();
    }

    Block &currentBlock = m_blocks.back();

    // 计算对齐
    uintptr_t currentAddr = reinterpret_cast<uintptr_t>(currentBlock.start) + currentBlock.used;
    uintptr_t alignedAddr = (currentAddr + alignment - 1) & ~(alignment - 1);
    size_t padding = alignedAddr - currentAddr;

    // 检查当前块是否有足够空间容纳当前对象 + 潜在的 TLS Arena 预分配
    // 注意：这里先只检查当前对象，TLS 预分配是可选优化
    if (currentBlock.used + padding + size <= currentBlock.size) {
        void *ptr = reinterpret_cast<void *>(alignedAddr);
        currentBlock.used += padding + size;

        // 【修复点 1 & 2】优化 TLS 初始化逻辑，增加边界断言
        // 如果请求的大小适合放入 TLS (小于 ARENA_SIZE)，且 TLS 尚未初始化
        if (size < ThreadLocalArena::ARENA_SIZE && !t_tlsArena.initialized) {
            // 计算 TLS Arena 的起始位置（紧接在当前对象之后，并对齐）
            uintptr_t nextAddr = alignedAddr + size;
            uintptr_t nextAligned = (nextAddr + alignment - 1) & ~(alignment - 1);

            // 计算 TLS Arena 所需的总空间（从当前对象结束到 TLS 结束）
            size_t tlsSpaceNeeded = (nextAligned - currentAddr) + ThreadLocalArena::ARENA_SIZE;

            // 双重保险：检查剩余空间是否足够容纳一个完整的 TLS Arena
            if (currentBlock.used + (nextAligned - currentAddr) + ThreadLocalArena::ARENA_SIZE <= currentBlock.size) {

                // 【修复点 1】显式确保 endPtr 不超出 Block 物理边界
                char *tlsStart = reinterpret_cast<char *>(nextAligned);
                char *tlsEnd = tlsStart + ThreadLocalArena::ARENA_SIZE;
                char *blockEnd = reinterpret_cast<char *>(currentBlock.start) + currentBlock.size;

                assert(tlsEnd <= blockEnd && "TLS Arena exceeds block boundary!");

                t_tlsArena.currentPtr = tlsStart;
                t_tlsArena.endPtr = tlsEnd;
                t_tlsArena.initialized = true;

                // 更新全局块的 used，预留出 TLS 的空间
                currentBlock.used += (nextAligned - currentAddr) + ThreadLocalArena::ARENA_SIZE;
            }
        }

        return ptr;
    }

    // 当前块空间不足，分配新块
    AllocateBlockInternal();

    Block &newBlock = m_blocks.back();
    uintptr_t newAlignedAddr = (reinterpret_cast<uintptr_t>(newBlock.start) + alignment - 1) & ~(alignment - 1);
    size_t newPadding = newAlignedAddr - reinterpret_cast<uintptr_t>(newBlock.start);

    if (newPadding + size > newBlock.size) {
        assert(false && "Object too large for DataPool block");
        return nullptr;
    }

    void *ptr = reinterpret_cast<void *>(newAlignedAddr);
    newBlock.used = newPadding + size;

    // 同样，如果是小对象且 TLS 未初始化，尝试初始化 TLS
    if (size < ThreadLocalArena::ARENA_SIZE && !t_tlsArena.initialized) {
        uintptr_t nextAddr = newAlignedAddr + size;
        uintptr_t nextAligned = (nextAddr + alignment - 1) & ~(alignment - 1);

        size_t tlsSpaceNeeded =
            (nextAligned - reinterpret_cast<uintptr_t>(newBlock.start)) + ThreadLocalArena::ARENA_SIZE;

        if (newBlock.used + (nextAligned - reinterpret_cast<uintptr_t>(newBlock.start)) +
                ThreadLocalArena::ARENA_SIZE <=
            newBlock.size) {

            char *tlsStart = reinterpret_cast<char *>(nextAligned);
            char *tlsEnd = tlsStart + ThreadLocalArena::ARENA_SIZE;
            char *blockEnd = reinterpret_cast<char *>(newBlock.start) + newBlock.size;

            assert(tlsEnd <= blockEnd && "TLS Arena exceeds new block boundary!");

            t_tlsArena.currentPtr = tlsStart;
            t_tlsArena.endPtr = tlsEnd;
            t_tlsArena.initialized = true;

            // 修正 used 计算：从块起始到 TLS 结束的总偏移
            newBlock.used = (nextAligned - reinterpret_cast<uintptr_t>(newBlock.start)) + ThreadLocalArena::ARENA_SIZE;
        }
    }

    return ptr;
}

void DataPool::Free(void *ptr) {
    // 线性分配器不支持随机 Free。
    // 真正的回收由 Reset() 或 Shutdown() 处理。
    (void)ptr;
}

bool DataPool::Contains(void *ptr) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

    for (const auto &block : m_blocks) {
        uintptr_t start = reinterpret_cast<uintptr_t>(block.start);
        uintptr_t end = start + block.size;
        if (addr >= start && addr < end) {
            return true;
        }
    }
    return false;
}

size_t DataPool::GetTotalAllocatedSize() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t total = 0;
    for (const auto &block : m_blocks) {
        total += block.used;
    }
    return total;
}

// 内部辅助：分配一个新的 64MB 块
void DataPool::AllocateBlockInternal() {
    void *mem = malloc(BLOCK_SIZE);
    if (!mem) {
        throw std::bad_alloc();
    }

// Debug 模式下新分配的内存也初始化一下，避免脏数据
#ifdef _DEBUG
    memset(mem, 0xCD, BLOCK_SIZE);
#endif

    Block newBlock;
    newBlock.start = mem;
    newBlock.size = BLOCK_SIZE;
    newBlock.used = 0;

    m_blocks.push_back(newBlock);
}

} // namespace Resource
} // namespace System
} // namespace DX12Engine