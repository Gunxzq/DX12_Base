#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/Logger/LoggerConfig.h"

namespace DX12Engine {
namespace Core {

// ========================================================================
// ConfigManager 类定义
// ========================================================================

class ConfigManager {
public:
    // 回调类型定义
    using ConfigChangeCallback = std::function<void(const std::string &section)>;

    // --- 1. 单例与生命周期 ---

    /**
     * @brief 获取单例实例
     */
    static ConfigManager &GetInstance();

    /**
     * @brief 初始化配置管理器
     * @param userConfigPath 用户配置文件路径 (如 config/user_settings.json)
     * @param defaultConfigPath 默认配置文件路径 (可选, 如 config/default_settings.json)
     */
    void Initialize(const std::filesystem::path &userConfigPath, const std::filesystem::path &defaultConfigPath = "");

    /**
     * @brief 关闭并强制保存配置
     */
    void Shutdown();

    // 禁止拷贝和移动
    ConfigManager(const ConfigManager &) = delete;
    ConfigManager &operator=(const ConfigManager &) = delete;
    ConfigManager(ConfigManager &&) = delete;
    ConfigManager &operator=(ConfigManager &&) = delete;

    // --- 2. 读取配置 (Read) ---

    /**
     * @brief 获取日志配置 (只读引用)
     */
    const LogConfig &GetLogConfig() const;

    // --- 3. 修改配置 (Write) ---

    /**
     * @brief 设置日志全局级别
     */
    void SetLogGlobalLevel(LogLevel level);

    /**
     * @brief 设置日志目录
     */
    void SetLogDirectory(const std::string &dir);

    // --- 4. 持久化 (Persist) ---

    /**
     * @brief 手动触发保存
     */
    void Save();

    // --- 5. 热重载 (Reload) ---

    /**
     * @brief 重新从磁盘加载配置
     */
    void Reload();

    // --- 6. 每帧更新 (Update) ---

    /**
     * @brief 在主循环中调用，处理节流自动保存
     * @param deltaTime 帧间隔时间(秒)
     */
    void Update(float deltaTime);

    // --- 7. 订阅者模式 (Observer) ---

    /**
     * @brief 订阅配置变更事件
     * @param section 配置节名称 (如 "Log")
     * @param callback 变更回调
     */
    void Subscribe(const std::string &section, ConfigChangeCallback callback);

private:
    ConfigManager();
    ~ConfigManager();

    // --- 内部辅助方法 ---

    /**
     * @brief 加载并合并 JSON 文件
     */
    void LoadAndMergeConfigs(const std::filesystem::path &userPath, const std::filesystem::path &defaultPath);

    /**
     * @brief 将内存中的 Struct 同步到 JSON 对象 (准备保存)
     */
    void SyncStructsToJson();

    /**
     * @brief 将 JSON 对象解析到内存 Struct
     */
    void ParseJsonToStructs(const nlohmann::json &j);

    /**
     * @brief 原子写入文件 (防止损坏)
     */
    bool AtomicWriteFile(const std::filesystem::path &targetPath, const std::string &content);

    /**
     * @brief 通知订阅者
     */
    void NotifySubscribers(const std::string &section);

    // --- 成员变量 ---

    // 互斥锁: 保护配置数据的读写 (std::shared_mutex 允许并发读)
    mutable std::shared_mutex m_mutex;

    // 强类型配置实例
    LogConfig m_logConfig;

    // 原始 JSON 数据
    nlohmann::json m_configData;

    // 状态标记
    bool m_isInitialized = false;
    bool m_isDirty = false;

    // 时间追踪
    std::chrono::steady_clock::time_point m_lastModifyTime;
    std::chrono::steady_clock::time_point m_lastSaveTime;

    // 配置路径
    std::filesystem::path m_userConfigPath;
    std::filesystem::path m_defaultConfigPath;

    // 节流阈值
    static constexpr float SAVE_THRESHOLD_SECONDS = 5.0f;

    // 订阅者列表: Key = Section Name
    std::unordered_map<std::string, std::vector<ConfigChangeCallback>> m_subscribers;
};

} // namespace Core
} // namespace DX12Engine