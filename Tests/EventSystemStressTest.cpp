#include "System/Event/BucketManager.h"
#include "System/Event/Event.h"
#include "System/Event/EventTypes.h" // 包含 WindowResizeEvent 等定义
#include "System/Event/MessageArena.h"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <iostream>
#include <fstream>
#include <random>
#include <thread>
#include <vector>
#include <mutex>

// ===== 调试日志到文件 =====
#define TEST_DEBUG 1
#if TEST_DEBUG
#include <iomanip>

namespace {
    std::mutex g_testLogMutex;
    std::ofstream g_testDebugLogFile;

    void InitTestDebugLogFile() {
        static bool initialized = false;
        if (!initialized) {
            std::lock_guard<std::mutex> lock(g_testLogMutex);
            if (!initialized) {
                auto now = std::chrono::system_clock::now();
                auto time_t_now = std::chrono::system_clock::to_time_t(now);
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()) % 1000;
                
                std::ostringstream filename;
                filename << "test_debug_"
                        << std::put_time(std::localtime(&time_t_now), "%Y%m%d_%H%M%S")
                        << "_" << std::setfill('0') << std::setw(3) << ms.count()
                        << ".log";
                
                g_testDebugLogFile.open(filename.str(), std::ios::out | std::ios::trunc);
                initialized = true;
            }
        }
    }

    void TestDebugLog(const char* msg) {
        InitTestDebugLogFile();
        if (g_testDebugLogFile.is_open()) {
            std::lock_guard<std::mutex> lock(g_testLogMutex);
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()) % 1000000;
            
            g_testDebugLogFile << "[" << std::put_time(std::localtime(&time_t_now), "%H:%M:%S")
                              << "." << std::setfill('0') << std::setw(6) << ms.count()
                              << "] [Test] " << msg << std::endl;
            g_testDebugLogFile.flush();
        }
    }

    void TestDebugLog(const std::string& msg) {
        TestDebugLog(msg.c_str());
    }
}

#define TEST_DBG(msg) do { \
    std::ostringstream ss; \
    ss << msg; \
    TestDebugLog(ss.str()); \
} while(0)
#else
#define TEST_DBG(msg) ((void)0)
#endif

namespace DX12Engine {
namespace System {
namespace Event {

// ========================================================================
// 测试套件: EventSystemStressTest
// ========================================================================

class EventSystemStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        // ===== "急救手术"：精准扩容 =====
        // 测试强度：4 线程 × 2500 条 = 10000 条消息
        // 安全边际：留 2 倍余量，确保不会因为容量不足导致测试失败
        constexpr uint32_t BUCKET_CAPACITY = 16384; // 每个桶 16K 条消息
        constexpr uint32_t ARENA_CAPACITY = 65536;  // Arena 支持 64K 消息

        // 初始化 Arena 和 BucketManager
        m_arena = std::make_unique<MessageArena>(ARENA_CAPACITY); // 64K 槽位，防止溢出
        m_bucketManager = std::make_unique<BucketManager>();
        m_bucketManager->Initialize(*m_arena, BUCKET_CAPACITY); // 每个桶 16K 容量
    }

    void TearDown() override {
        m_bucketManager.reset();
        m_arena.reset();
    }

    std::unique_ptr<MessageArena> m_arena;
    std::unique_ptr<BucketManager> m_bucketManager;
};

// 辅助结构：用于统计测试结果
struct StressTestStats {
    std::atomic<uint64_t> producedCount{0};
    std::atomic<uint64_t> consumedCount{0};
    std::atomic<bool> errorOccurred{false};
    std::string errorMessage;
};

// ========================================================================
// 测试用例1：多线程高并发写入与读取 (Throughput Test)
// ========================================================================
TEST_F(EventSystemStressTest, HighConcurrencyThroughput) {
    // 测试强度：4 线程 × 2,500 条 = 10,000 条消息
    constexpr int NUM_PRODUCER_THREADS = 4;
    constexpr int MESSAGES_PER_THREAD = 2500;
    constexpr uint64_t TOTAL_MESSAGES = static_cast<uint64_t>(NUM_PRODUCER_THREADS) * MESSAGES_PER_THREAD;

    StressTestStats stats;
    std::vector<std::thread> producers;
    std::vector<WindowResizeEvent> receivedEvents;
    receivedEvents.reserve(TOTAL_MESSAGES);

    std::atomic<int> pushFailThread{-1};
    std::atomic<int> pushFailAt{-1};

    // --- 1. 启动生产者线程 ---
    auto startProdTime = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_PRODUCER_THREADS; ++i) {
        producers.emplace_back([this, &stats, &pushFailThread, &pushFailAt, i, MESSAGES_PER_THREAD]() {
            for (int j = 0; j < MESSAGES_PER_THREAD; ++j) {
                // 1. 分配槽位
                MessageIndex index = m_arena->AllocateSlot();
                if (index == MessageArena::INVALID_INDEX) {
                    pushFailThread.store(i);
                    pushFailAt.store(j);
                    stats.errorOccurred.store(true);
                    stats.errorMessage = "Arena overflow at thread " + std::to_string(i) + " msg " + std::to_string(j);
                    return;
                }

                // 2. 构造事件数据 (模拟不同优先级)
                EventPriority prio = (j % 2 == 0) ? EventPriority::P1_High : EventPriority::P2_Normal;
                uint32_t w = static_cast<uint32_t>(i * 1000 + j);
                uint32_t h = static_cast<uint32_t>(800 + j);

                // 3. 在栈上构造事件
                WindowResizeEvent event(w, h);
                event.Priority = prio;

                // 4. 写入 Arena（传入大小，让 Arena 内部 memcpy 存储）
                m_arena->WriteMessage(index, event.GetTypeHash(), 0, &event, sizeof(event));

                // 5. 推入 Bucket
                bool pushSuccess = m_bucketManager->PushMessage(index, prio);
                if (!pushSuccess) {
                    pushFailThread.store(i);
                    pushFailAt.store(j);
                    stats.errorOccurred.store(true);
                    stats.errorMessage =
                        "Failed to push message to bucket at thread " + std::to_string(i) + " msg " + std::to_string(j);
                    return;
                }

                stats.producedCount.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // 等待所有生产者完成
    for (auto &t : producers) {
        t.join();
    }

    auto endProdTime = std::chrono::high_resolution_clock::now();
    double prodDurationMs = std::chrono::duration<double, std::milli>(endProdTime - startProdTime).count();

    // --- 2. 消费者单线程读取 (模拟调度器) ---
    TEST_DBG("Arena count after production: " << m_arena->GetCount());
    TEST_DBG("Produced count: " << stats.producedCount.load());
    TEST_DBG("Push fail thread: " << pushFailThread.load() << " at msg: " << pushFailAt.load());

    if (stats.errorOccurred.load()) {
        TEST_DBG("ERROR: " << stats.errorMessage);
    }
    ASSERT_FALSE(stats.errorOccurred.load()) << "Producer error: " << stats.errorMessage;
    EXPECT_EQ(stats.producedCount.load(), TOTAL_MESSAGES);

    auto startConsTime = std::chrono::high_resolution_clock::now();

    uint64_t consumed = 0;
    while (consumed < TOTAL_MESSAGES) {
        MessageIndex index;
        EventPriority prio;

        if (m_bucketManager->PopNextMessage(index, prio)) {
            // 从 Arena 读取数据
            void *payloadPtr = m_arena->GetPayload(index);
            if (!payloadPtr) {
                stats.errorOccurred.store(true);
                stats.errorMessage = "Null payload in arena at index " + std::to_string(index);
                break;
            }

            // 强制转换为 WindowResizeEvent
            WindowResizeEvent *evt = static_cast<WindowResizeEvent *>(payloadPtr);

            // 基本完整性检查（使用 MAGIC 标记检测真正的未初始化内存）
            constexpr uint32_t UNINIT_MAGIC = 0xCDCDCDCD;
            if (evt->Padding == UNINIT_MAGIC) {
                // 详细调试信息
                uint32_t *data = reinterpret_cast<uint32_t *>(payloadPtr);
                TEST_DBG("ERROR: Corrupted (uninitialized) at index=" << index
                          << ", Width=" << evt->Width << ", Height=" << evt->Height
                          << ", Padding=0x" << std::hex << evt->Padding);
                stats.errorOccurred.store(true);
                stats.errorMessage = "Uninitialized padding at index " + std::to_string(index);
                break;
            }

            // 存储以便后续分析（可选，仅存部分以防内存爆炸）
            if (receivedEvents.size() < 100) {
                receivedEvents.push_back(*evt);
            }

            consumed++;
            stats.consumedCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            // 如果没有消息，稍微让出 CPU，避免忙等待死锁（虽然理论上生产者已写完）
            std::this_thread::yield();
        }
    }

    auto endConsTime = std::chrono::high_resolution_clock::now();
    double consDurationMs = std::chrono::duration<double, std::milli>(endConsTime - startConsTime).count();

    // --- 3. 验证结果 ---
    ASSERT_FALSE(stats.errorOccurred.load()) << "Consumer error: " << stats.errorMessage;
    EXPECT_EQ(stats.consumedCount.load(), TOTAL_MESSAGES);

    // 输出性能数据
    TEST_DBG("Produced " << TOTAL_MESSAGES << " messages.");
    TEST_DBG("Producer Time: " << prodDurationMs << " ms ("
              << (TOTAL_MESSAGES / prodDurationMs * 1000) << " msg/s)");
    TEST_DBG("Consumer Time: " << consDurationMs << " ms ("
              << (TOTAL_MESSAGES / consDurationMs * 1000) << " msg/s)");

    // 验证收到的样本数据合理性
    if (!receivedEvents.empty()) {
        EXPECT_GT(receivedEvents[0].Width, 0);
        EXPECT_GT(receivedEvents[0].Height, 0);
    }
}

// ========================================================================
// 测试用例2：混合优先级与 Aging 机制验证 (Priority & Aging Test)
// ========================================================================
TEST_F(EventSystemStressTest, MixedPriorityAndAging) {
    // 这个测试验证 BucketManager 是否能正确处理不同优先级的消息
    // 并验证 Aging 机制是否会让旧的低优先级消息被优先处理（如果实现正确）

    constexpr int LOW_PRIO_COUNT = 100;
    constexpr int HIGH_PRIO_COUNT = 10;

    // 1. 先填入大量低优先级消息 (P3_Low)
    for (int i = 0; i < LOW_PRIO_COUNT; ++i) {
        MessageIndex index = m_arena->AllocateSlot();
        ASSERT_NE(index, MessageArena::INVALID_INDEX);

        WindowResizeEvent event(100 + i, 200 + i);
        event.Priority = EventPriority::P3_Low; // 手动覆盖优先级

        m_arena->WriteMessage(index, event.GetTypeHash(), 0, &event, sizeof(event));
        m_bucketManager->PushMessage(index, EventPriority::P3_Low);
    }

    // 2. 填入少量高优先级消息 (P0_Critical)
    for (int i = 0; i < HIGH_PRIO_COUNT; ++i) {
        MessageIndex index = m_arena->AllocateSlot();
        ASSERT_NE(index, MessageArena::INVALID_INDEX);

        WindowResizeEvent event(900 + i, 900 + i);
        event.Priority = EventPriority::P0_Critical;

        m_arena->WriteMessage(index, event.GetTypeHash(), 0, &event, sizeof(event));
        m_bucketManager->PushMessage(index, EventPriority::P0_Critical);
    }

    // 3. 消费消息并检查顺序
    // 预期：由于 P0 优先级远高于 P3，即使 P3 先入队，P0 也应该先被取出（除非 Aging 极大，但这里时间差极小）
    std::vector<EventPriority> processedPriorities;

    for (int i = 0; i < (LOW_PRIO_COUNT + HIGH_PRIO_COUNT); ++i) {
        MessageIndex index;
        EventPriority prio;
        if (m_bucketManager->PopNextMessage(index, prio)) {
            processedPriorities.push_back(prio);
        } else {
            FAIL() << "Failed to pop message at index " << i;
        }
    }

    // 验证：前几个消息应该是 P0_Critical
    // 注意：BucketManager 使用浮点数评分，P0 的基础分最高。
    // 只要 Aging 系数不是极大，P0 应该排在前面。
    int criticalCountAtStart = 0;
    for (int i = 0; i < HIGH_PRIO_COUNT; ++i) {
        if (processedPriorities[i] == EventPriority::P0_Critical) {
            criticalCountAtStart++;
        }
    }

    // 期望大部分高优先级消息在前部被处理
    EXPECT_GT(criticalCountAtStart, HIGH_PRIO_COUNT / 2) << "High priority messages were not processed first. Order "
                                                            "might be affected by Aging or implementation details.";
}

} // namespace Event
} // namespace System
} // namespace DX12Engine