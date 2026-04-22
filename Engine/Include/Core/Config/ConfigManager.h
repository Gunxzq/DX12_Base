#pragma once

#include "Common/Common.h"
#include "LoggerConfig.h"
#include "WindowConfig.h"

#include <nlohmann/json.hpp>

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
     * @throw std::runtime_error 如果配置目录无效或关键配置文件损坏
     */
    void Initialize(const std::filesystem::path &configDir);

    /**
     * @brief 关闭并强制保存配置
     */
    void Shutdown();

    /**
     * @brief 构造函数
     */
    ConfigManager();

    /**
     * @brief 析构函数
     */
    ~ConfigManager();

    // 禁止拷贝和移动
    ConfigManager(const ConfigManager &) = delete;
    ConfigManager &operator=(const ConfigManager &) = delete;
    ConfigManager(ConfigManager &&) = delete;
    ConfigManager &operator=(ConfigManager &&) = delete;

    // --- 2. 读取配置 (Read) ---

    /**
     * @brief 获取日志配置
     * @attention Thread-safe: Acquires shared lock.
     * @return const LogConfig &
     */
    const LogConfig &GetLogConfig() const;

    /**
     * @brief 获取窗口配置
     * @attention Thread-safe: Acquires shared lock.
     * @return const WindowConfig &
     */
    const WindowConfig &GetWindowConfig() const;

    // --- 3. 修改配置 (Write) ---

    /**
     * @brief 设置全局日志级别
     * @attention Thread-safe: Acquires unique lock.
     */
    void SetLogGlobalLevel(LogLevel level);

    /**
     * @brief 设置日志目录
     * @attention Thread-safe: Acquires unique lock.
     */
    void SetLogDirectory(const std::string &dir);

    // --- 4. 持久化 (Persist) ---

    /**
     * @brief 手动触发保存
     * @attention Thread-safe: Acquires unique lock.
     */
    void Save();

    // --- 5. 热重载 (Reload) ---

    /**
     * @brief 重新从磁盘加载配置
     * @attention Thread-safe: Acquires unique lock.
     */
    void Reload();

    // --- 6. 每帧更新 (Update) ---

    /**
     * @brief 在主循环中调用，处理节流自动保存
     * @attention Thread-safe: Acquires unique lock internally if dirty.
     * @param deltaTime 帧间隔时间(秒)
     */
    void Update(float deltaTime);

    // --- 7. 订阅者模式 (Observer) ---

    /**
     * @brief 订阅配置变更事件
     * @attention  Thread-safe: Acquires unique lock.
     * @param section 配置节名称 (如 "Log")
     * @param callback 变更回调
     */
    void Subscribe(const std::string &section, ConfigChangeCallback callback);

private:
    // --- 内部辅助方法 ---

    /**
     * @brief 加载日志配置
     * @attention REQUIRES: Caller must hold m_mutex (unique).
     */
    void LoadLoggingConfig_Locked(const std::filesystem::path &path);

    /**
     * @brief 加载窗口配置
     * @attention REQUIRES: Caller must hold m_mutex (unique).
     */
    void LoadWindowConfig_Locked(const std::filesystem::path &path);

    /**
     * @brief 加载并合并 JSON 文件
     * @attention  REQUIRES: Caller must hold m_mutex (unique or shared depending on usage, usually unique forload).
     * @note Internal logic only, no locking performed here.
     */
    void LoadAndMergeConfigs_Locked(const std::filesystem::path &userPath, const std::filesystem::path &defaultPath);

    /**
     * @brief 将内存中的 Struct 同步到 JSON 对象 (准备保存)
     * @attention REQUIRES: Caller must hold m_mutex (unique).
     * @note Internal logic only, no locking performed here.
     */
    void SyncStructsToJson_Locked();

    /**
     * @brief 将 JSON 对象解析到内存 Struct
     * @attention REQUIRES: Caller must hold m_mutex (shared).
     * @note Internal logic only, no locking performed here.
     */
    void ParseJsonToStructs_Locked(const nlohmann::json &j);

    /**
     * @brief 保存配置
     * @attention REQUIRES: Caller must hold m_mutex (unique).
     * @note Performs the actual file write logic. Lock is held to ensure consistency of m_isDirty/m_configData.
     */
    bool SaveInternal_Locked();

    /**
     * @brief 通知订阅者
     * @attention REQUIRES: Caller must NOT hold m_mutex (to avoid deadlock in callbacks).
     * @note Notifies subscribers outside of the lock scope.
     */
    void NotifySubscribers_Unlocked(const std::string &section);

    /**
     * @brief 原子写入文件 (防止损坏)
     */
    bool AtomicWriteFile(const std::filesystem::path &targetPath, const std::string &content);

    // --- 成员变量 ---

    // 互斥锁: 保护配置数据的读写 (std::shared_mutex 允许并发读)
    mutable std::shared_mutex m_mutex;

    // 强类型配置实例
    LogConfig m_logConfig;
    WindowConfig m_windowConfig;

    // 原始 JSON 数据
    nlohmann::json m_configData;

    // 状态标记
    bool m_isInitialized = false;
    bool m_isDirty = false;

    // 时间追踪
    std::chrono::steady_clock::time_point m_lastModifyTime;
    std::chrono::steady_clock::time_point m_lastSaveTime;

    // 配置路径
    std::filesystem::path m_configDir;

    // 节流阈值
    static constexpr float SAVE_THRESHOLD_SECONDS = 5.0f;

    // 订阅者列表: Key = Section Name
    std::unordered_map<std::string, std::vector<ConfigChangeCallback>> m_subscribers;
};

} // namespace Core
} // namespace DX12Engine