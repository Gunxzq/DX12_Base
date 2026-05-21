// DataPool.cpp
#include "DataPool.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <intrin.h>

using namespace DX12Engine::Boot;

namespace DX12Engine::Resource {

void DataPool::Initialize(const std::string &name, size_t totalSize, size_t alignment, MemoryStrategy strategy,
                          size_t blockSize) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_name = name;
    m_totalSize = totalSize;
    m_alignment = alignment;
    m_strategy = strategy;
    m_blockSize = blockSize;

    if (strategy == MemoryStrategy::FixedSizeBlock) {
        // Block 策略：预分配槽位数组和位图
        AllocateSlotsInternal();
    } else {
        // Linear/RingBuffer：分配初始块
        if (m_blocks.empty()) {
            AllocateBlockInternal();
        }
    }
}

void DataPool::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto &block : m_blocks) {
        if (block.start)
            free(block.start);
    }
    m_blocks.clear();
    m_slots.clear();
    m_bitmap.clear();
    m_name.clear();
    m_totalSize = 0;

    // Reset TLS state
    PoolThreadState *state = GetThreadPoolState(m_poolID);
    state->currentPtr = nullptr;
    state->endPtr = nullptr;
    state->initialized = false;
}

void DataPool::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_strategy == MemoryStrategy::FixedSizeBlock) {
        // Block 策略：重置所有槽位
        for (auto &slot : m_slots) {
            slot.free = true;
        }
        // 重置位图
        std::fill(m_bitmap.begin(), m_bitmap.end(), ~0ULL);
    } else {
        // Linear/RingBuffer：重置块使用量
        for (auto &block : m_blocks) {
            block.used = 0;
#ifdef _DEBUG
            memset(block.start, 0xCD, block.size);
#endif
        }
    }

    // Reset TLS Arena
    PoolThreadState *state = GetThreadPoolState(m_poolID);
    state->currentPtr = nullptr;
    state->endPtr = nullptr;
    state->initialized = false;
}

void *DataPool::Allocate(size_t size, size_t alignment) {
    if (size == 0)
        return nullptr;

    switch (m_strategy) {
    case MemoryStrategy::FixedSizeBlock:
        return AllocateBlock(size, alignment);
    case MemoryStrategy::Linear:
    case MemoryStrategy::RingBuffer:
    default:
        // Linear 策略使用 TLS 快速路径
        PoolThreadState *state = GetThreadPoolState(m_poolID);
        if (state->initialized) {
            void *ptr = AllocateFromTLSArena(size, alignment);
            if (ptr)
                return ptr;
        }
        return AllocateRaw(size, alignment);
    }
}

void *DataPool::AllocateRaw(size_t size, size_t alignment) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return AllocateRaw_Locked(size, alignment);
}

void *DataPool::AllocateFromTLSArena(size_t size, size_t alignment) {
    PoolThreadState *state = GetThreadPoolState(m_poolID);

    uintptr_t current = reinterpret_cast<uintptr_t>(state->currentPtr);
    uintptr_t end = reinterpret_cast<uintptr_t>(state->endPtr);

    uintptr_t alignedAddr = (current + alignment - 1) & ~(alignment - 1);
    size_t required = alignedAddr + size - current;

    if (required <= static_cast<size_t>(end - current)) {
        void *ptr = reinterpret_cast<void *>(alignedAddr);
        state->currentPtr = reinterpret_cast<char *>(alignedAddr + size);
        return ptr;
    }
    return nullptr;
}

// ========================================================================
// Linear 策略实现
// ========================================================================
void *DataPool::AllocateLinear(size_t size, size_t alignment) {
    if (m_blocks.empty()) {
        AllocateBlockInternal();
    }

    Block &currentBlock = m_blocks.back();
    PoolThreadState *state = GetThreadPoolState(m_poolID);

    uintptr_t currentAddr = reinterpret_cast<uintptr_t>(currentBlock.start) + currentBlock.used;
    uintptr_t alignedAddr = (currentAddr + alignment - 1) & ~(alignment - 1);
    size_t padding = alignedAddr - currentAddr;

    if (currentBlock.used + padding + size <= currentBlock.size) {
        void *ptr = reinterpret_cast<void *>(alignedAddr);
        currentBlock.used += padding + size;

        // Initialize TLS Arena if needed
        if (size < TLS_ARENA_SIZE && !state->initialized) {
            uintptr_t nextAddr = alignedAddr + size;
            uintptr_t nextAligned = (nextAddr + alignment - 1) & ~(alignment - 1);

            if (currentBlock.used + (nextAligned - currentAddr) + TLS_ARENA_SIZE <= currentBlock.size) {
                char *tlsStart = reinterpret_cast<char *>(nextAligned);
                state->currentPtr = tlsStart;
                state->endPtr = tlsStart + TLS_ARENA_SIZE;
                state->initialized = true;
                currentBlock.used += (nextAligned - currentAddr) + TLS_ARENA_SIZE;
            }
        }
        return ptr;
    }

    // Current block insufficient, allocate new block
    AllocateBlockInternal();
    Block &newBlock = m_blocks.back();
    uintptr_t newAlignedAddr = (reinterpret_cast<uintptr_t>(newBlock.start) + alignment - 1) & ~(alignment - 1);

    if (alignment + size > newBlock.size) {
        assert(false && "Object too large for DataPool block");
        return nullptr;
    }

    void *ptr = reinterpret_cast<void *>(newAlignedAddr);
    newBlock.used = alignment + size;

    // Initialize TLS for new block
    if (size < TLS_ARENA_SIZE && !state->initialized) {
        uintptr_t nextAddr = newAlignedAddr + size;
        uintptr_t nextAligned = (nextAddr + alignment - 1) & ~(alignment - 1);

        if (newBlock.used + (nextAligned - reinterpret_cast<uintptr_t>(newBlock.start)) + TLS_ARENA_SIZE <=
            newBlock.size) {
            char *tlsStart = reinterpret_cast<char *>(nextAligned);
            state->currentPtr = tlsStart;
            state->endPtr = tlsStart + TLS_ARENA_SIZE;
            state->initialized = true;
            newBlock.used = (nextAligned - reinterpret_cast<uintptr_t>(newBlock.start)) + TLS_ARENA_SIZE;
        }
    }
    return ptr;
}

void *DataPool::AllocateRaw_Locked(size_t size, size_t alignment) {
    // REQUIRES: Caller must hold m_mutex.

    if (m_blocks.empty()) {
        AllocateBlockInternal();
    }

    Block &currentBlock = m_blocks.back();
    PoolThreadState *state = GetThreadPoolState(m_poolID);

    uintptr_t currentAddr = reinterpret_cast<uintptr_t>(currentBlock.start) + currentBlock.used;
    uintptr_t alignedAddr = (currentAddr + alignment - 1) & ~(alignment - 1);
    size_t padding = alignedAddr - currentAddr;

    if (currentBlock.used + padding + size <= currentBlock.size) {
        void *ptr = reinterpret_cast<void *>(alignedAddr);
        currentBlock.used += padding + size;

        // Initialize TLS Arena
        if (size < TLS_ARENA_SIZE && !state->initialized) {
            uintptr_t nextAddr = alignedAddr + size;
            uintptr_t nextAligned = (nextAddr + alignment - 1) & ~(alignment - 1);

            if (currentBlock.used + (nextAligned - currentAddr) + TLS_ARENA_SIZE <= currentBlock.size) {
                char *tlsStart = reinterpret_cast<char *>(nextAligned);
                state->currentPtr = tlsStart;
                state->endPtr = tlsStart + TLS_ARENA_SIZE;
                state->initialized = true;
                currentBlock.used += (nextAligned - currentAddr) + TLS_ARENA_SIZE;
            }
        }
        return ptr;
    }

    // Allocate new block
    AllocateBlockInternal();
    Block &newBlock = m_blocks.back();
    uintptr_t newAlignedAddr = (reinterpret_cast<uintptr_t>(newBlock.start) + alignment - 1) & ~(alignment - 1);

    if (alignment + size > newBlock.size) {
        assert(false && "Object too large for DataPool block");
        return nullptr;
    }

    void *ptr = reinterpret_cast<void *>(newAlignedAddr);
    newBlock.used = alignment + size;

    // Initialize TLS for new block
    if (size < TLS_ARENA_SIZE && !state->initialized) {
        uintptr_t nextAddr = newAlignedAddr + size;
        uintptr_t nextAligned = (nextAddr + alignment - 1) & ~(alignment - 1);

        if (newBlock.used + (nextAligned - reinterpret_cast<uintptr_t>(newBlock.start)) + TLS_ARENA_SIZE <=
            newBlock.size) {
            char *tlsStart = reinterpret_cast<char *>(nextAligned);
            state->currentPtr = tlsStart;
            state->endPtr = tlsStart + TLS_ARENA_SIZE;
            state->initialized = true;
            newBlock.used = (nextAligned - reinterpret_cast<uintptr_t>(newBlock.start)) + TLS_ARENA_SIZE;
        }
    }
    return ptr;
}

// ========================================================================
// Block 策略实现（固定大小块 + 位图管理）
// ========================================================================
void *DataPool::AllocateBlock(size_t size, size_t alignment) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return AllocateBlock_Locked(size, alignment);
}

void *DataPool::AllocateBlock_Locked(size_t size, size_t alignment) {
    // 验证请求大小
    if (size > m_blockSize) {
        assert(false && "Allocation size exceeds block size");
        return nullptr;
    }

    // 找到空闲槽位
    int slotIdx = FindFreeSlot_Locked();
    if (slotIdx < 0) {
        // 没有空闲槽，需要扩展
        AllocateSlotsInternal();
        slotIdx = FindFreeSlot_Locked();
        if (slotIdx < 0) {
            return nullptr; // 分配失败
        }
    }

    MarkSlotUsed(slotIdx);
    char *ptr = reinterpret_cast<char *>(m_blocks[0].start) + static_cast<size_t>(slotIdx) * m_blockSize;

    // 对齐调整
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t aligned = (addr + alignment - 1) & ~(alignment - 1);
    size_t alignmentOffset = aligned - addr;

    if (alignmentOffset + size > m_blockSize) {
        // 对齐后超出块范围，尝试下一块
        MarkSlotFree(slotIdx);
        int nextSlot = slotIdx + 1;
        if (static_cast<size_t>(nextSlot) < m_slots.size()) {
            MarkSlotUsed(nextSlot);
            ptr = reinterpret_cast<char *>(m_blocks[0].start) + static_cast<size_t>(nextSlot) * m_blockSize;
            aligned = (reinterpret_cast<uintptr_t>(ptr) + alignment - 1) & ~(alignment - 1);
        } else {
            return nullptr;
        }
    }

    return reinterpret_cast<void *>(aligned);
}

int DataPool::FindFreeSlot_Locked() {
    for (size_t i = 0; i < m_bitmap.size(); ++i) {
        if (m_bitmap[i] != 0) {
            // 找到有空闲位的字
            unsigned long bit;
            _BitScanForward64(&bit, m_bitmap[i]); // 找最低位的1
            int slotIdx = static_cast<int>(i * 64 + bit);
            if (static_cast<size_t>(slotIdx) < m_slots.size()) {
                return slotIdx;
            }
        }
    }
    return -1;
}

void DataPool::MarkSlotUsed(int index) {
    size_t wordIdx = index / 64;
    int bit = index % 64;
    m_bitmap[wordIdx] &= ~(1ULL << bit);
    m_slots[index].free = false;
}

void DataPool::MarkSlotFree(int index) {
    size_t wordIdx = index / 64;
    int bit = index % 64;
    m_bitmap[wordIdx] |= (1ULL << bit);
    m_slots[index].free = true;
}

void DataPool::AllocateSlotsInternal() {
    if (m_blockSize == 0)
        return;

    size_t slotCount = m_totalSize / m_blockSize;
    size_t oldCount = m_slots.size();

    // 扩展槽位数组
    m_slots.resize(slotCount);
    for (size_t i = oldCount; i < slotCount; ++i) {
        m_slots[i].free = true;
        m_slots[i].size = m_blockSize;
    }

    // 扩展位图（每64个槽位一个 uint64_t）
    size_t bitmapWords = (slotCount + 63) / 64;
    m_bitmap.resize(bitmapWords, ~0ULL); // 全1表示全部空闲

    // 分配底层内存块
    size_t totalMemSize = slotCount * m_blockSize;
    size_t alignedSize = (totalMemSize + 4096 - 1) & ~(4096 - 1); // 页对齐

    void *mem = malloc(alignedSize);
    if (!mem) {
        throw std::bad_alloc();
    }

#ifdef _DEBUG
    memset(mem, 0xCD, alignedSize);
#endif

    Block newBlock;
    newBlock.start = mem;
    newBlock.size = alignedSize;
    newBlock.used = 0;
    m_blocks.push_back(newBlock);
}

// ========================================================================
// 通用接口实现
// ========================================================================
void DataPool::Free(void *ptr) {
    if (!ptr)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_strategy == MemoryStrategy::FixedSizeBlock) {
        // 计算槽位索引
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t blockStart = reinterpret_cast<uintptr_t>(m_blocks[0].start);
        size_t offset = addr - blockStart;
        int slotIdx = static_cast<int>(offset / m_blockSize);

        if (slotIdx >= 0 && static_cast<size_t>(slotIdx) < m_slots.size()) {
            MarkSlotFree(slotIdx);
        }
    }
    // Linear/RingBuffer 不支持单独 Free
}

bool DataPool::Contains_Locked(void *ptr) const {
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

size_t DataPool::GetTotalAllocatedSize_Locked() const {
    if (m_strategy == MemoryStrategy::FixedSizeBlock) {
        size_t used = 0;
        for (const auto &slot : m_slots) {
            if (!slot.free)
                used += slot.size;
        }
        return used;
    } else {
        size_t total = 0;
        for (const auto &block : m_blocks) {
            total += block.used;
        }
        return total;
    }
}

bool DataPool::Contains(void *ptr) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return Contains_Locked(ptr);
}

size_t DataPool::GetTotalAllocatedSize() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return GetTotalAllocatedSize_Locked();
}

size_t DataPool::GetFreeBlockCount() const {
    if (m_strategy != MemoryStrategy::FixedSizeBlock)
        return 0;
    size_t free = 0;
    for (const auto &slot : m_slots) {
        if (slot.free)
            ++free;
    }
    return free;
}

size_t DataPool::GetUsedBlockCount() const {
    if (m_strategy != MemoryStrategy::FixedSizeBlock)
        return 0;
    size_t used = 0;
    for (const auto &slot : m_slots) {
        if (!slot.free)
            ++used;
    }
    return used;
}

void DataPool::AllocateBlockInternal() {
    void *mem = malloc(BLOCK_SIZE);
    if (!mem) {
        throw std::bad_alloc();
    }

#ifdef _DEBUG
    memset(mem, 0xCD, BLOCK_SIZE);
#endif

    Block newBlock;
    newBlock.start = mem;
    newBlock.size = BLOCK_SIZE;
    newBlock.used = 0;
    m_blocks.push_back(newBlock);
}

} // namespace DX12Engine::Resource
