#include "Core/Config/ConfigManager.h"

#include "Common/Common.h"
#include <fstream>

namespace DX12Engine::Boot {

ConfigManager &ConfigManager::GetInstance() {
    static ConfigManager instance;
    return instance;
}

ConfigManager::ConfigManager()
    : m_lastModifyTime(std::chrono::steady_clock::now()), m_lastSaveTime(std::chrono::steady_clock::now()) {}

ConfigManager::~ConfigManager() {
    if (m_isInitialized) {
        try {
            Shutdown();
        } catch (const std::exception &e) {
            ErrorReporter::Fatal("ConfigManager Destructor Exception: %s", e.what());
        }
    }
}

void ConfigManager::Initialize(const std::filesystem::path &configDir) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    if (m_isInitialized) {
        return;
    }

    // 验证并创建目录
    if (!std::filesystem::exists(configDir)) {
        try {
            std::filesystem::create_directories(configDir);
        } catch (const std::filesystem::filesystem_error &e) {
            ErrorReporter::Fatal("Failed to create config directory: %s", e.what());
            throw std::runtime_error(std::string("Failed to create config directory: ") + e.what());
        }
    }

    m_configDir = configDir;
    m_isInitialized = true;
    m_isDirty = false;
    m_lastSaveTime = std::chrono::steady_clock::now();
}

void ConfigManager::Shutdown() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    if (!m_isInitialized) {
        return;
    }

    // 保存所有脏配置
    if (m_isDirty) {
        for (auto &[key, entry] : m_entries) {
            auto it = m_configData.find(key);
            if (it != m_configData.end() && m_isDirty) {
                SaveEntry_Locked(key, entry, it->second);
            }
        }
    }

    m_isInitialized = false;
    m_isDirty = false;
    m_entries.clear();
    m_configData.clear();
    m_appliers.clear();
    m_subscribers.clear();
}

// --- 托管 API ---

void ConfigManager::Register(const std::string &key, const ConfigEntry &entry,
                             std::function<void(const nlohmann::json &)> applier) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    if (!m_isInitialized) {
        ErrorReporter::Fatal("%s", "ConfigManager::Register called before Initialize");
        return;
    }

    m_entries[key] = entry;
    if (applier) {
        m_appliers[key] = std::move(applier);
    }

    // 注册时立即加载
    LoadEntry_Locked(key, entry);
}

void ConfigManager::Reload(const std::string &key) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    auto entryIt = m_entries.find(key);
    if (entryIt == m_entries.end()) {
        ErrorReporter::Fatal("ConfigManager::Reload: key '%s' not registered", key.c_str());
        return;
    }

    LoadEntry_Locked(key, entryIt->second);
}

void ConfigManager::LoadEntry_Locked(const std::string &key, const ConfigEntry &entry) {
    // REQUIRES: m_mutex is held by caller

    if (!std::filesystem::exists(entry.path)) {
        if (entry.format == ConfigFormat::INI) {
            // INI 文件缺失或格式错误时直接抛错，不静默回退
            ErrorReporter::Fatal("Config file not found: %s\n"
                                 "INI 配置文件不可缺失，请检查路径或创建该文件。",
                                 entry.path.string().c_str());
        }
        // 文件不存在：通知订阅者使用默认值（applier 会收到空的 JSON）
        ErrorReporter::Fatal("Config file not found: %s, using defaults", entry.path.string().c_str());
        nlohmann::json emptyJson;
        m_configData[key] = emptyJson;

        auto applierIt = m_appliers.find(key);
        if (applierIt != m_appliers.end() && applierIt->second) {
            applierIt->second(emptyJson);
        }
        return;
    }

    try {
        std::ifstream ifs(entry.path, std::ios::binary);
        if (!ifs.is_open()) {
            throw std::runtime_error("Cannot open file: " + entry.path.string());
        }

        nlohmann::json j;
        if (entry.format == ConfigFormat::JSON) {
            j = nlohmann::json::parse(ifs);
        } else {
            // INI → 先解析为 JSON 再走统一 applier 路径
            ifs.close();
            j = ParseINI(entry.path);
        }

        m_configData[key] = j;

        // 调用 applier 将 JSON 转换为调用方的结构体
        auto applierIt = m_appliers.find(key);
        if (applierIt != m_appliers.end() && applierIt->second) {
            applierIt->second(j);
        }

    } catch (const nlohmann::json::parse_error &e) {
        ErrorReporter::Fatal("JSON syntax error in %s: %s", entry.path.string().c_str(), e.what());
        throw;
    } catch (const std::exception &e) {
        ErrorReporter::Fatal("Failed to load config '%s': %s", key.c_str(), e.what());
        throw;
    }
}

void ConfigManager::Save(const std::string &key) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    auto entryIt = m_entries.find(key);
    if (entryIt == m_entries.end()) {
        ErrorReporter::Fatal("ConfigManager::Save: key '%s' not registered", key.c_str());
        return;
    }

    auto dataIt = m_configData.find(key);
    if (dataIt == m_configData.end()) {
        return;
    }

    if (SaveEntry_Locked(key, entryIt->second, dataIt->second)) {
        m_isDirty = false;
    }
}

void ConfigManager::SaveAll() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    if (!m_isDirty) {
        return;
    }

    for (auto &[key, entry] : m_entries) {
        auto dataIt = m_configData.find(key);
        if (dataIt != m_configData.end()) {
            SaveEntry_Locked(key, entry, dataIt->second);
        }
    }

    m_isDirty = false;
}

bool ConfigManager::SaveEntry_Locked(const std::string &key, const ConfigEntry &entry, const nlohmann::json &data) {
    // REQUIRES: m_mutex is held by caller

    if (entry.format == ConfigFormat::INI) {
        // INI 保存暂未实现
        return false;
    }

    std::string content = data.dump(4);

    if (!AtomicWriteFile(entry.path, content)) {
        ErrorReporter::Fatal("Failed to save config: %s", entry.path.string().c_str());
        return false;
    }

    m_lastSaveTime = std::chrono::steady_clock::now();
    return true;
}

void ConfigManager::Update(float deltaTime) {
    if (!m_isInitialized) {
        return;
    }

    bool shouldSave = false;
    nlohmann::json dirtyData;
    std::string dirtyKey;
    ConfigEntry dirtyEntry;

    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);

        if (!m_isDirty) {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<float> elapsed = now - m_lastSaveTime;

        if (elapsed.count() > SAVE_THRESHOLD_SECONDS && !m_entries.empty()) {
            // 遍历找到第一个脏配置（简化：全部标记为已处理）
            m_isDirty = false;
            m_lastSaveTime = now;

            // 复制需 IO 的数据，在锁外执行写入
            for (auto &[key, entry] : m_entries) {
                auto it = m_configData.find(key);
                if (it != m_configData.end()) {
                    dirtyKey = key;
                    dirtyEntry = entry;
                    dirtyData = it->second;
                    break;
                }
            }

            shouldSave = true;
        }
    }

    if (shouldSave) {
        if (!SaveEntry_Locked(dirtyKey, dirtyEntry, dirtyData)) {
            // 保存失败，重新标记脏
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            m_isDirty = true;
            ErrorReporter::Fatal("%s", "Auto-save failed.");
        }
    }
}

void ConfigManager::Subscribe(const std::string &section, ConfigChangeCallback callback) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_subscribers[section].push_back(std::move(callback));
}

const nlohmann::json &ConfigManager::GetJSON(const std::string &key) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    static const nlohmann::json s_emptyJson;
    auto it = m_configData.find(key);
    if (it == m_configData.end()) {
        return s_emptyJson;
    }
    return it->second;
}

// --- Private Helpers ---

void ConfigManager::NotifySubscribers_Unlocked(const std::string &section) {
    // REQUIRES: m_mutex is NOT held
    auto it = m_subscribers.find(section);
    if (it != m_subscribers.end()) {
        for (auto &cb : it->second) {
            if (cb) {
                try {
                    cb(section);
                } catch (const std::exception &e) {
                    ErrorReporter::Fatal("Subscriber callback exception: %s", e.what());
                }
            }
        }
    }
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

// ========================================================================
// INI 解析器（INI → JSON）
// ========================================================================

static std::string TrimINI(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r");
    if (start == std::string::npos)
        return {};
    size_t end = s.find_last_not_of(" \t\r");
    return s.substr(start, end - start + 1);
}

static nlohmann::json ParseINIImpl(std::istream &stream) {
    nlohmann::json root;
    std::string line, section;
    while (std::getline(stream, line)) {
        line = TrimINI(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line[0] == '[') {
            auto end = line.find(']');
            if (end != std::string::npos)
                section = TrimINI(line.substr(1, end - 1));
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos || section.empty())
            continue;
        std::string key = TrimINI(line.substr(0, eq));
        std::string value = TrimINI(line.substr(eq + 1));

        // 类型推断：bool → number → string
        if (value == "true") {
            root[section][key] = true;
        } else if (value == "false") {
            root[section][key] = false;
        } else {
            char *end = nullptr;
            long longVal = strtoll(value.c_str(), &end, 10);
            if (*end == '\0') {
                // 正数存为 uint64_t（满足 is_number_unsigned 检查）
                // 负数存为 int64_t（保持符号）
                if (longVal >= 0)
                    root[section][key] = static_cast<uint64_t>(longVal);
                else
                    root[section][key] = static_cast<int64_t>(longVal);
                continue;
            }
            double dblVal = strtod(value.c_str(), &end);
            if (*end == '\0') {
                root[section][key] = dblVal;
                continue;
            }
            root[section][key] = value;
        }
    }
    return root;
}

nlohmann::json ConfigManager::ParseINI(const std::filesystem::path &path) {
    std::ifstream ifs(path);
    if (!ifs.is_open())
        throw std::runtime_error("ConfigManager: cannot open " + path.string());
    return ParseINIImpl(ifs);
}

nlohmann::json ConfigManager::ParseINI(std::span<const uint8_t> data) {
    std::string str(reinterpret_cast<const char *>(data.data()), data.size());
    std::istringstream iss(str);
    return ParseINIImpl(iss);
}

} // namespace DX12Engine::Boot
