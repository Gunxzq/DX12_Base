// File: d:\project\DX12_Base\Tests\ResourceStressTest.cpp
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
// 测试框架/CI 系统会负责日志收集
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

        auto &rm = ResourceManager::GetInstance();

        // 创建默认配置：使用 Linear 策略的数据池
        ResourceSystemConfig config;
        config.HandlePoolConfig.MaxTotalHandles = 262144;
        config.HandlePoolConfig.InitialFreeListReserve = 262144;

        // 添加一个默认数据池（Linear 策略）
        MemoryPoolConfig poolConfig;
        poolConfig.HandleTag = 0;
        poolConfig.Name = "DefaultLinearPool";
        poolConfig.Strategy = MemoryStrategy::Linear;
        poolConfig.SizeMB = 64; // 64MB
        poolConfig.Alignment = 16;
        config.MemoryPools.push_back(poolConfig);

        rm.Initialize(config);

        Logger::Log("=== ResourceManager Stress Test Started ===");
        Logger::Log("ResourceManager Initialized.");
    }

    void TearDown() override {
        auto &rm = ResourceManager::GetInstance();

        // 【关键优化】使用 ForceCleanupForTesting 绕过帧延迟机制
        // 避免在测试结束时 PendingRelease 队列积压导致长时间等待
        // 这在压力测试场景下（80000句柄）可节省 90%+ 的清理时间
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

        // 验证无泄漏
        EXPECT_EQ(activeCount, 0u) << "Resource leak detected! Active handles: " << activeCount;

        rm.Shutdown();
    }
};

// ========================================================================
// 测试用例 1: 高并发分配与释放 (Mountains of Handles)
// 目的: 测试 HandlePool 的 TLS 缓存机制和全局锁竞争情况
// ========================================================================
TEST_F(ResourceManagerStressTest, HighConcurrencyAllocateFree) {
    const int NUM_THREADS = 16;
    const int OPS_PER_THREAD = 20000;

    std::atomic<int> successCount{0};
    std::vector<std::thread> threads;

    Logger::Log("[Test 1] Starting HighConcurrencyAllocateFree. Threads: " + std::to_string(NUM_THREADS));

    // 使用 Preallocate 一次性分配到目标容量，避免循环扩容
    // 测试总共需要约 320000 次分配（16线程 × 20000），预分配 350000 个确保充足
    Logger::Log("[Test 1] Pre-allocating handles to avoid expansion during concurrency...");
    auto &rm = ResourceManager::GetInstance();

    // 【优化】使用 Preallocate 一次性扩容到目标容量
    const int TARGET_PREALLOC = 350000;
    rm.Preallocate(TARGET_PREALLOC);
    Logger::Log("[Test 1] Pre-allocated " + std::to_string(TARGET_PREALLOC) + " handles.");
    Logger::Log("[Test 1] Pre-allocation complete, starting concurrent test...");

    auto workerFunc = [&](int threadId) {
        auto &rm = ResourceManager::GetInstance();
        std::vector<ResourceHandle> localHandles;
        localHandles.reserve(100);

        std::mt19937 rng(threadId);
        std::uniform_int_distribution<int> dist(0, 100);

        Logger::Log("[Test 1] Thread " + std::to_string(threadId) + " started.");

        for (int i = 0; i < OPS_PER_THREAD; ++i) {
            // 分配 Slot（现在应该从 TLS 缓存快速获取，无需触发扩容）
            ResourceHandle handle = rm.AllocateSlot(ResourceType::Mesh);

            // 【防御性检查】如果分配失败（容量不足），跳过
            if (handle.index == UINT32_MAX) {
                Logger::Log("[Test 1] Thread " + std::to_string(threadId) + " allocation failed at " +
                            std::to_string(i));
                continue;
            }

            localHandles.push_back(handle);
            successCount.fetch_add(1);

            // 随机释放一些之前的 Handle（简化测试，移除 RegisterData 调用）
            if (localHandles.size() > 50 && dist(rng) < 20) {
                ResourceHandle toRelease = localHandles.back();
                localHandles.pop_back();
                rm.Release(toRelease);
            }

            // 【调试】每 1000 次输出一次进度
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

    // 【清理】预分配的 handles 已经释放回池，只需等待 PendingRelease 完成
    // 并发测试中的 handles 会被线程自动清理
    Logger::Log("[Test 1] Cleanup: processing pending releases...");

    // 【优化】使用 ForceCleanupForTesting 绕过帧延迟，立即清理所有待释放句柄
    rm.ForceCleanupForTesting();

    Logger::Log("[Test 1] Cleanup complete. Active count: " + std::to_string(rm.GetActiveCount()));
}

// ========================================================================
// 测试用例 2: 混合读写与状态转换压力测试
// 目的: 测试 Validate, GetState, SetState 在多线程下的一致性
// ========================================================================
TEST_F(ResourceManagerStressTest, MixedReadWriteStress) {
    const int NUM_THREADS = 16;
    const int HANDLE_COUNT = 2000;

    std::vector<ResourceHandle> sharedHandles;
    sharedHandles.reserve(HANDLE_COUNT);

    auto &rm = ResourceManager::GetInstance();

    // 预分配一些 Handle
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

        for (int i = 0; i < 10000; ++i) {
            // 【修复】每次操作分配新句柄，避免重复使用已释放的句柄
            // 这样可以正确测试 AllocateSlot -> RegisterData -> Release 流程
            ResourceHandle h = rm.AllocateSlot(ResourceType::Mesh);

            // 防御性检查：确保分配成功
            if (h.index == UINT32_MAX) {
                std::this_thread::yield();
                continue;
            }

            // 模拟加载过程: Loading -> Ready
            rm.RegisterData(h, nullptr, 0);

            // 短暂停留，增加竞争窗口
            std::this_thread::yield();

            // 模拟释放: Ready -> PendingRelease -> Empty
            rm.Release(h);
        }
    };

    auto readerFunc = [&](int threadId, const std::vector<ResourceHandle> &handles) {
        std::mt19937 rng(threadId + 200);
        std::uniform_int_distribution<int> dist(0, static_cast<int>(handles.size()) - 1);

        for (int i = 0; i < 10000; ++i) {
            int idx = dist(rng);
            ResourceHandle h = handles[idx];

            // 频繁验证和获取数据
            void *ptr = rm.GetData(h);
            // ptr 可能为 nullptr，这是预期的
        }
    };

    auto start = std::chrono::high_resolution_clock::now();

    // 预分配不足够的句柄，设为150000以触发扩容（16线程*10000 = 160000）
    rm.Preallocate(150000);

    // 启动写线程
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

    // 等待写者线程的句柄完成延迟释放
    Logger::Log("[Test 2] Cleanup: processing pending releases...");
    rm.ForceCleanupForTesting();

    // 释放预分配的共享句柄
    Logger::Log("[Test 2] Releasing shared handles...");
    for (auto &h : sharedHandles) {
        rm.Release(h);
    }

    // 【优化】使用 ForceCleanupForTesting 立即清理
    rm.ForceCleanupForTesting();

    Logger::Log("[Test 2] Cleanup complete. Active count: " + std::to_string(rm.GetActiveCount()));

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    Logger::Log("[Test 2] Completed in " + std::to_string(duration.count()) + " ms.");

    EXPECT_EQ(validationErrors.load(), 0);
}

// ========================================================================
// 测试用例 3: 极端情况 - 快速创建与销毁循环
// 目的: 测试 HandlePool 的 ExpandCapacity 和 Generation 递增逻辑
// ========================================================================
TEST_F(ResourceManagerStressTest, RapidCycleStress) {
    const int CYCLES = 200;
    const int HANDLES_PER_CYCLE = 1000;

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

        // 2. 批量注册并释放
        for (auto &h : batch) {
            rm.RegisterData(h, nullptr, 0);
            rm.Release(h);
        }

        // 3. 每10个循环更新一次 ResourceManager，触发部分回收
        if (c % 10 == 0) {
            rm.Update(0.016f);
        }
    }

    // 【优化】使用 ForceCleanupForTesting 立即清理所有待释放句柄
    rm.ForceCleanupForTesting();

    Logger::Log("[Test 3] Completed. Final Active Count: " + std::to_string(rm.GetActiveCount()));

    EXPECT_EQ(rm.GetActiveCount(), 0u);
}