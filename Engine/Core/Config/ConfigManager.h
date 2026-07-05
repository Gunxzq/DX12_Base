#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace DX12Engine {
namespace Boot {

// ========================================================================
// ConfigManager — 配置加载工具
//
// 工具 API: LoadJSON<T> / LoadINI<T>，调用方持有数据，无状态
// 托管 API: Register / Reload / Save / Subscribe，两种格式均支持
// ========================================================================

class ConfigManager {
public:
    using ConfigChangeCallback = std::function<void(const std::string &section)>;

    enum class ConfigFormat { JSON, INI };

    struct ConfigEntry {
        std::filesystem::path path;
        ConfigFormat format = ConfigFormat::JSON;
        bool enableHotReload = false;
    };

    // --- 1. 单例与生命周期 ---

    static ConfigManager &GetInstance();
    void Initialize(const std::filesystem::path &configDir);
    void Shutdown();

    ConfigManager();
    ~ConfigManager();

    ConfigManager(const ConfigManager &) = delete;
    ConfigManager &operator=(const ConfigManager &) = delete;
    ConfigManager(ConfigManager &&) = delete;
    ConfigManager &operator=(ConfigManager &&) = delete;

    // --- 2. 工具 API（无状态） ---

    template <typename T> static T LoadJSON(const std::filesystem::path &path) {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open())
            throw std::runtime_error("ConfigManager: cannot open " + path.string());
        nlohmann::json j = nlohmann::json::parse(ifs);
        return j.template get<T>();
    }

    template <typename T> static T LoadJSON(std::span<const uint8_t> data) {
        nlohmann::json j = nlohmann::json::parse(data);
        return j.template get<T>();
    }

    template <typename T> static T LoadINI(const std::filesystem::path &path) {
        auto j = ParseINI(path);
        return j.template get<T>();
    }

    template <typename T> static T LoadINI(std::span<const uint8_t> data) {
        auto j = ParseINI(data);
        return j.template get<T>();
    }

    // --- 3. 托管 API ---

    void Register(const std::string &key, const ConfigEntry &entry,
                  std::function<void(const nlohmann::json &)> applier);

    void Reload(const std::string &key);
    void Save(const std::string &key);
    void SaveAll();
    void Update(float deltaTime);
    void Subscribe(const std::string &section, ConfigChangeCallback callback);
    const nlohmann::json &GetJSON(const std::string &key) const;

private:
    void LoadEntry_Locked(const std::string &key, const ConfigEntry &entry);
    bool SaveEntry_Locked(const std::string &key, const ConfigEntry &entry, const nlohmann::json &data);
    void NotifySubscribers_Unlocked(const std::string &section);
    bool AtomicWriteFile(const std::filesystem::path &targetPath, const std::string &content);

    // INI 解析
    static nlohmann::json ParseINI(const std::filesystem::path &path);
    static nlohmann::json ParseINI(std::span<const uint8_t> data);

    mutable std::shared_mutex m_mutex;

    std::unordered_map<std::string, ConfigEntry> m_entries;
    std::unordered_map<std::string, nlohmann::json> m_configData;
    std::unordered_map<std::string, std::function<void(const nlohmann::json &)>> m_appliers;
    std::unordered_map<std::string, std::vector<ConfigChangeCallback>> m_subscribers;

    bool m_isInitialized = false;
    bool m_isDirty = false;

    std::chrono::steady_clock::time_point m_lastModifyTime; // 最后修改时间
    std::chrono::steady_clock::time_point m_lastSaveTime;   // 最后保存时间

    std::filesystem::path m_configDir;

    static constexpr float SAVE_THRESHOLD_SECONDS = 5.0f;
};

} // namespace Boot
} // namespace DX12Engine
