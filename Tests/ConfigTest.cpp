#include "Core/Config/ConfigManager.h"
#include "Core/Config/LoggerConfig.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace DX12Engine::Core;

// ========================================================================
// 测试套件: LogConfigSerialization
// ========================================================================

TEST(LogConfigTest, DeserializeFromManualJson) {
    // 1. 手动构建一个标准的 JSON 对象 (模拟从文件读取的内容)
    // 这样测试不依赖于 NLOHMANN_DEFINE_TYPE_INTRUSIVE 宏的具体行为，只关注数据映射
    json j;
    j["GlobalLevel"] = "debug";
    j["FlushLevel"] = "warn";
    j["FormatPattern"] = "[%l] %v";

    // 嵌套结构
    j["Sinks"]["Console"]["Enabled"] = false;
    j["Sinks"]["Console"]["Level"] = "info";
    j["Sinks"]["Console"]["Colorize"] = true;

    j["Sinks"]["File"]["Enabled"] = true;
    j["Sinks"]["File"]["Path"] = "TestLogs/custom.log";
    j["Sinks"]["File"]["Level"] = "trace";
    j["Sinks"]["File"]["Rotation"]["Policy"] = "size";
    j["Sinks"]["File"]["Rotation"]["MaxSizeMb"] = 20;
    j["Sinks"]["File"]["Rotation"]["MaxFiles"] = 5;

    j["Sinks"]["Async"]["Enabled"] = true;
    j["Sinks"]["Async"]["QueueSize"] = 8192;
    j["Sinks"]["Async"]["OverflowPolicy"] = "discard";

    // 2. 尝试反序列化
    // 如果 LogConfig 没有正确定义 from_json (无论是通过宏还是手动)，这里会编译错误或运行时异常
    ASSERT_NO_THROW({
        LogConfig config = j.get<LogConfig>();

        // 3. 验证反序列化后的数据
        EXPECT_EQ(config.GlobalLevel, LogLevel::Debug);
        EXPECT_EQ(config.FlushLevel, LogLevel::Warn);
        EXPECT_EQ(config.FormatPattern, "[%l] %v");

        EXPECT_FALSE(config.Sinks.Console.Enabled);
        EXPECT_EQ(config.Sinks.Console.Level, LogLevel::Info);

        EXPECT_TRUE(config.Sinks.File.Enabled);
        EXPECT_EQ(config.Sinks.File.Path, "TestLogs/custom.log");
        EXPECT_EQ(config.Sinks.File.Level, LogLevel::Trace);
        EXPECT_EQ(config.Sinks.File.Rotation.MaxSizeMb, 20);

        EXPECT_TRUE(config.Sinks.Async.Enabled);
        EXPECT_EQ(config.Sinks.Async.QueueSize, 8192);
    });
}

TEST(LogConfigTest, SerializeToManualJsonCheck) {
    // 1. 准备 C++ 结构体数据
    LogConfig original;
    original.GlobalLevel = LogLevel::Error;
    original.Sinks.File.Enabled = true;
    original.Sinks.File.Path = "logs/test.log";
    original.Sinks.Console.Enabled = false;

    // 2. 序列化为 JSON
    json j;
    ASSERT_NO_THROW({
        j = original; // 依赖 to_json (由宏或手动定义提供)
    });

    // 3. 手动验证 JSON 关键字段是否符合预期
    // 这里我们只关心关键业务字段，而不关心 JSON 的所有细节
    EXPECT_EQ(j["GlobalLevel"], "error");
    EXPECT_EQ(j["Sinks"]["File"]["Path"], "logs/test.log");
    EXPECT_EQ(j["Sinks"]["File"]["Enabled"], true);
    EXPECT_EQ(j["Sinks"]["Console"]["Enabled"], false);
}

// ========================================================================
// 测试套件: ConfigManager Integration
// ========================================================================

TEST(ConfigManagerTest, GetInstanceDoesNotCrash) {
    auto &instance = DX12Engine::Core::ConfigManager::GetInstance();
    EXPECT_NO_THROW(instance.GetLogConfig());
}

TEST(ConfigManagerTest, SaveAndLoadFromFile) {
    // 1. 准备临时文件路径
    std::filesystem::path testDir = std::filesystem::current_path() / "TestOutput_Config";
    std::filesystem::create_directories(testDir);
    std::filesystem::path userConfigPath = testDir / "user_test_config.json";

    // 清理旧文件
    if (std::filesystem::exists(userConfigPath)) {
        std::filesystem::remove(userConfigPath);
    }

    // 2. 获取单例并初始化
    auto &manager = DX12Engine::Core::ConfigManager::GetInstance();

    EXPECT_NO_THROW(manager.Initialize(userConfigPath));

    // 3. 修改配置 (在内存中)
    manager.SetLogGlobalLevel(LogLevel::Critical);
    manager.SetLogDirectory("MyCustomLogs/integration_test.log");

    // 4. 手动触发保存
    EXPECT_NO_THROW(manager.Save());

    // 5. 验证文件是否存在
    ASSERT_TRUE(std::filesystem::exists(userConfigPath)) << "配置文件未生成！";

    // 6. 读取文件内容验证
    std::ifstream fileStream(userConfigPath);
    ASSERT_TRUE(fileStream.is_open()) << "无法打开生成的配置文件进行验证！";

    json savedJson;
    fileStream >> savedJson;
    fileStream.close();

    // 7. 验证内容
    // 假设 ConfigManager 将日志配置保存在 "logging" 键下
    if (savedJson.contains("logging")) {
        const auto &logNode = savedJson["logging"];

        // 验证全局级别
        EXPECT_EQ(logNode["GlobalLevel"], "critical");

        // 验证文件路径
        EXPECT_EQ(logNode["Sinks"]["File"]["Path"], "MyCustomLogs/integration_test.log");
    } else {
        FAIL() << "JSON 结构中未找到 'logging' 节点。实际内容: " << savedJson.dump(4);
    }

    // 8. 清理
    std::filesystem::remove(userConfigPath);
    if (std::filesystem::is_empty(testDir)) {
        std::filesystem::remove(testDir);
    }
}