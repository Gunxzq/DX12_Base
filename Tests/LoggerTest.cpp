#include "Core/Logger/Logger.h"
#include "Core/Logger/LoggerConfig.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

// 辅助函数：清理测试产生的日志文件
void CleanupLogFiles(const std::string &path) {
    if (std::filesystem::exists(path)) {
        std::filesystem::remove_all(path);
    }
}

// ========================================================================
// 测试套件: LoggerInitialization
// ========================================================================

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 每个测试前确保 Logger 处于关闭状态
        DX12Engine::Core::Logger::Shutdown();
        // 清理可能的残留日志目录
        CleanupLogFiles("TestLogs");
    }

    void TearDown() override {
        DX12Engine::Core::Logger::Shutdown();
        CleanupLogFiles("TestLogs");
    }
};

// 测试用例1：正常初始化（启用控制台和文件）
TEST_F(LoggerTest, InitWithValidConfig) {
    DX12Engine::Core::LogConfig config;
    config.GlobalLevel = DX12Engine::Core::LogLevel::Debug;
    config.Sinks.Console.Enabled = true;
    config.Sinks.File.Enabled = true;
    config.Sinks.File.Path = "TestLogs/engine_test.log";
    config.Sinks.Async.Enabled = false; // 同步模式便于测试

    EXPECT_NO_THROW(DX12Engine::Core::Logger::Init(config));

    // 验证 Logger 实例有效
    auto logger = DX12Engine::Core::Logger::GetInstance();
    EXPECT_NE(logger, nullptr);
    EXPECT_EQ(logger->name(), "engine_logger");

    // 验证文件是否创建
    EXPECT_TRUE(std::filesystem::exists("TestLogs/engine_test.log"));
}

// 测试用例2：空 Sink 健壮性（所有 Sink 禁用）
TEST_F(LoggerTest, InitWithNoSinksUsesNullSink) {
    DX12Engine::Core::LogConfig config;
    config.GlobalLevel = DX12Engine::Core::LogLevel::Info;
    config.Sinks.Console.Enabled = false;
    config.Sinks.File.Enabled = false;
    config.Sinks.Async.Enabled = false;

    // 即使没有启用任何 Sink，也不应崩溃
    EXPECT_NO_THROW(DX12Engine::Core::Logger::Init(config));

    auto logger = DX12Engine::Core::Logger::GetInstance();
    EXPECT_NE(logger, nullptr);

    // 尝试记录日志，不应崩溃
    EXPECT_NO_THROW(SPDLOG_INFO("This should be discarded by null sink"));
}

// 测试用例3：异步 Logger 初始化
TEST_F(LoggerTest, InitAsyncLogger) {
    DX12Engine::Core::LogConfig config;
    config.GlobalLevel = DX12Engine::Core::LogLevel::Info;
    config.Sinks.Console.Enabled = true;
    config.Sinks.File.Enabled = true;
    config.Sinks.File.Path = "TestLogs/async_test.log";
    config.Sinks.Async.Enabled = true;
    config.Sinks.Async.QueueSize = 4096;
    config.Sinks.Async.OverflowPolicy = "discard";

    EXPECT_NO_THROW(DX12Engine::Core::Logger::Init(config));

    auto logger = DX12Engine::Core::Logger::GetInstance();
    EXPECT_NE(logger, nullptr);

    // 异步模式下，日志可能不会立即落盘，但实例应有效
    SPDLOG_INFO("Async log message");

    // 强制刷盘以确保写入
    logger->flush();
    EXPECT_TRUE(std::filesystem::exists("TestLogs/async_test.log"));
}

// 测试用例4：目录自动创建
TEST_F(LoggerTest, AutoCreateLogDirectory) {
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

// 测试用例5：Shutdown 安全性
TEST_F(LoggerTest, ShutdownSafety) {
    DX12Engine::Core::LogConfig config;
    config.Sinks.Console.Enabled = true;
    DX12Engine::Core::Logger::Init(config);

    // 第一次关闭
    EXPECT_NO_THROW(DX12Engine::Core::Logger::Shutdown());

    // 第二次关闭（幂等性）
    EXPECT_NO_THROW(DX12Engine::Core::Logger::Shutdown());

    // 关闭后获取实例（应返回 nullptr 或无效状态，取决于实现，这里期望不崩溃）
    // 注意：当前实现 Shutdown 后 s_logger 重置为 nullptr
    auto logger = DX12Engine::Core::Logger::GetInstance();
    EXPECT_EQ(logger, nullptr);
}