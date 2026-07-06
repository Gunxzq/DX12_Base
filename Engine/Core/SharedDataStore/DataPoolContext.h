// DataPoolContext.h
#pragma once
#include <cstdint>

namespace DX12Engine::Core {

// ========================================================================
// 全局线程上下文：解决 TLS 爆炸问题
// 每个线程只维护一个上下文，内部用 PoolID 做数组索引
// ========================================================================
constexpr size_t MAX_POOL_COUNT = 16;

struct PoolThreadState {
    char *currentPtr = nullptr; // LinearPool: 当前分配位置
    char *endPtr = nullptr;     // LinearPool: Arena 结束位置
    bool initialized = false;   // 是否已初始化
    uint8_t padding[7] = {0};   // 对齐填充
};

struct alignas(64) ResourceThreadContext {
    // 64字节对齐防止伪共享
    PoolThreadState slotStates[MAX_POOL_COUNT];
};

// 全局唯一的 TLS：每个线程只有一个实例
inline thread_local ResourceThreadContext g_threadContext;

// 获取指定池的线程状态
inline PoolThreadState *GetThreadPoolState(uint8_t poolId) { return &g_threadContext.slotStates[poolId]; }

} // namespace DX12Engine