#include "System/Event/BucketManager.h"
#include "System/Event/Event.h"
#include "System/Event/EventTypes.h" // 包含 WindowResizeEvent 等定义
#include "System/Event/MessageArena.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

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
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

            std::ostringstream filename;
            filename << "test_debug_" << std::put_time(std::localtime(&time_t_now), "%Y%m%d_%H%M%S") << "_"
                     << std::setfill('0') << std::setw(3) << ms.count() << ".log";

            g_testDebugLogFile.open(filename.str(), std::ios::out | std::ios::trunc);
            initialized = true;
        }
    }
}

void TestDebugLog(const char *msg) {
    InitTestDebugLogFile();
    if (g_testDebugLogFile.is_open()) {
        std::lock_guard<std::mutex> lock(g_testLogMutex);
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;

        g_testDebugLogFile << "[" << std::put_time(std::localtime(&time_t_now), "%H:%M:%S") << "." << std::setfill('0')
                           << std::setw(6) << ms.count() << "] [Test] " << msg << std::endl;
        g_testDebugLogFile.flush();
    }
}

void TestDebugLog(const std::string &msg) { TestDebugLog(msg.c_str()); }
} // namespace

#define TEST_DBG(msg)                                                                                                  \
    do {                                                                                                               \
        std::ostringstream ss;                                                                                         \
        ss << msg;                                                                                                     \
        TestDebugLog(ss.str());                                                                                        \
    } while (0)
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
    // 风暴测试参数: 8 线程 × 2000 条 = 16000 条消息，桶容量 8192
    // 这会导致 ~8000 条消息被 Sample 策略踢出
    static constexpr uint32_t STORM_THREADS = 8;
    static constexpr uint32_t STORM_MSG_PER_THREAD = 2000;
    static constexpr uint32_t STORM_TOTAL_MSGS = STORM_THREADS * STORM_MSG_PER_THREAD; // 16000
    static constexpr uint32_t STORM_BUCKET_CAP = 8192;                                 // 桶只能装 8K
    static constexpr uint32_t STORM_ARENA_CAP = STORM_TOTAL_MSGS + 4096;               // Arena = 20K

    void SetUp() override {
        // 默认风暴测试配置 (会被各测试用例覆盖)
        m_arena = std::make_unique<MessageArena>(STORM_ARENA_CAP);
        m_bucketManager = std::make_unique<BucketManager>();
        m_bucketManager->Initialize(*m_arena, STORM_BUCKET_CAP);
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
// 测试用例1：风暴测试 - 16000 条消息冲击 8192 桶
// 观察 Sample 策略疯狂踢旧人 (~8000 条被丢弃)
// ========================================================================
TEST_F(EventSystemStressTest, StormFloodingTest) {
    // 重新初始化以适应风暴参数
    m_bucketManager.reset();
    m_arena.reset();
    m_arena = std::make_unique<MessageArena>(STORM_ARENA_CAP);
    m_bucketManager = std::make_unique<BucketManager>();
    m_bucketManager->Initialize(*m_arena, STORM_BUCKET_CAP);

    constexpr uint64_t TOTAL_MSGS = STORM_TOTAL_MSGS; // 16000
    constexpr uint32_t BUCKET_CAP = STORM_BUCKET_CAP; // 8192

    // 打印参数到终端
    std::cout << "\n========== STORM TEST ==========" << std::endl;
    std::cout << "  Threads: " << STORM_THREADS << ", Msg/Thread: " << STORM_MSG_PER_THREAD << std::endl;
    std::cout << "  Total msgs: " << TOTAL_MSGS << ", Bucket cap: " << BUCKET_CAP << std::endl;
    std::cout << "  Expected evicted: ~" << (TOTAL_MSGS - BUCKET_CAP) << std::endl;

    StressTestStats stats;
    std::vector<std::thread> producers;

    // --- 1. 启动生产者线程 (所有消息强制进入 P3_Sample 桶) ---
    auto startProdTime = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < STORM_THREADS; ++i) {
        producers.emplace_back([this, &stats, i]() {
            for (int j = 0; j < STORM_MSG_PER_THREAD; ++j) {
                MessageIndex index = m_arena->AllocateSlot();
                if (index == MessageArena::INVALID_INDEX) {
                    stats.errorOccurred.store(true);
                    stats.errorMessage = "Arena overflow at thread " + std::to_string(i) + " msg " + std::to_string(j);
                    return;
                }

                // 强制使用 P3_Low (Sample 桶)，所有消息挤入同一个桶触发踢人
                EventPriority prio = EventPriority::P3_Low;
                WindowResizeEvent event(static_cast<uint32_t>(i * 10000 + j), static_cast<uint32_t>(800 + j));
                event.Priority = prio;

                m_arena->WriteMessage(index, event.GetTypeHash(), 0, &event, sizeof(event));

                bool pushSuccess = m_bucketManager->PushMessage(index, prio);
                if (pushSuccess) {
                    stats.producedCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto &t : producers) {
        t.join();
    }

    auto endProdTime = std::chrono::high_resolution_clock::now();
    double prodDurationMs = std::chrono::duration<double, std::milli>(endProdTime - startProdTime).count();

    // --- 2. 统计 ---
    uint64_t evicted = m_bucketManager->GetTotalEvictedCount();
    uint64_t stored = m_bucketManager->GetTotalPendingCount();

    std::cout << "\n--- Storm Result ---" << std::endl;
    std::cout << "  Produced: " << TOTAL_MSGS << std::endl;
    std::cout << "  Stored in buckets: " << stored << std::endl;
    std::cout << "  Evicted (Sample kicked): " << evicted << std::endl;
    std::cout << "  Producer time: " << prodDurationMs << " ms" << std::endl;

    ASSERT_FALSE(stats.errorOccurred.load()) << "Error: " << stats.errorMessage;

    // --- 3. 消费所有消息 ---
    uint64_t consumed = 0;
    auto startConsTime = std::chrono::high_resolution_clock::now();

    while (true) {
        MessageIndex index;
        EventPriority prio;
        if (m_bucketManager->PopNextMessage(index, prio)) {
            consumed++;
            stats.consumedCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            break;
        }
    }

    auto endConsTime = std::chrono::high_resolution_clock::now();
    double consDurationMs = std::chrono::duration<double, std::milli>(endConsTime - startConsTime).count();

    std::cout << "  Consumed: " << consumed << std::endl;
    std::cout << "  Consumer time: " << consDurationMs << " ms" << std::endl;

    // --- 4. 验证 ---
    // 预期 evicted = TOTAL_MSGS - BUCKET_CAP = 16000 - 8192 = 7808
    EXPECT_GE(evicted, TOTAL_MSGS - BUCKET_CAP) << "Sample eviction should kick expected amount";
    EXPECT_EQ(stats.consumedCount.load(), stats.producedCount.load());
}

// ========================================================================
// 测试用例2：增强压力测试 - 8 线程 × 5000 条 = 40000 条
// ========================================================================
TEST_F(EventSystemStressTest, HighConcurrencyThroughput) {
    constexpr int NUM_PRODUCER_THREADS = 8;
    constexpr int MESSAGES_PER_THREAD = 5000;
    constexpr uint64_t TOTAL_MESSAGES = static_cast<uint64_t>(NUM_PRODUCER_THREADS) * MESSAGES_PER_THREAD;
    constexpr uint32_t ARENA_CAP = TOTAL_MESSAGES + (TOTAL_MESSAGES / 4); // 50K
    constexpr uint32_t BUCKET_CAP = 16384;                                // 16K 桶

    // 重新初始化
    m_bucketManager.reset();
    m_arena.reset();
    m_arena = std::make_unique<MessageArena>(ARENA_CAP);
    m_bucketManager = std::make_unique<BucketManager>();
    m_bucketManager->Initialize(*m_arena, BUCKET_CAP);

    StressTestStats stats;
    std::vector<std::thread> producers;
    std::vector<WindowResizeEvent> receivedEvents;
    receivedEvents.reserve(TOTAL_MESSAGES);

    std::atomic<int> pushFailThread{-1};
    std::atomic<int> pushFailAt{-1};

    // 打印参数到终端
    std::cout << "\n========== HIGH CONCURRENCY TEST ==========" << std::endl;
    std::cout << "  Threads: " << NUM_PRODUCER_THREADS << ", Msg/Thread: " << MESSAGES_PER_THREAD << std::endl;
    std::cout << "  Total msgs: " << TOTAL_MESSAGES << ", Arena: " << ARENA_CAP << ", Bucket: " << BUCKET_CAP
              << std::endl;

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
    std::cout << "\n--- High Concurrency Result ---" << std::endl;
    std::cout << "  Arena count: " << m_arena->GetCount() << std::endl;
    std::cout << "  Produced: " << stats.producedCount.load() << std::endl;
    std::cout << "  Push fail: thread " << pushFailThread.load() << " at msg " << pushFailAt.load() << std::endl;

    if (stats.errorOccurred.load()) {
        std::cout << "  ERROR: " << stats.errorMessage << std::endl;
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
                std::cout << "  ERROR: Corrupted at index=" << index << ", Width=" << evt->Width
                          << ", Height=" << evt->Height << ", Padding=0x" << std::hex << evt->Padding << std::dec
                          << std::endl;
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
    std::cout << "  Produced: " << TOTAL_MESSAGES << " messages" << std::endl;
    std::cout << "  Producer: " << prodDurationMs << " ms (" << (TOTAL_MESSAGES / prodDurationMs * 1000) << " msg/s)"
              << std::endl;
    std::cout << "  Consumer: " << consDurationMs << " ms (" << (consumed / consDurationMs * 1000) << " msg/s)"
              << std::endl;

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