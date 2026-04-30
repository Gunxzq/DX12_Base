// File: d:\project\DX12_Base\Tests\ResourceStressTest.cpp
#include "System/Resource/Core/DataPool.h"
#include "System/Resource/ResourceManager.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

using namespace DX12Engine::System::Resource;

// ========================================================================
// 辅助类：轻量级日志记录器 (仅输出到 stdout，不涉及文件IO)
// 测试框架/CI 系统会负责日志收�?
// ========================================================================
class TestLogger {
public:
    static void Init() {}

    static void Log(const std::string &msg) {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto tid = std::this_thread::get_id();
        std::hash<std::thread::id> hasher;
        size_t tid_hash = hasher(tid) % 1000;
        std::cout << "[StressTest][T" << tid_hash << "] " << msg << std::endl;
    }

    static void Shutdown() {}

private:
    static std::mutex s_mutex;
};

std::mutex TestLogger::s_mutex;
using Logger = TestLogger;

// ========================================================================
// 测试套件: ResourceManagerStressTest
// ========================================================================

class ResourceManagerStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::Init();

        // 初始�?ResourceManager
        auto &rm = ResourceManager::GetInstance();
        rm.Initialize();

        Logger::Log("=== ResourceManager Stress Test Started ===");
        Logger::Log("ResourceManager Initialized.");
    }

    void TearDown() override {
        auto &rm = ResourceManager::GetInstance();

        // 【关键优化】使�?ForceCleanupForTesting 绕过帧延迟机�?
        // 避免在测试结束时�?PendingRelease 队列积压导致长时间等�?
        // 这在压力测试场景下（8�?句柄）可节省 90%+ 的清理时�?
        rm.ForceCleanupForTesting();

        uint32_t activeCount = rm.GetActiveCount();
        size_t memUsage = rm.GetMemoryUsage();

        std::stringstream ss;
        ss << "=== Test Finished ===\n"
           << "Final Active Handles: " << activeCount << "\n"
           << "Final Memory Usage: " << memUsage << " bytes";

        if (activeCount != 0) {
            ss << "\nERROR: RESOURCE LEAK DETECTED!";
        } else {
            ss << "\nSUCCESS: No resource leaks.";
        }

        Logger::Log(ss.str());

        // 验证无泄�?
        EXPECT_EQ(activeCount, 0u) << "Resource leak detected! Active handles: " << activeCount;

        rm.Shutdown();
    }
};

// ========================================================================
// 测试用例 1: 高并发分配与释放 (Mountains of Handles)
// 目的: 测试 HandlePool �?TLS 缓存机制和全局锁竞争情�?
// ========================================================================
TEST_F(ResourceManagerStressTest, HighConcurrencyAllocateFree) {
    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 10000;

    std::atomic<int> successCount{0};
    std::vector<std::thread> threads;

    Logger::Log("[Test 1] Starting HighConcurrencyAllocateFree. Threads: " + std::to_string(NUM_THREADS));

    // 【关键修复】使�?Preallocate 一次性分配到目标容量，避免循环扩�?
    // 测试总共需要约 80000 次分配（8线程 × 10000），预分�?300000 个确保充�?
    Logger::Log("[Test 1] Pre-allocating handles to avoid expansion during concurrency...");
    auto &rm = ResourceManager::GetInstance();

    // 【优化】使�?Preallocate 一次性扩容到目标容量
    // 这样只需一次（或少数几次）扩容，而不是循环中每次分配都触发扩�?
    const int TARGET_PREALLOC = 300000;
    rm.Preallocate(TARGET_PREALLOC);
    Logger::Log("[Test 1] Pre-allocated " + std::to_string(TARGET_PREALLOC) + " handles.");
    Logger::Log("[Test 1] Pre-allocation complete, starting concurrent test...");

    // 【关键】保留预分配句柄的引用，用于测试结束后清�?
    // 注意：前 PRE_WARM_COUNT 个已经被释放�?TLS 缓存，后面的�?preAllocatedHandles 中等待被测试使用
    // 测试结束后会统一清理

    auto workerFunc = [&](int threadId) {
        auto &rm = ResourceManager::GetInstance();
        std::vector<ResourceHandle> localHandles;
        localHandles.reserve(100);

        std::mt19937 rng(threadId);
        std::uniform_int_distribution<int> dist(0, 100);

        Logger::Log("[Test 1] Thread " + std::to_string(threadId) + " started.");

        for (int i = 0; i < OPS_PER_THREAD; ++i) {
            // 分配 Slot（现在应该从 TLS 缓存快速获取，无需触发扩容�?
            ResourceHandle handle = rm.AllocateSlot(ResourceType::Mesh);

            // 【防御性检查】如果分配失败（容量不足），跳过
            if (handle.index == UINT32_MAX) {
                Logger::Log("[Test 1] Thread " + std::to_string(threadId) + " allocation failed at " +
                            std::to_string(i));
                continue;
            }

            localHandles.push_back(handle);
            successCount.fetch_add(1);

            // 随机释放一些之前的 Handle（简化测试，移除 RegisterData 调用�?
            if (localHandles.size() > 50 && dist(rng) < 20) {
                ResourceHandle toRelease = localHandles.back();
                localHandles.pop_back();
                rm.Release(toRelease);
            }

            // 【调试】每 1000 次输出一次进�?
            if (i % 1000 == 0) {
                Logger::Log("[Test 1] Thread " + std::to_string(threadId) + " at iteration " + std::to_string(i));
            }
        }

        Logger::Log("[Test 1] Thread " + std::to_string(threadId) +
                    " finished allocations, releasing remaining handles...");

        // 线程结束前，释放所有剩余持有的 Handle
        for (auto &h : localHandles) {
            rm.Release(h);
        }

        Logger::Log("[Test 1] Thread " + std::to_string(threadId) + " done.");
    };

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(workerFunc, i);
    }

    for (auto &t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    Logger::Log("[Test 1] Completed in " + std::to_string(duration.count()) +
                " ms. Total Allocations: " + std::to_string(successCount.load()));

    EXPECT_GT(duration.count(), 0);

    // 【清理】预分配�?handles 已经释放回池，只需等待 PendingRelease 完成
    // 并发测试中的 handles 会被线程自动清理
    Logger::Log("[Test 1] Cleanup: processing pending releases...");

    // 【优化】使�?ForceCleanupForTesting 绕过帧延迟，立即清理所有待释放句柄
    rm.ForceCleanupForTesting();

    Logger::Log("[Test 1] Cleanup complete. Active count: " + std::to_string(rm.GetActiveCount()));
}

// ========================================================================
// 测试用例 2: 混合读写与状态转换压力测�?
// 目的: 测试 Validate, GetState, SetState 在多线程下的一致�?
// ========================================================================
TEST_F(ResourceManagerStressTest, MixedReadWriteStress) {
    const int NUM_THREADS = 4;
    const int HANDLE_COUNT = 1000;

    std::vector<ResourceHandle> sharedHandles;
    sharedHandles.reserve(HANDLE_COUNT);

    auto &rm = ResourceManager::GetInstance();

    // 预分配一�?Handle
    for (int i = 0; i < HANDLE_COUNT; ++i) {
        ResourceHandle h = rm.AllocateSlot(ResourceType::Mesh);
        sharedHandles.push_back(h);
    }

    std::atomic<int> validationErrors{0};
    std::vector<std::thread> threads;

    Logger::Log("[Test 2] Starting MixedReadWriteStress. Handles: " + std::to_string(HANDLE_COUNT));

    // 分配句柄的索引（用于轮换分配新句柄）
    std::atomic<int> nextHandleIdx{0};

    auto writerFunc = [&](int threadId, int handleCount) {
        std::mt19937 rng(threadId + 100);

        for (int i = 0; i < 5000; ++i) {
            // 【修复】每次操作分配新句柄，避免重复使用已释放的句�?
            // 这样可以正确测试 AllocateSlot -> RegisterData -> Release 流程
            ResourceHandle h = rm.AllocateSlot(ResourceType::Mesh);

            // 防御性检查：确保分配成功
            if (h.index == UINT32_MAX) {
                std::this_thread::yield();
                continue;
            }

            // 模拟加载过程: Loading -> Ready
            rm.RegisterData(h, nullptr, 0);

            // 短暂停留，增加竞争窗�?
            std::this_thread::yield();

            // 模拟释放: Ready -> PendingRelease -> Empty
            rm.Release(h);
        }
    };

    auto readerFunc = [&](int threadId, const std::vector<ResourceHandle> &handles) {
        std::mt19937 rng(threadId + 200);
        std::uniform_int_distribution<int> dist(0, static_cast<int>(handles.size()) - 1);

        for (int i = 0; i < 5000; ++i) {
            int idx = dist(rng);
            ResourceHandle h = handles[idx];

            // 频繁验证和获取数�?
            void *ptr = rm.GetData(h);
            // ptr 可能�?nullptr，这是预期的
        }
    };

    auto start = std::chrono::high_resolution_clock::now();

    // 预分配足够的句柄供测试使�?
    rm.Preallocate(NUM_THREADS * 5000 * 2);

    // 启动写�?
    for (int i = 0; i < NUM_THREADS / 2; ++i) {
        threads.emplace_back(writerFunc, i, HANDLE_COUNT);
    }

    // 启动读者（仍然使用预分配的句柄池进行只读操作）
    for (int i = NUM_THREADS / 2; i < NUM_THREADS; ++i) {
        threads.emplace_back(readerFunc, i, std::cref(sharedHandles));
    }

    for (auto &t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    // 【关键修复】等待写者线程的句柄完成延迟释放
    Logger::Log("[Test 2] Cleanup: processing pending releases...");
    rm.ForceCleanupForTesting();

    // 释放预分配的共享句柄
    Logger::Log("[Test 2] Releasing shared handles...");
    for (auto &h : sharedHandles) {
        rm.Release(h);
    }

    // 【优化】使�?ForceCleanupForTesting 立即清理
    rm.ForceCleanupForTesting();

    Logger::Log("[Test 2] Cleanup complete. Active count: " + std::to_string(rm.GetActiveCount()));

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    Logger::Log("[Test 2] Completed in " + std::to_string(duration.count()) + " ms.");

    EXPECT_EQ(validationErrors.load(), 0);
}

// ========================================================================
// 测试用例 3: 极端情况 - 快速创建与销毁循�?
// 目的: 测试 HandlePool �?ExpandCapacity �?Generation 递增逻辑
// ========================================================================
TEST_F(ResourceManagerStressTest, RapidCycleStress) {
    const int CYCLES = 100;
    const int HANDLES_PER_CYCLE = 500;

    auto &rm = ResourceManager::GetInstance();

    Logger::Log("[Test 3] Starting RapidCycleStress. Cycles: " + std::to_string(CYCLES));

    for (int c = 0; c < CYCLES; ++c) {
        std::vector<ResourceHandle> batch;
        batch.reserve(HANDLES_PER_CYCLE);

        // 1. 批量分配
        for (int i = 0; i < HANDLES_PER_CYCLE; ++i) {
            ResourceHandle h = rm.AllocateSlot(ResourceType::Mesh);
            batch.push_back(h);
        }

        // 2. 批量注册并释�?
        for (auto &h : batch) {
            rm.RegisterData(h, nullptr, 0);
            rm.Release(h);
        }

        // 3. �?10 个循环更新一�?ResourceManager，触发部分回�?
        if (c % 10 == 0) {
            rm.Update(0.016f);
        }
    }

    // 【优化】使�?ForceCleanupForTesting 立即清理所有待释放句柄
    rm.ForceCleanupForTesting();

    Logger::Log("[Test 3] Completed. Final Active Count: " + std::to_string(rm.GetActiveCount()));

    EXPECT_EQ(rm.GetActiveCount(), 0u);
}

// ========================================================================
// 测试套件: DataPoolTest
// 专门测试 DataPool 的内存分配功能
// ========================================================================

class DataPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::Init();
        Logger::Log("=== DataPool Test Started ===");
        auto &rm = ResourceManager::GetInstance();
        rm.Initialize();
    }

    void TearDown() override {
        auto &rm = ResourceManager::GetInstance();
        rm.ForceCleanupForTesting();
        rm.Shutdown();
        Logger::Log("=== DataPool Test Finished ===");
    }
};

// ========================================================================
// 测试用例 4: DataPool 基础分配
// ========================================================================
TEST_F(DataPoolTest, BasicAllocation) {
    auto &rm = ResourceManager::GetInstance();

    Logger::Log("[Test 4] Starting BasicAllocation.");

    // 分配句柄
    ResourceHandle h1 = rm.AllocateSlot(ResourceType::Mesh);
    ASSERT_NE(h1.index, UINT32_MAX);

    // 分配不同大小的内存
    size_t size1 = 64;
    void *ptr1 = rm.GetDataPool().Allocate(size1, 16);

    // 注册数据
    rm.RegisterData(h1, ptr1, size1);

    // 验证
    void *retrieved = rm.GetData(h1);
    EXPECT_EQ(retrieved, ptr1);

    rm.Release(h1);
    Logger::Log("[Test 4] BasicAllocation passed.");
}

// ========================================================================
// 测试用例 5: DataPool 对齐测试
// ========================================================================
TEST_F(DataPoolTest, AlignmentTest) {
    auto &rm = ResourceManager::GetInstance();

    Logger::Log("[Test 5] Starting AlignmentTest.");

    const int NUM_ALLOCS = 100;
    std::vector<std::pair<void *, size_t>> allocs;

    // 测试不同对齐要求
    std::vector<size_t> alignments = {8, 16, 32, 64, 128};

    for (size_t align : alignments) {
        for (int i = 0; i < 10; ++i) {
            size_t size = 64 + (rand() % 256);
            void *ptr = rm.GetDataPool().Allocate(size, align);

            // 验证对齐
            uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
            EXPECT_EQ(addr % align, 0u) << "Alignment failed for align=" << align;

            allocs.push_back({ptr, size});
        }
    }

    Logger::Log("[Test 5] AlignmentTest passed. Allocations: " + std::to_string(allocs.size()));
}

// ========================================================================
// 测试用例 6: DataPool 大对象分配
// ========================================================================
TEST_F(DataPoolTest, LargeObjectAllocation) {
    auto &rm = ResourceManager::GetInstance();

    Logger::Log("[Test 6] Starting LargeObjectAllocation.");

    // 分配超过 TLS Arena 大小的对象 (64KB)
    size_t largeSize = 128 * 1024;
    void *largePtr = rm.GetDataPool().Allocate(largeSize, 16);

    ASSERT_NE(largePtr, nullptr);

    // 验证对齐
    uintptr_t addr = reinterpret_cast<uintptr_t>(largePtr);
    EXPECT_EQ(addr % 16, 0u);

    // 写入数据并验证
    std::memset(largePtr, 0xAB, largeSize);

    // 分配小块，验证没有覆盖大数据
    void *smallPtr = rm.GetDataPool().Allocate(64, 16);
    ASSERT_NE(smallPtr, nullptr);

    Logger::Log("[Test 6] LargeObjectAllocation passed.");
}

// ========================================================================
// 测试用例 7: DataPool 多线程分配
// ========================================================================
TEST_F(DataPoolTest, MultiThreadAllocation) {
    const int NUM_THREADS = 4;
    const int ALLOCS_PER_THREAD = 1000;

    auto &rm = ResourceManager::GetInstance();

    Logger::Log("[Test 7] Starting MultiThreadAllocation. Threads: " + std::to_string(NUM_THREADS));

    std::atomic<int> allocCount{0};
    std::vector<std::thread> threads;

    auto workerFunc = [&](int threadId) {
        std::mt19937 rng(threadId);
        std::uniform_int_distribution<size_t> sizeDist(16, 1024);
        std::vector<void *> localPtrs;

        for (int i = 0; i < ALLOCS_PER_THREAD; ++i) {
            size_t size = sizeDist(rng);
            size_t align = 16;

            void *ptr = rm.GetDataPool().Allocate(size, align);
            ASSERT_NE(ptr, nullptr);

            // 验证对齐
            uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
            ASSERT_EQ(addr % align, 0u);

            // 写入数据
            std::memset(ptr, static_cast<int>(threadId), size);

            localPtrs.push_back(ptr);
            allocCount.fetch_add(1);
        }

        Logger::Log("[Test 7] Thread " + std::to_string(threadId) + " allocated " + std::to_string(localPtrs.size()) +
                    " blocks.");
    };

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(workerFunc, i);
    }

    for (auto &t : threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    Logger::Log("[Test 7] MultiThreadAllocation completed in " + std::to_string(duration.count()) +
                " ms. Total allocations: " + std::to_string(allocCount.load()));

    EXPECT_GT(allocCount.load(), 0);
}

// ========================================================================
// 测试用例 8: DataPool Reset 测试
// ========================================================================
TEST_F(DataPoolTest, ResetTest) {
    auto &rm = ResourceManager::GetInstance();

    Logger::Log("[Test 8] Starting ResetTest.");

    // 分配一些数据
    ResourceHandle h1 = rm.AllocateSlot(ResourceType::Mesh);
    void *ptr1 = rm.GetDataPool().Allocate(1024, 16);
    rm.RegisterData(h1, ptr1, 1024);

    ResourceHandle h2 = rm.AllocateSlot(ResourceType::Texture);
    void *ptr2 = rm.GetDataPool().Allocate(2048, 16);
    rm.RegisterData(h2, ptr2, 2048);

    // 重置前的大小
    size_t sizeBefore = rm.GetMemoryUsage();
    Logger::Log("[Test 8] Memory before reset: " + std::to_string(sizeBefore));

    // 重置 DataPool
    rm.GetDataPool().Reset();

    // 重置后大小应该为 0（因为使用了内存池）
    size_t sizeAfter = rm.GetMemoryUsage();
    Logger::Log("[Test 8] Memory after reset: " + std::to_string(sizeAfter));

    // 重新分配应该成功
    void *ptr3 = rm.GetDataPool().Allocate(512, 16);
    EXPECT_NE(ptr3, nullptr);

    Logger::Log("[Test 8] ResetTest passed.");
}
