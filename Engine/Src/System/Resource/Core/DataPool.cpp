// DataPool.cpp
#include "System/Resource/Core/DataPool.h"
#include <algorithm>
#include <cassert>
#include <cstdlib> // malloc/free
#include <cstring> // memset
#include <iostream>

namespace DX12Engine {
namespace System {
namespace Resource {

// Thread local Arena - each thread has its own allocation pointer
// No lock needed for allocations
static thread_local DataPool::ThreadLocalArena t_tlsArena;

void DataPool::Initialize() {
    std::cout << "[DataPool] Initialize: acquiring lock..." << std::endl;
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cout << "[DataPool] Initialize: lock acquired." << std::endl;

    if (m_blocks.empty()) {
        std::cout << "[DataPool] Initialize: allocating first block..." << std::endl;
        AllocateRaw_Locked(BLOCK_SIZE, 16); // Pre-allocate first block
        std::cout << "[DataPool] Initialize: first block allocated." << std::endl;
    }
    std::cout << "[DataPool] Initialize: done, releasing lock." << std::endl;
}

void DataPool::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto &block : m_blocks) {
        free(block.start);
    }
    m_blocks.clear();

    // Reset TLS state
    t_tlsArena.currentPtr = nullptr;
    t_tlsArena.endPtr = nullptr;
    t_tlsArena.initialized = false;
}

void DataPool::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto &block : m_blocks) {
        block.used = 0;

#ifdef _DEBUG
        // Fill with 0xCD to detect uninitialized memory access
        memset(block.start, 0xCD, block.size);
#endif
    }

    // Reset TLS Arena
    t_tlsArena.currentPtr = nullptr;
    t_tlsArena.endPtr = nullptr;
    t_tlsArena.initialized = false;
}

void *DataPool::Allocate(size_t size, size_t alignment) {
    if (size == 0)
        return nullptr;

    // TLS fast path - no lock needed
    if (t_tlsArena.initialized) {
        uintptr_t current = reinterpret_cast<uintptr_t>(t_tlsArena.currentPtr);
        uintptr_t end = reinterpret_cast<uintptr_t>(t_tlsArena.endPtr);

        uintptr_t alignedAddr = (current + alignment - 1) & ~(alignment - 1);
        size_t required = alignedAddr + size - current;

        if (required <= static_cast<size_t>(end - current)) {
            void *ptr = reinterpret_cast<void *>(alignedAddr);
            t_tlsArena.currentPtr = reinterpret_cast<char *>(alignedAddr + size);
            return ptr;
        }
        // TLS exhausted, fallback to global allocation
    }

    return AllocateRaw(size, alignment);
}

void *DataPool::AllocateRaw(size_t size, size_t alignment) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return AllocateRaw_Locked(size, alignment);
}

void *DataPool::AllocateRaw_Locked(size_t size, size_t alignment) {
    // REQUIRES: Caller must hold m_mutex.

    if (m_blocks.empty()) {
        AllocateBlockInternal();
    }

    Block &currentBlock = m_blocks.back();

    uintptr_t currentAddr = reinterpret_cast<uintptr_t>(currentBlock.start) + currentBlock.used;
    uintptr_t alignedAddr = (currentAddr + alignment - 1) & ~(alignment - 1);
    size_t padding = alignedAddr - currentAddr;

    if (currentBlock.used + padding + size <= currentBlock.size) {
        void *ptr = reinterpret_cast<void *>(alignedAddr);
        currentBlock.used += padding + size;

        // Initialize TLS Arena if needed
        if (size < ThreadLocalArena::ARENA_SIZE && !t_tlsArena.initialized) {
            uintptr_t nextAddr = alignedAddr + size;
            uintptr_t nextAligned = (nextAddr + alignment - 1) & ~(alignment - 1);

            size_t tlsSpaceNeeded = (nextAligned - currentAddr) + ThreadLocalArena::ARENA_SIZE;

            if (currentBlock.used + (nextAligned - currentAddr) + ThreadLocalArena::ARENA_SIZE <= currentBlock.size) {
                char *tlsStart = reinterpret_cast<char *>(nextAligned);
                char *tlsEnd = tlsStart + ThreadLocalArena::ARENA_SIZE;
                char *blockEnd = reinterpret_cast<char *>(currentBlock.start) + currentBlock.size;

                assert(tlsEnd <= blockEnd && "TLS Arena exceeds block boundary!");

                t_tlsArena.currentPtr = tlsStart;
                t_tlsArena.endPtr = tlsEnd;
                t_tlsArena.initialized = true;

                currentBlock.used += (nextAligned - currentAddr) + ThreadLocalArena::ARENA_SIZE;
            }
        }

        return ptr;
    }

    // Current block insufficient, allocate new block
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

    // Initialize TLS for new block
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

            newBlock.used = (nextAligned - reinterpret_cast<uintptr_t>(newBlock.start)) + ThreadLocalArena::ARENA_SIZE;
        }
    }

    return ptr;
}

void DataPool::Free(void *ptr) {
    // Linear allocator does not support random Free
    // Actual recycling is handled by Reset() or Shutdown()
    (void)ptr;
}

// REQUIRES: Caller must hold m_mutex.
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

// REQUIRES: Caller must hold m_mutex.
size_t DataPool::GetTotalAllocatedSize_Locked() const {
    size_t total = 0;
    for (const auto &block : m_blocks) {
        total += block.used;
    }
    return total;
}

bool DataPool::Contains(void *ptr) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return Contains_Locked(ptr);
}

size_t DataPool::GetTotalAllocatedSize() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return GetTotalAllocatedSize_Locked();
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

} // namespace Resource
} // namespace System
} // namespace DX12Engine
