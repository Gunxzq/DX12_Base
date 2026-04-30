// DataPool.h
#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace DX12Engine {
namespace System {
namespace Resource {

class DataPool {
public:
    struct ThreadLocalArena {
        static constexpr size_t ARENA_SIZE = 64 * 1024; // 假设的大小，根据实际定义调整
        char *currentPtr = nullptr;
        char *endPtr = nullptr;
        bool initialized = false;
    };

    // 初始化
    void Initialize();
    //  shutdown
    void Shutdown();

    // --- 核心接口 ---

    /**
     * @brief 分配内存 (线程安全，内部自动处理 TLS)
     */
    void *Allocate(size_t size, size_t alignment = 16);

    /**
     * @brief 释放内存 (在线性分配器中通常为空操作，依赖 Reset)
     */
    void Free(void *ptr);

    /**
     * @brief 重置所有已分配内存 (用于帧结束或关卡切换)
     * @note 调用后所有之前分配的指针失效
     */
    void Reset();

    // --- 调试/监控 (要求调用者持有锁) ---
    // REQUIRES: Caller must hold m_mutex.
    bool Contains_Locked(void *ptr) const;
    size_t GetTotalAllocatedSize_Locked() const;

    // 线程安全版本 (内部加锁)
    bool Contains(void *ptr) const;
    size_t GetTotalAllocatedSize() const;

private:
    mutable std::mutex m_mutex;
    // 内部结构：内存块
    struct Block {
        void *start;
        size_t size;
        size_t used; // 当前块已使用的字节数
    };

    // 全局内存块列表
    std::vector<Block> m_blocks;

    // 默认大块大小 (64MB)
    static constexpr size_t BLOCK_SIZE = 64 * 1024 * 1024;

    // --- 内部辅助函数 ---

    /**
     * @brief 从全局池分配一个新的 TLS Arena 段
     * @return 指向新段起始位置的指针，失败返回 nullptr
     */
    void *AllocateTLSSegment();

    /**
     * @brief 原始分配：直接从全局大块中分配内存 (加锁)
     * @note 用于大对象或 TLS 段耗尽时的补充
     */
    void *AllocateRaw(size_t size, size_t alignment);

    /**
     * @brief 内部无锁版本：调用者必须持有 m_mutex
     */
    void *AllocateRaw_Locked(size_t size, size_t alignment);

    void AllocateBlockInternal();
};

} // namespace Resource
} // namespace System
} // namespace DX12Engine