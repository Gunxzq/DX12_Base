#include "Core/Config/ConfigManager.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

// 测试套件: LogConfigSerialization
// 测试用例: BasicSerializeDeserialize

// 测试用例1：基本序列化和反序列化
TEST(LogConfigTest, SerializeAndDeserialize) {
    // 1. 准备数据
    DX12Engine::Core::LogConfig original;
    original.GlobalLevel = DX12Engine::Core::LogLevel::Debug;
    original.LogDirectory = "TestLogs";
    original.EnableFileLogging = false;

    // 2. 序列化为 JSON
    nlohmann::json j;
    j = original; // 显式赋值，触发 to_json

    // 3. 验证 JSON 内容是否符合预期
    EXPECT_EQ(j["GlobalLevel"], 1); // Debug 枚举值为 1
    EXPECT_EQ(j["LogDirectory"], "TestLogs");
    EXPECT_EQ(j["EnableFileLogging"], false);

    // 4. 从 JSON 反序列化回结构体
    DX12Engine::Core::LogConfig restored = j.get<DX12Engine::Core::LogConfig>();

    // 5. 验证恢复后的数据与原数据一致
    EXPECT_EQ(restored.GlobalLevel, original.GlobalLevel);
    EXPECT_EQ(restored.LogDirectory, original.LogDirectory);
    EXPECT_EQ(restored.EnableFileLogging, original.EnableFileLogging);
}

// 测试用例2：单例获取不崩溃
TEST(ConfigManagerTest, GetInstanceDoesNotCrash) {
    // 简单测试单例获取是否崩溃
    auto &instance = DX12Engine::Core::ConfigManager::GetInstance();
    EXPECT_NO_THROW(instance.GetLogConfig());
}

// 测试用例3：保存和加载到文件
TEST(ConfigManagerTest, SaveAndLoadFromFile) {
    // 1. 准备临时文件路径
    std::filesystem::path testDir = std::filesystem::current_path() / "TestOutput";
    std::filesystem::create_directories(testDir);

    std::filesystem::path userConfigPath = testDir / "user_test_config.json";

    // 清理旧文件
    if (std::filesystem::exists(userConfigPath)) {
        std::filesystem::remove(userConfigPath);
    }

    // 2. 获取单例并初始化
    auto &manager = DX12Engine::Core::ConfigManager::GetInstance();

    // 初始化
    EXPECT_NO_THROW(manager.Initialize(userConfigPath));

    // 3. 修改配置 (在内存中)
    manager.SetLogGlobalLevel(DX12Engine::Core::LogLevel::Error);
    manager.SetLogDirectory("MyCustomLogs");

    // 4. 手动触发保存
    EXPECT_NO_THROW(manager.Save());

    // 5. 验证文件是否存在
    ASSERT_TRUE(std::filesystem::exists(userConfigPath)) << "配置文件未生成！";

    // 6. 读取文件内容验证 (这里用到了 ifstream，所以必须 #include <fstream>)
    std::ifstream fileStream(userConfigPath);
    ASSERT_TRUE(fileStream.is_open()) << "无法打开生成的配置文件进行验证！";

    nlohmann::json savedJson;
    fileStream >> savedJson;
    fileStream.close();

    // 7. 验证内容 (根据 ConfigManager.cpp 中的 SyncStructsToJson，数据在 "Log" 节点下)
    if (savedJson.contains("Log")) {
        EXPECT_EQ(savedJson["Log"]["GlobalLevel"], 4); // Error = 4
        EXPECT_EQ(savedJson["Log"]["LogDirectory"], "MyCustomLogs");
    } else {
        FAIL() << "JSON 结构中未找到 'Log' 节点";
    }

    // 8. 清理
    std::filesystem::remove(userConfigPath);
    if (std::filesystem::is_empty(testDir)) {
        std::filesystem::remove(testDir);
    }
}