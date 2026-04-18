# ConfigManager (配置管理器)

## 定位和职责

### 定位

- **上游依赖**：无（最早被初始化的核心单例，仅依赖基础文件系统读取能力）。
- **下游服务**：日志系统 (`Logger`)、渲染后端 (`Renderer`)、输入系统 (`InputSystem`)、音频系统 (`AudioSystem`)等几乎所有子系统都依赖它获取运行时参数。

### 职责

- **统一配置源管理**：加载、合并、验证来自不同来源的配置（如默认配置、用户自定义配置、命令行参数）。
- **结构化数据访问**：将扁平或嵌套的 JSON 数据转换为类型安全的 C++ 对象或直接提供查询接口。
- **配置持久化**：支持运行时修改配置并保存回磁盘，采用“节流自动保存 + 关闭强制保存”的混合策略。
- **热重载支持 (Hot-Reload)**：在开发模式下，监听配置文件变化并通知相关子系统重新应用配置。
- **默认值fallback机制**：当配置项缺失时，提供合理的默认值，防止引擎崩溃。

## 架构设计

### 配置层级与优先级
分层配置策略，优先级从低到高：
- **Hardcoded Defaults (代码硬编码默认值)**：写在 C++ 代码中的默认值。
- **Default Config File (默认配置文件)**：如 `config/default_settings.json`，随引擎发布，不可由用户直接修改。
- **User Config File (用户配置文件)**如 `config/user_settings.json` 或 `logging_config.json`，允许用户覆盖默认值。
- **Command Line Arguments (命令行参数)**：启动时传入的参数，优先级最高，用于快速调试（如 `-windowed`, `-log_level=debug`）。


### 功能模块

#### A.加载器 (Loader)
- **功能**：读取磁盘 JSON 文件，解析为 `nlohmann::json` 临时对象。
- **合并逻辑**：先加载默认配置，再加载用户配置进行覆盖（Key 级别合并）。
- **错误处理**：文件缺失或格式错误时记录警告，并回退到默认配置，避免崩溃。

#### B.存储与状态管理 (Store & State Management)
- **内部表示**：
    - **不维护**原始的 `nlohmann::json` 树以节省内存。
    - **维护**强类型的 C++ 配置结构体实例（如 `LogConfig`, `RenderConfig`）。这是运行时唯一的数据真理来源。
- **脏标记机制 (Dirty Tracking)：**
    - 维护全局或模块级的 bool m_isDirty 标志。
    - 任何 `Set` 操作都会将标记置为 true，并更新 `m_lastModifyTime`。
- **序列化映射：**
    - 定义 `FromJson()` 和 `ToJson()` 方法，实现 JSON 与 Struct 的双向转换。

#### C. 接口设计 (Interface Design)
```cpp
class ConfigManager {
public:
    // 1. 初始化与生命周期
    static void Initialize(const std::string& userConfigPath);
    static void Shutdown(); // 触发强制保存

    // 2. 读取配置 (Read)
    // 返回常量引用，确保外部只读访问，零拷贝开销
    const LogConfig& GetLogConfig() const;
    const RenderConfig& GetRenderConfig() const;

    // 3. 修改配置 (Write)
    // 更新内部结构体，标记 Dirty，并可选地触发 OnChange 回调
    void SetLogGlobalLevel(LogLevel level);
    void SetRenderFullscreen(bool enabled);

    // 4. 持久化 (Persist)
    // 将当前内存中的 Struct 序列化为 JSON 并写入磁盘
    void Save(); 

    // 5. 热重载 (Reload)
    // 重新读取磁盘 JSON，覆盖内存 Struct，并通知订阅者
    void Reload();

    // 6. 每帧更新 (用于节流保存检查)
    static void Update(float deltaTime);
};
```

#### D.验证器 (Validator)
- **功能**：在 `FromJson` 或 `Set` 调用时，检查数值合法性（如分辨率 > 0，日志级别枚举有效）。
- **作用**：早期发现配置错误，记录警告并重置为安全默认值。


#### E. 持久化策略 (Persistence Strategy)
采用 “**节流自动保存** + **关闭强制保存**” 的混合策略：

1. **脏标记与时间戳：**
    - 每次配置修改设置 `m_isDirty = true` 并记录 `m_lastModifyTime`。

2. **节流自动保存 (Throttled Auto-Save)：**
    - 在 `ConfigManager::Update()` 中检查：
    ```cpp
    if (m_isDirty && (CurrentTime - m_lastSaveTime > SAVE_THRESHOLD_SECONDS)) {
        Save();
    }
    ```
- 阈值建议：5~10 秒。
- **目的**：平衡 I/O 性能与数据安全，避免频繁磁盘写入，同时防止崩溃导致大量数据丢失。

3. **强制保存 (Force Save on Shutdown)：**
- 在引擎 `Shutdown` 流程中，无论是否满足节流条件，强制执行一次同步 `Save()`。
- 目的：确保程序正常退出时，最终状态一定被持久化。

4. **手动触发 (Manual Trigger)：**
- 暴露控制台命令 `config.save` 或 API，允许开发者立即持久化配置。

5. **冲突处理 (Conflict Handling)：**
执行 `Reload()` 前，若 `m_isDirty` 为真，应先执行 `Save()` 或发出警告，防止外部文件修改覆盖未保存的运行时更改。

#### F. 工程化进阶考量 (Advanced Engineering Considerations)

为了确保引擎在生产环境下的健壮性和高性能，需额外关注以下四点：

1. **线程安全模型 (Thread Safety Model)**
    - **问题**：渲染线程、物理线程和主逻辑线程可能同时调用 `GetRenderConfig()`。
    - **方案 A (读多写少 - 推荐)**：使用 `std::shared_mutex`。读取时加共享锁（Shared Lock），写入时加独占锁（Exclusive Lock）。
    - **方案 B (无锁双缓冲)**：维护两份配置副本 `ActiveConfig` 和 `PendingConfig`。写入时更新 `PendingConfig`，在帧同步点（Frame Sync）原子交换指针。读取端永远只读 `ActiveConfig`，完全无锁。

2. **原子写入与防损坏 (Atomic Write & Corruption Prevention)**
    - **风险**：直接覆盖 `user_settings.json` 时若进程崩溃，文件可能截断或损坏，导致下次启动解析失败。
    - **策略**：
        1. 序列化内容写入临时文件 `user_settings.json.tmp`。
        2. 确保临时文件写入成功并 flush 到磁盘。
        3. 使用操作系统原子的重命名操作（如 Windows `MoveFileEx` with `MOVEFILE_REPLACE_EXISTING`）将 tmp 文件替换为目标文件。

3. **版本控制与迁移 (Versioning & Migration)**
    - **机制**：在 JSON 根节点增加 `"version": 1` 字段。
    - **迁移器 (Migrator)**：
        - 加载时检查版本。
        - 若 `LoadedVersion < CurrentVersion`，调用迁移函数链（例如：V1->V2 重命名字段，V2->V3 删除废弃字段）。
        - 保存时始终写入当前最新版本号。

4. **热重载的订阅者模式 (Observer Pattern for Hot-Reload)**
    - **解耦**：`ConfigManager` 不应直接调用 `Renderer::ApplySettings()`。
    - **实现**：
      ```cpp
      // 定义回调类型
      using ConfigChangeCallback = std::function<void(const std::string& section)>;

      // 注册监听
      void Subscribe(const std::string& section, ConfigChangeCallback callback);

      // 触发
      void OnConfigChanged(const std::string& section) {
          for (auto& cb : m_subscribers[section]) {
              cb();
          }
      }
      ```

