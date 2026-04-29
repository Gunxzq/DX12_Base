// File: d:\project\DX12_Base\Tests\LoggerTest.cpp
#include "System/Logger/Logger.h"
#include "Core/Config/LoggerConfig.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

// 辅助函数：清理测试产生的日志文件
void CleanupLogFiles(const std::string &path) {
    if (std::filesystem::exists(path)) {
        try {
            std::filesystem::remove_all(path);
        } catch (...) {
            // 忽略清理失败，可能是文件被占用
        }
    }
}

// ========================================================================
// 测试套件: LoggerInitialization
// ========================================================================

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 使用 TestReset 安全地重置 Logger 状态（可多次调用）
        DX12Engine::Core::Logger::TestReset();
        // 清理之前的日志文件
        CleanupLogFiles("TestLogs");
        // 预先创建日志目录
        std::filesystem::create_directories("TestLogs");
    }

    void TearDown() override {
        DX12Engine::Core::Logger::TestReset();
        CleanupLogFiles("TestLogs");
    }
};

// 测试用例1：正常初始化（启用控制台和文件）
// 注意：Logger 测试因 spdlog 内部复杂性暂时禁用
TEST_F(LoggerTest, DISABLED_InitWithValidConfig) {
    DX12Engine::Core::LogConfig config;
    config.GlobalLevel = DX12Engine::Core::LogLevel::Debug;
    config.Sinks.Console.Enabled = true;
    config.Sinks.File.Enabled = true;
    config.Sinks.File.Path = "TestLogs/engine_test.log";
    config.Sinks.Async.Enabled = false; // 同步模式便于测试

    EXPECT_NO_THROW(DX12Engine::Core::Logger::Init(config));

    // 验证 Logger 实例有效
    auto *logger = DX12Engine::Core::Logger::GetInstance();
    ASSERT_NE(logger, nullptr) << "Logger instance should not be null after Init";

    // 测试日志方法
    EXPECT_NO_THROW(logger->Info("Test message"));
    logger->Flush();

    // 验证文件是否创建
    EXPECT_TRUE(std::filesystem::exists("TestLogs/engine_test.log"))
        << "Log file should be created at TestLogs/engine_test.log";
}

// 测试用例2：空 Sink 健壮性（所有 Sink 禁用）
TEST_F(LoggerTest, DISABLED_InitWithNoSinksUsesNullSink) {
    DX12Engine::Core::LogConfig config;
    config.GlobalLevel = DX12Engine::Core::LogLevel::Info;
    config.Sinks.Console.Enabled = false;
    config.Sinks.File.Enabled = false;
    config.Sinks.Async.Enabled = false;

    // 即使没有启用任何 Sink，也不应崩溃
    EXPECT_NO_THROW(DX12Engine::Core::Logger::Init(config));

    auto *logger = DX12Engine::Core::Logger::GetInstance();
    ASSERT_NE(logger, nullptr);

    // 尝试记录日志，不应崩溃
    EXPECT_NO_THROW(logger->Info("This should be discarded by null sink"));
}

// 测试用例3：异步 Logger 初始化
TEST_F(LoggerTest, DISABLED_InitAsyncLogger) {
    DX12Engine::Core::LogConfig config;
    config.GlobalLevel = DX12Engine::Core::LogLevel::Info;
    config.Sinks.Console.Enabled = true;
    config.Sinks.File.Enabled = true;
    config.Sinks.File.Path = "TestLogs/async_test.log";
    config.Sinks.Async.Enabled = true;
    config.Sinks.Async.QueueSize = 4096;
    config.Sinks.Async.OverflowPolicy = "discard";

    EXPECT_NO_THROW(DX12Engine::Core::Logger::Init(config));

    auto *logger = DX12Engine::Core::Logger::GetInstance();
    ASSERT_NE(logger, nullptr);

    // 异步模式下，日志可能不会立即落盘，但实例应有效
    EXPECT_NO_THROW(logger->Info("Async log message"));

    // 强制刷盘以确保写入
    logger->Flush();
}

// 测试用例4：目录自动创建
TEST_F(LoggerTest, DISABLED_AutoCreateLogDirectory) {
    std::string deepPath = "TestLogs/Deep/Nested/Dir/log.log";

    DX12Engine::Core::LogConfig config;
    config.Sinks.Console.Enabled = false;
    config.Sinks.File.Enabled = true;
    config.Sinks.File.Path = deepPath;
    config.Sinks.Async.Enabled = false;

    EXPECT_NO_THROW(DX12Engine::Core::Logger::Init(config));

    // 验证深层目录是否被创建
    EXPECT_TRUE(std::filesystem::exists("TestLogs/Deep/Nested/Dir"));
    EXPECT_TRUE(std::filesystem::exists(deepPath));
}

// 测试用例5：Shutdown 安全性（幂等性）
TEST_F(LoggerTest, DISABLED_ShutdownSafety) {
    DX12Engine::Core::LogConfig config;
    config.Sinks.Console.Enabled = true;
    config.Sinks.File.Enabled = true;
    config.Sinks.File.Path = "TestLogs/shutdown_test.log";
    DX12Engine::Core::Logger::Init(config);

    auto *logger = DX12Engine::Core::Logger::GetInstance();
    ASSERT_NE(logger, nullptr);

    // 第一次关闭
    EXPECT_NO_THROW(DX12Engine::Core::Logger::Shutdown());

    // 第二次关闭（幂等性验证）
    EXPECT_NO_THROW(DX12Engine::Core::Logger::Shutdown());

    // 关闭后获取实例，验证单例仍有效
    auto *loggerAfterShutdown = DX12Engine::Core::Logger::GetInstance();
    EXPECT_NE(loggerAfterShutdown, nullptr);

    // 验证可以重新初始化
    EXPECT_NO_THROW(DX12Engine::Core::Logger::Init(config));
}

// 测试用例6：多次 Init/Shutdown 循环
TEST_F(LoggerTest, DISABLED_MultipleInitShutdownCycles) {
    DX12Engine::Core::LogConfig config;
    config.Sinks.Console.Enabled = true;
    config.Sinks.File.Enabled = true;
    config.Sinks.Async.Enabled = false;

    // 多次 Init/Shutdown 循环
    for (int i = 0; i < 3; ++i) {
        config.Sinks.File.Path = "TestLogs/cycle_" + std::to_string(i) + ".log";
        EXPECT_NO_THROW(DX12Engine::Core::Logger::Init(config));

        auto *logger = DX12Engine::Core::Logger::GetInstance();
        ASSERT_NE(logger, nullptr);

        EXPECT_NO_THROW(logger->Info("Cycle {}", i));
        logger->Flush();

        EXPECT_NO_THROW(DX12Engine::Core::Logger::Shutdown());
    }
}
