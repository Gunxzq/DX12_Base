#include "Core/Config/ConfigManager.h"
#include <fstream>
#include <iostream>
#include <spdlog/spdlog.h> // 假设使用 spdlog 进行内部日志记录，或者使用 std::cout

namespace DX12Engine {
namespace Core {

// 静态成员初始化 (如果需要，但这里 GetInstance 使用 local static)
ConfigManager &ConfigManager::GetInstance() {
    static ConfigManager instance;
    return instance;
}

ConfigManager::ConfigManager()
    : m_lastModifyTime(std::chrono::steady_clock::now()), m_lastSaveTime(std::chrono::steady_clock::now()) {}

ConfigManager::~ConfigManager() {
    if (m_isInitialized) {
        Shutdown();
    }
}

/**
 * @brief 初始化配置管理器
 * @param userConfigPath 用户配置文件路径
 * @param defaultConfigPath 默认配置文件路径
 * @date 2026-04-18
 */
void ConfigManager::Initialize(const std::filesystem::path &userConfigPath,
                               const std::filesystem::path &defaultConfigPath) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    if (m_isInitialized) {
        return;
    }

    m_userConfigPath = userConfigPath;
    m_defaultConfigPath = defaultConfigPath;

    LoadAndMergeConfigs(userConfigPath, defaultConfigPath);

    m_isInitialized = true;
    m_isDirty = false;
    m_lastSaveTime = std::chrono::steady_clock::now();
}

/**
 * @brief 关闭配置管理器
 * @date 2026-04-18
 */
void ConfigManager::Shutdown() {
    Save();
    m_isInitialized = false;
}

/**
 * @brief 获取日志配置
 * @return const LogConfig&
 * @date 2026-04-18
 */
const LogConfig &ConfigManager::GetLogConfig() const {

    // 锁
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_logConfig;
}

/**
 * @brief 设置日志全局等级
 * @param level
 * @date 2026-04-18
 */
void ConfigManager::SetLogGlobalLevel(LogLevel level) {
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        if (m_logConfig.GlobalLevel != level) {
            m_logConfig.GlobalLevel = level;
            m_isDirty = true;
            m_lastModifyTime = std::chrono::steady_clock::now();

            // 同步到 JSON 以便保存
            SyncStructsToJson();
        }
    }
    NotifySubscribers("Log");
}

void ConfigManager::SetLogDirectory(const std::string &dir) {
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        if (m_logConfig.LogDirectory != dir) {
            m_logConfig.LogDirectory = dir;
            m_isDirty = true;
            m_lastModifyTime = std::chrono::steady_clock::now();
            SyncStructsToJson();
        }
    }
    NotifySubscribers("Log");
}

void ConfigManager::Save() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (!m_isDirty) {
        return;
    }

    // 确保 JSON 数据与 Struct 同步
    SyncStructsToJson();

    // 序列化 JSON
    std::string content = m_configData.dump(4); // 4 spaces indent

    if (AtomicWriteFile(m_userConfigPath, content)) {
        m_isDirty = false;
        m_lastSaveTime = std::chrono::steady_clock::now();
    } else {
        // 输出到控制台
        std::cout << "Failed to save config to file: " << m_userConfigPath << std::endl;
    }
}

void ConfigManager::Reload() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    // 如果有未保存的更改，先保存或警告
    if (m_isDirty) {
        // 这里选择先保存，防止丢失数据
        lock.unlock();
        Save();
        lock.lock();
    }

    LoadAndMergeConfigs(m_userConfigPath, m_defaultConfigPath);

    // 通知所有订阅者
    for (auto &pair : m_subscribers) {
        NotifySubscribers(pair.first);
    }
}

void ConfigManager::Update(float deltaTime) {
    if (!m_isInitialized || !m_isDirty) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<float> elapsed = now - m_lastSaveTime;

    if (elapsed.count() > SAVE_THRESHOLD_SECONDS) {
        // 解锁后保存，避免持有锁进行 IO
        m_mutex.unlock();
        Save();
        m_mutex.lock();
    }
}

void ConfigManager::Subscribe(const std::string &section, ConfigChangeCallback callback) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_subscribers[section].push_back(std::move(callback));
}

// --- Private Methods ---

void ConfigManager::LoadAndMergeConfigs(const std::filesystem::path &userPath,
                                        const std::filesystem::path &defaultPath) {
    nlohmann::json mergedJson;

    // 1. 加载默认配置
    if (!defaultPath.empty() && std::filesystem::exists(defaultPath)) {
        try {
            std::ifstream ifs(defaultPath);
            mergedJson = nlohmann::json::parse(ifs);
        } catch (const std::exception &e) {
            // 记录错误
        }
    } else {
        // 如果没有默认文件，初始化为空对象或包含默认值的对象
        mergedJson = {};
    }

    // 2. 加载用户配置并合并
    if (std::filesystem::exists(userPath)) {
        try {
            std::ifstream ifs(userPath);
            nlohmann::json userJson = nlohmann::json::parse(ifs);
            mergedJson.merge_patch(userJson); // merge_patch 会递归合并
        } catch (const std::exception &e) {

            // 记录错误，用户配置损坏，回退到默认
        }
    }

    m_configData = mergedJson;
    ParseJsonToStructs(m_configData);
}

void ConfigManager::SyncStructsToJson() {
    nlohmann::json logJson = m_logConfig;
    m_configData["Log"] = logJson;
}

void ConfigManager::ParseJsonToStructs(const nlohmann::json &j) {
    // 从 JSON 对象读取到强类型结构体
    if (j.contains("Log")) {
        try {
            m_logConfig = j["Log"].get<LogConfig>();
        } catch (const std::exception &e) {
            // 解析失败，保持默认值或记录错误
        }
    } else {
        // 如果 JSON 中没有 Log 节，保持 m_logConfig 的默认构造函数值
    }

    // 如果有其他配置，继续解析
}

bool ConfigManager::AtomicWriteFile(const std::filesystem::path &targetPath, const std::string &content) {
    std::filesystem::path tempPath = targetPath;
    tempPath += ".tmp";

    try {
        // 确保目录存在
        std::filesystem::create_directories(targetPath.parent_path());

        // 写入临时文件
        std::ofstream ofs(tempPath, std::ios::trunc);
        if (!ofs.is_open()) {
            return false;
        }
        ofs << content;
        ofs.flush();
        ofs.close();

        // 原子替换
        std::filesystem::rename(tempPath, targetPath);
        return true;
    } catch (...) {
        // 清理临时文件
        if (std::filesystem::exists(tempPath)) {
            std::filesystem::remove(tempPath);
        }
        return false;
    }
}

void ConfigManager::NotifySubscribers(const std::string &section) {
    // 注意：调用此函数时通常已经持有锁或刚释放锁
    // 为了避免死锁，最好在锁外调用，或者确保回调不尝试再次获取同一把锁
    auto it = m_subscribers.find(section);
    if (it != m_subscribers.end()) {
        for (auto &cb : it->second) {
            if (cb) {
                cb(section);
            }
        }
    }
}

} // namespace Core
} // namespace DX12Engine