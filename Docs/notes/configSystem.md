# 配置系统

## 1. 配置层级与生命周期

### 按修改时间划分

| 阶段 | 修改方式 | 典型配置 |
|------|---------|---------|
| **设计期**（Design-time） | 编辑 JSON 源文件，重新导出 | 管线 RingBuffer 定义、渲染通道布局、材质预设 |
| **开发期**（Development） | 热重载、运行时修改、保存回磁盘 | 日志级别、窗口大小、调试开关 |
| **发布期**（Runtime） | 仅通过存档/菜单设置覆盖有限选项 | 分辨率、画质档、音量 |

### 按归属划分

| 归属 | 内容 | 配置来源 |
|------|------|---------|
| **引擎默认值** | 引擎代码自带的最小可行配置 | CMake 安装时复制到产物目录，或内嵌二进制 |
| **项目级配置** | 具体游戏项目的管线定义、资源路径 | `Config/` 目录，代码仓库管理 |
| **用户配置** | 玩家/开发者的个人偏好 | `user_settings.json`，存档目录，运行时写入 |

---

## 2. 格式划分：JSON vs INI

| 格式 | 适用场景 | 示例 |
|------|---------|------|
| **JSON** | 嵌套结构、对象数组、复杂数据类型 | 渲染管线配置、RingBuffer 定义、品质预设 |
| **INI** | 扁平键值对、简单开关、路径配置 | 窗口尺寸、启动路径、引擎快速开关 |

### 实际判断标准

需要数组或嵌套对象 → JSON
只有平铺的 `key=value` → INI
模棱两可时 → JSON（INI 的任何内容 JSON 都能表达，反之不然）

### 参考：商业引擎实践

| 引擎 | 做法 |
|------|------|
| **Unreal** | INI（`Engine.ini`、`Game.ini`）+ JSON（`.uproject` 资产文件） |
| **Unity** | 文本格式 + JSON（`ProjectSettings.json`、`Package.json`） |
| **Godot** | 自定义 INI 风格（`project.godot`）+ JSON（导出配置） |

---

## 3. 开发期与发布期：配置的两种形态

### 开发期配置

原始 JSON/INI 文件，位于 `Config/` 目录下：

```
项目根/
├── Engine/                       ← 引擎源码（CMake 源码目录）
├── Config/                       ← 配置根目录
│   ├── EngineDefaults/           ← 引擎自带的默认配置
│   │   ├── renderer.json
│   │   └── logging.json
│   ├── frame_resource.json       ← 项目级管线配置
│   └── gameplay.ini
```

特点：
- 人类可读，直接文本编辑
- 在版本控制中追踪
- 修改后需要重启或触发热重载

### 发布期配置

发布时配置可能以多种形态存在，取决于目标平台和发布策略：

| 发布形态 | 配置处理方式 | 可修改性 |
|---------|-------------|---------|
| **PC 独立游戏** | 直接附带原始 JSON/INI | 用户可直接编辑 |
| **PC 商业游戏** | JSON 打包进 AssetArchive（pak），运行时解码到内存 | 不可直接修改 |
| **主机游戏** | 编译期序列化为二进制结构体，`memcpy` 直接加载 | 不可修改 |
| **手游** | JSON 内嵌在 AssetBundle 或 SQLite 中 | 不可直接修改 |

### 二进制序列化路径

```
开发期：
  frame_resource.json → [导出工具/CI] → frame_resource.bin
                                        (内存布局与 C++ Struct 一致)

运行时：
  frame_resource.bin → memcpy → FrameResourceConfig struct (零解析)
```

**何时适合烘焙为二进制：**
- 管线定义类配置（RingBuffer 定义、渲染通道布局）
- 运行时修改没有意义，因为管道在设计期已固定
- 对加载性能敏感的中小型数据

**何时保留 JSON：**
- 运行时需要修改的配置（日志、窗口）
- 热重载场景
- 用户偏好设置

---

## 4. 二进制默认值 + 用户覆盖的两层模型

```
启动时加载路径:

[Baked defaults]             [User overlay files]
 (X:\game\data\configs\)      (%appdata%\GameName\)
      |                              |
      ↓ memcpy                       ↓
  [Defaults Struct]        [JSON/INI → Struct]
          \                      /
           ↓ 按字段覆盖           ↓
        [Final Runtime Struct]
```

- **Baked defaults**：编译期序列化或内嵌资源，只读，零解析
- **User overlay**：运行时从存档目录加载，仅包含差异字段，可热重载

修改路径：

```
我要改 frame_resource.json：

开发期 → 编辑 Config/frame_resource.json → 重新导出二进制 → 重启

我要改窗口分辨率：

运行时 → 菜单修改 → 写入 user_settings.json → 立即生效（或下次启动）
```

这种分离保证了：
-  发布产物安全（核心配置不可篡改）
-  用户偏好自由（覆盖层独立）
-  启动性能（默认值零解析）

---

## 5. ConfigManager 工具化架构

### 定位变化

```
之前: ConfigManager 是 "管理器" — 知道有哪些配置类型，各自硬编码加载器
现在: ConfigManager 是 "工具"   — 调用方指定路径+类型，它只负责读取、解析、通知
```

### API 设计

```cpp
class ConfigManager {
public:
    // ============================================================
    // 工具 API — 一次性加载，无状态
    //   - 调用方自己持有返回的数据
    //   - 不注册、不追踪、无热重载
    // ============================================================

    template<typename T>
    static T LoadJSON(const std::filesystem::path &path);

    template<typename T>
    static T LoadJSON(std::span<const uint8_t> data);  // 从内存解析

    template<typename T>
    static T LoadINI(const std::filesystem::path &path);

    template<typename T>
    static T LoadINI(std::span<const uint8_t> data);

    // ============================================================
    // 托管 API — 注册引擎 CORE 配置（带热重载/自动保存）
    //   - ConfigManager 持有 JSON 数据副本
    //   - 支持注册、通知、节流保存
    // ============================================================

    enum class ConfigFormat { JSON, INI };

    struct ConfigEntry {
        std::filesystem::path path;
        ConfigFormat format;
        bool enableHotReload = false;
    };

    void Register(const std::string &key,
                  const ConfigEntry &entry,
                  std::function<void(const nlohmann::json &)> applier);

    // 生命周期
    void Update(float deltaTime);           // 节流自动保存检测
    void Reload(const std::string &key);    // 重载单个配置节
    void Save(const std::string &key);      // 保存单个配置节
    void SaveAll();                         // 保存所有脏配置

    // 订阅变更
    void Subscribe(const std::string &key, std::function<void()> callback);

    // 访问原始 JSON 数据（供调用方读取配置字段）
    const nlohmann::json &GetJSON(const std::string &key) const;

private:
    // 已有状态：订阅者列表、脏标记、节流计时
    // 新增：std::unordered_map<std::string, ConfigEntry> m_entries
};
```

### 使用方式对比

**引擎 CORE 配置（托管 + 热重载）：**

```cpp
// Bootstrap.cpp — 一次性注册所有引擎配置
auto &cfg = ConfigManager::GetInstance();

cfg.Register("renderer", {"EngineDefaults/renderer.json", ConfigFormat::JSON, true},
    [this](const nlohmann::json &j) {
        m_rendererConfig = j.get<RendererConfig>();
        m_rendererConfig.PostLoad();
    });

cfg.Register("frame_resource", {"Config/frame_resource.json", ConfigFormat::JSON, false},
    [this](const nlohmann::json &j) {
        m_frameResConfig = j.get<FrameResourceConfig>();
        // 应用配置到 FrameResourceManager
    });
```

**项目配置（一次性加载，调用方自行管理）：**

```cpp
// 无状态工具调用
auto gameplay = ConfigManager::LoadINI<GameplayConfig>("Config/gameplay.ini");
auto ringBufs = ConfigManager::LoadJSON<FrameResourceConfig>("Config/frame_resource.json");
```

**二进制产物加载：**

```cpp
std::vector<uint8_t> cookedData = AssetArchive::Read("configs/renderer.bin");
auto cfg = ConfigManager::LoadJSON<RendererConfig>(cookedData);
```

### 不变的状态

ConfigManager 仍持有的单例状态：
- 已注册配置节的元数据（路径、格式、是否热重载）
- 订阅者列表 `std::unordered_map<std::string, std::vector<Callback>>`
- 脏标记 `m_isDirty` 和节流计时
- 原始 JSON 数据副本（用于热重载时的差异检测）

不持有的状态：
- 具体的 C++ 结构体实例（由调用方自己管理）
- 配置文件与具体类型的映射关系

---

## 6. FrameResourceManager 配置化设计

参见 `Docs/architecture/FrameResourceManager.md`

### RingBuffer 配置格式

```json
{
    "frameResource": {
        "ringBuffers": [
            { "name": "ObjectCB",   "initialSize": 16777216, "alignment": 256 },
            { "name": "Skinning",   "initialSize": 16777216, "alignment": 16 },
            { "name": "Instance",   "initialSize": 16777216, "alignment": 16 },
            { "name": "WaterCB",    "initialSize": 16777216, "alignment": 256 }
        ]
    }
}
```

### 对应 Struct

```cpp
struct RingBufferConfig {
    std::string name;
    uint32_t initialSize = 16 * 1024 * 1024;
    uint32_t alignment = 256;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(RingBufferConfig, name, initialSize, alignment)
};

struct FrameResourceConfig {
    std::vector<RingBufferConfig> ringBuffers;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(FrameResourceConfig, ringBuffers)
};
```
