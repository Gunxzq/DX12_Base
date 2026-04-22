#include "Core/Config/ConfigManager.h"

namespace {

/**
 * @brief UTF-8 字符串转换为 UTF-16 宽字符串
 */
std::wstring Utf8ToWstring(const std::string &utf8Str) {
    if (utf8Str.empty())
        return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, nullptr, 0);
    if (size_needed == 0)
        return std::wstring();
    std::wstring result(size_needed - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, &result[0], size_needed);
    return result;
}

} // namespace

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
        try {
            Shutdown();
        } catch (const std::exception &e) {
            // 析构函数中不能抛出异常，使用宏记录并中断（如果是Debug）
            ENGINE_ASSERT_FMT("ConfigManager Destructor Exception: %s", e.what());
        }
    }
}

/**
 * @brief 初始化配置管理器
 * @param configDir 配置目录路径
 * @date 2026-04-18
 */
void ConfigManager::Initialize(const std::filesystem::path &configDir) {

    std::unique_lock<std::shared_mutex> lock(m_mutex);

    if (m_isInitialized) {
        return;
    }

    // 1. 验证并创建目录
    if (!std::filesystem::exists(configDir)) {
        try {
            std::filesystem::create_directories(configDir);
        } catch (const std::filesystem::filesystem_error &e) {
            // 致命错误：记录、中断、抛出
            ENGINE_ASSERT_FMT("Failed to create config directory: %s", e.what());
            throw std::runtime_error(std::string("Failed to create config directory: ") + e.what());
        }
    }

    m_configDir = configDir;

    // 2. 加载配置
    try {
        LoadLoggingConfig_Locked(configDir / "logging_config.json");
        LoadWindowConfig_Locked(configDir / "window.json");
    } catch (const std::exception &e) {
        // 致命错误：记录、中断、抛出
        ENGINE_ASSERT_FMT("Initialization failed during load: %s", e.what());
        throw std::runtime_error(std::string("Initialization failed: ") + e.what());
    }
    m_isInitialized = true;
    m_isDirty = false;
    m_lastSaveTime = std::chrono::steady_clock::now();

    //
}

/**
 * @brief 关闭配置管理器
 * @date 2026-04-18
 */
void ConfigManager::Shutdown() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    if (!m_isInitialized) {
        return;
    }

    if (m_isDirty) {
        if (!SaveInternal_Locked()) {
            // 警告级别：记录但不中断程序退出流程
            ENGINE_ASSERT_MSG("Warning: Failed to save config on shutdown.");
        }
    }

    m_isInitialized = false;
    m_isDirty = false;
    m_configData.clear();
    m_subscribers.clear();
}

/**
 * @brief 获取日志配置
 * @return const LogConfig&
 * @date 2026-04-18
 */
const LogConfig &ConfigManager::GetLogConfig() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    // Direct access is safe as we hold the lock
    return m_logConfig;
}

/**
 * @brief 获取窗口配置
 * @return const WindowConfig&
 */
const WindowConfig &ConfigManager::GetWindowConfig() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_windowConfig;
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
            SyncStructsToJson_Locked();
        }
    }
    // Notify outside lock to prevent deadlocks if subscriber tries to read config
    NotifySubscribers_Unlocked("Log");
}

void ConfigManager::SetLogDirectory(const std::string &dir) {
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        if (m_logConfig.Sinks.File.Path != dir) {
            m_logConfig.Sinks.File.Path = dir;
            m_isDirty = true;
            m_lastModifyTime = std::chrono::steady_clock::now();
            SyncStructsToJson_Locked();
        }
    }
    NotifySubscribers_Unlocked("Log");
}

void ConfigManager::Save() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    SaveInternal_Locked();
}

void ConfigManager::Reload() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    // REQUIRES: Lock is held.
    // 如果有修改，请先保存以防数据丢失。
    if (m_isDirty) {
        SaveInternal_Locked();
    }

    try {
        LoadLoggingConfig_Locked(m_configDir / "logging_config.json");
        LoadWindowConfig_Locked(m_configDir / "window.json");
    } catch (const std::exception &e) {
        ENGINE_ASSERT_FMT("Reload failed: %s", e.what());
        // Reload 失败通常不抛出异常，而是保持旧配置，但这里我们记录错误
        lock.unlock();
        return;
    }

    // 准备在持有锁的情况下通知的部分列表，但在锁外通知
    std::vector<std::string> sectionsToNotify;
    for (auto &pair : m_subscribers) {
        sectionsToNotify.push_back(pair.first);
    }

    // Unlock before notifying
    lock.unlock();

    for (const auto &section : sectionsToNotify) {
        NotifySubscribers_Unlocked(section);
    }
}

void ConfigManager::Update(float deltaTime) {
    if (!m_isInitialized) {
        return;
    }

    bool shouldSave = false;
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        if (!m_isDirty) {
            return;
        }

        std::string content;
        std::filesystem::path path;

        {

            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<float> elapsed = now - m_lastSaveTime;

            if (elapsed.count() > SAVE_THRESHOLD_SECONDS) {
                shouldSave = true;
                // 在锁外执行实际的 IO，
                // 但需要确保状态是一致的。
                // 为了简化这种模式，可以在这里调用 SaveInternal_Locked
                // 或者解锁后调用公共的 Save。
                // 为了最小化 IO 的锁定时间：
                m_isDirty = false; // 乐观地标记为干净，如果保存失败，可能需要恢复,
                                   // 但对于配置，下一帧重试是可以接受的。
                m_lastSaveTime = now;

                SyncStructsToJson_Locked();

                // 需要在锁外写入内容
                // 然而，AtomicWriteFile 需要路径和内容。
                // 让我们坚持调用 SaveInternal_Locked，但接受 IO 在锁内以确保正确性
                // 或者重构 SaveInternal，仅准备数据并返回内容。

                // “最小化持锁时间”的更好方法：
                // 1. 检查是否脏以及时间（完成）
                // 2. 标记为干净并更新时间（完成）
                // 3. 复制 IO 所需的数据（内容）
                // 4. 解锁
                // 5. 写入文件

                content = m_configData.dump(4);
                path = m_configDir / "logging_config.json";
            }

            if (shouldSave) {
                // 在锁外执行 IO
                // 注意：这需要稍微重构 SaveInternal，使其返回内容或单独处理 IO。
                // 根据当前结构，让我们将 SaveInternal_Locked 调整为纯数据同步，
                // 并为 IO 提供一个单独的辅助函数，或者接受在 IO 期间 Save() 持有锁。

                // 为了严格遵循“在锁外执行 IO”，我们需要在这里提取内容。
                if (!AtomicWriteFile(path, content)) {
                    // 保存失败，重新标记为 dirty
                    std::unique_lock<std::shared_mutex> lock(m_mutex);
                    m_isDirty = true;
                    ENGINE_ASSERT_MSG("Auto-save failed.");
                }
            }
        }
    }
}

void ConfigManager::Subscribe(const std::string &section, ConfigChangeCallback callback) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_subscribers[section].push_back(std::move(callback));
}

// --- Private Methods ---

void ConfigManager::LoadLoggingConfig_Locked(const std::filesystem::path &path) {
    // REQUIRES: m_mutex is held by caller
    if (std::filesystem::exists(path)) {
        try {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs.is_open()) {
                throw std::runtime_error("Cannot open file: " + path.string());
            }
            nlohmann::json j = nlohmann::json::parse(ifs);
            if (j.contains("logging")) {
                m_logConfig = j["logging"].get<LogConfig>();
            } else {
                // 非致命：使用默认值
                m_logConfig = LogConfig();
            }
        } catch (const nlohmann::json::parse_error &e) {
            // 致命：JSON 语法错误
            ENGINE_ASSERT_FMT("JSON syntax error in %s: %s", path.string().c_str(), e.what());
            throw std::runtime_error(std::string("JSON parse error: ") + e.what());
        } catch (const std::exception &e) {
            // 非致命：其他解析错误，使用默认值
            ENGINE_ASSERT_FMT("Failed to parse logging config, using defaults: %s", e.what());
            m_logConfig = LogConfig();
        }
    } else {
        m_logConfig = LogConfig();
    }
}

void ConfigManager::LoadWindowConfig_Locked(const std::filesystem::path &path) {
    // REQUIRES: m_mutex is held by caller
    if (std::filesystem::exists(path)) {
        try {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs.is_open()) {
                throw std::runtime_error("Cannot open file: " + path.string());
            }
            nlohmann::json j = nlohmann::json::parse(ifs);
            if (j.contains("window")) {
                auto &winJson = j["window"];
                m_windowConfig.title = L"DX12 Engine";
                if (winJson.contains("title") && winJson["title"].is_string()) {
                    m_windowConfig.title = Utf8ToWstring(winJson["title"].get<std::string>());
                }
                if (winJson.contains("resolution")) {
                    auto &res = winJson["resolution"];
                    if (res.contains("width") && res["width"].is_number_unsigned())
                        m_windowConfig.width = res["width"].get<uint32_t>();
                    if (res.contains("height") && res["height"].is_number_unsigned())
                        m_windowConfig.height = res["height"].get<uint32_t>();
                }
                if (winJson.contains("mode") && winJson["mode"].is_string()) {
                    m_windowConfig.mode = winJson["mode"].get<std::string>();
                }
                if (winJson.contains("behavior")) {
                    auto &behavior = winJson["behavior"];
                    if (behavior.contains("resizable") && behavior["resizable"].is_boolean())
                        m_windowConfig.resizable = behavior["resizable"].get<bool>();
                    if (behavior.contains("maximizable") && behavior["maximizable"].is_boolean())
                        m_windowConfig.maximizable = behavior["maximizable"].get<bool>();
                }
            } else {
                m_windowConfig = WindowConfig();
            }
        } catch (const nlohmann::json::parse_error &e) {
            ENGINE_ASSERT_FMT("JSON syntax error in %s: %s", path.string().c_str(), e.what());
            throw std::runtime_error(std::string("JSON parse error: ") + e.what());
        } catch (const std::exception &e) {
            ENGINE_ASSERT_FMT("Failed to parse window config, using defaults: %s", e.what());
            m_windowConfig = WindowConfig();
        }
    } else {
        m_windowConfig = WindowConfig();
    }
}

void ConfigManager::LoadAndMergeConfigs_Locked(const std::filesystem::path &userPath,
                                               const std::filesystem::path &defaultPath) {
    //    保留
}

void ConfigManager::SyncStructsToJson_Locked() {
    // REQUIRES: m_mutex is held by caller
    nlohmann::json logJson = m_logConfig;
    m_configData["logging"] = logJson;

    // 手动构建 Window JSON (假设没有自动 to_json)
    nlohmann::json winJson;
    // 注意：wstring 转 string 可能需要辅助函数，这里简化处理
    std::string titleStr(m_windowConfig.title.begin(), m_windowConfig.title.end());
    winJson["title"] = titleStr;
    winJson["resolution"]["width"] = m_windowConfig.width;
    winJson["resolution"]["height"] = m_windowConfig.height;
    winJson["mode"] = m_windowConfig.mode;
    winJson["behavior"]["resizable"] = m_windowConfig.resizable;
    winJson["behavior"]["maximizable"] = m_windowConfig.maximizable;

    m_configData["window"] = winJson;
}
void ConfigManager::ParseJsonToStructs_Locked(const nlohmann::json &j) {
    // REQUIRES: m_mutex is held by caller
    if (j.contains("logging")) {
        try {
            m_logConfig = j["logging"].get<LogConfig>();
        } catch (const std::exception &) {
            m_logConfig = LogConfig();
        }
    } else {
        m_logConfig = LogConfig();
    }
}

bool ConfigManager::SaveInternal_Locked() {
    // REQUIRES: m_mutex is held by caller (unique)
    if (!m_isDirty) {
        return true;
    }

    SyncStructsToJson_Locked();

    std::string content = m_configData.dump(4);

    auto savePath = m_configDir / "logging_config.json";

    bool success = AtomicWriteFile(savePath, content);

    if (success) {
        m_isDirty = false;
        m_lastSaveTime = std::chrono::steady_clock::now();
    }

    return success;
}

void ConfigManager::NotifySubscribers_Unlocked(const std::string &section) {
    // REQUIRES: m_mutex is NOT held
    auto it = m_subscribers.find(section);
    if (it != m_subscribers.end()) {
        for (auto &cb : it->second) {
            if (cb) {
                try {
                    cb(section);
                } catch (const std::exception &e) {
                    ENGINE_ASSERT_FMT("Subscriber callback exception: %s", e.what());
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

} // namespace Core
} // namespace DX12Engine