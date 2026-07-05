#pragma once
#include "Core/Config/ConfigTypes/ResourceConfig.h"
#include "DataPoolContext.h"
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace DX12Engine {
namespace Resource {

class DataPool {
public:
    // --- 核心接口 ---

    /**
     * @brief 设置池ID（用于TLS索引）
     * @note 由 ResourceManager 在初始化时调用
     */
    void SetPoolID(uint8_t id) { m_poolID = id; }
    uint8_t GetPoolID() const { return m_poolID; }

    /**
     * @brief 初始化数据池
     * @param name 调试名称
     * @param totalSize 池的总大小
     * @param alignment 默认对齐方式
     * @param strategy 分配策略
     * @param blockSize 固定块大小（Block策略使用）
     */
    void Initialize(const std::string &name, size_t totalSize, size_t alignment,
                    Boot::MemoryStrategy strategy = Boot::MemoryStrategy::Linear, size_t blockSize = 0);

    void Shutdown();

    /**
     * @brief 分配内存
     */
    void *Allocate(size_t size, size_t alignment = 16);

    /**
     * @brief 释放内存（支持 Block 策略）
     */
    void Free(void *ptr);

    /**
     * @brief 重置所有已分配内存
     */
    void Reset();

    // --- 调试/监控 ---
    bool Contains_Locked(void *ptr) const;
    size_t GetTotalAllocatedSize_Locked() const;
    bool Contains(void *ptr) const;
    size_t GetTotalAllocatedSize() const;

    // --- 统计 ---
    size_t GetFreeBlockCount() const;
    size_t GetUsedBlockCount() const;

private:
    mutable std::mutex m_mutex;

    // 内部结构：内存块
    struct Block {
        void *start = nullptr;
        size_t size = 0;
        size_t used = 0;
    };

    // 固定块槽位（用于 Block 策略）
    struct Slot {
        bool free = true;
        size_t size = 0;
    };

    std::vector<Block> m_blocks;
    std::vector<Slot> m_slots;      // Block 策略的槽位数组
    std::vector<uint64_t> m_bitmap; // 位图标记槽位空闲状态

    std::string m_name;
    size_t m_totalSize = 0;
    size_t m_alignment = 16;
    size_t m_blockSize = 0; // 固定块大小
    uint8_t m_poolID = 0;
    Boot::MemoryStrategy m_strategy = Boot::MemoryStrategy::Linear;

    static constexpr size_t BLOCK_SIZE = 64 * 1024 * 1024;
    static constexpr size_t TLS_ARENA_SIZE = 64 * 1024;

    // --- 内部辅助 ---
    void *AllocateRaw(size_t size, size_t alignment);
    void *AllocateRaw_Locked(size_t size, size_t alignment);
    void *AllocateFromTLSArena(size_t size, size_t alignment);
    void *AllocateLinear(size_t size, size_t alignment);
    void *AllocateBlock(size_t size, size_t alignment);
    void *AllocateBlock_Locked(size_t size, size_t alignment);

    void AllocateBlockInternal();
    void AllocateSlotsInternal();

    // 位图操作
    int FindFreeSlot_Locked();
    void MarkSlotUsed(int index);
    void MarkSlotFree(int index);
};

} // namespace Resource

} // namespace DX12Engine
