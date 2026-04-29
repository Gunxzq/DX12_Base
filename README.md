# DX12_Base

基于 DirectX 12 的轻量级渲染引擎框架，采用现代 C++ (C++17) 开发，使用 CMake 进行跨平台构建管理（目前主要支持 Windows）。

## 1. 环境要求

1. **操作系统**: Windows 10/11 (64-bit)
2. **编译器**: MSVC (Visual Studio 2022 推荐)
3. **构建工具**: CMake (>= 3.14)
4. **包管理器**: vcpkg
5. **IDE**: Visual Studio Code (推荐安装 C/C++, CMake Tools 插件)

## 2. 快速开始

### 2.1 克隆项目
```bash
git clone <your-repo-url>
cd DX12_Base
```
### 2.2 安装依赖
本项目使用 vcpkg 管理第三方库。请确保已安装 vcpkg 并配置好环境变量，或者在项目中指定 vcpkg 路径。

在项目根目录或 vcpkg 安装目录下，执行以下命令安装所需库：

```powershell
# 假设 vcpkg 安装在 D:\Dev\vcpkg
cd D:\Dev\vcpkg

# 安装核心依赖 (x64-windows  triplet)
.\vcpkg install nlohmann-json:x64-windows ^
concurrentqueue:x64-windows ^
entt:x64-windows ^
spdlog:x64-windows ^
assimp:x64-windows ^
mimalloc:x64-windows ^
joltphysics:x64-windows ^
imgui[dx12-binding]:x64-windows ^
gtest:x64-windows
```


### 2.3 配置 VS Code
项目已包含 .vscode/settings.json，自动配置了 CMake 工具链和 vcpkg 路径。 如果你的 vcpkg 路径不是 D:/Dev/vcpkg，请修改 .vscode/settings.json 中的 cmake.configureArgs：

```json
"cmake.configureArgs": [
    "-DCMAKE_TOOLCHAIN_FILE=<你的vcpkg路径>/scripts/buildsystems/vcpkg.cmake"
]
```
### 2.4 构建与运行
打开 VS Code，按下 Ctrl+Shift+P。
输入 CMake: Configure 并选择 Visual Studio 17 2022 生成器。
等待配置完成后，按下 F7 或点击底部状态栏的 Build 按钮进行编译。
编译成功后，按 F5 调试运行，或在 Build/Bin/x64/Debug 目录下找到可执行文件。

## 3. 项目目录结构
```
DX12_Base/
│
├── .vscode/                # VS Code 配置 (CMake, C/C++ 设置)
│   ├── c_cpp_properties.json
│   └── settings.json
│
├── Config/                 # [配置文件] JSON 格式
│   ├── logging_config.json
│   └── window.json
│
├── Content/                # [数据资产] (构建时自动复制到输出目录)
│   ├── Models/             # 3D 模型 (.obj, .gltf, .fbx)
│   ├── Textures/           # 纹理资源 (.dds, .png, .jpg)
│   ├── DX12_Base.ico
│   ├── DX12_Base.rc
│   └── Resource.h
│
├── Engine/                 # [核心引擎代码]
│   ├── Include/            # 公开头文件
│   │   ├── Common/         # 公共基础 (d3dUtil, MathHelper, d3dx12)
│   │   ├── Core/           # 核心模块
│   │   │   ├── Bootstrap/  # 启动引导
│   │   │   ├── Config/     # 配置管理
│   │   │   └── Context/    # 相机、游戏上下文、计时器
│   │   ├── Renderer/       # 渲染模块
│   │   └── System/         # 系统模块
│   │       ├── Logger/     # 日志系统
│   │       └── Window/     # 窗口管理
│   └── Src/                # 源码实现
│
├── Runtime/                # [业务逻辑/入口]
│   ├── Application/        # 应用程序入口
│   └── Game/               # 游戏逻辑
│
├── Shaders/                # [着色器代码]
│
├── Tests/                  # [单元测试] Google Test
│
├── CMakeLists.txt          # CMake 构建脚本
├── CMakeSettings.json      # VS CMake 配置
└── README.md               # 项目说明
```
## 4. 技术栈与依赖说明

| 库名称 | 用途描述 |
| :--- | :--- |
| DirectX 12 | 底层图形 API，提供高性能渲染能力。 |
| nlohmann/json | 现代 C++ JSON 库，用于解析 Config/ 下的配置文件及场景数据。 |
| spdlog | 高性能 C++ 日志库，用于引擎内部调试信息输出与错误追踪。 |
| concurrentqueue | 无锁并发队列，用于多线程环境下的命令缓冲提交或任务调度。 |
| EnTT | ECS (Entity-Component-System) 框架，用于游戏对象管理与架构解耦。 |
| assimp | 3D 模型加载库，支持多种模型格式 (.obj, .gltf, .fbx 等)。 |
| mimalloc | 高性能内存分配器，支持全局替换使能 (MI_OVERRIDE=ON)。 |
| Jolt Physics | 物理引擎，用于刚体碰撞、关节等物理模拟。 |
| Dear ImGui | 即时模式 GUI 库，用于调试界面和编辑器工具。 |
| Google Test | 单元测试框架 (可选，需开启 BUILD_TESTS)。 |

## 6. 事件系统优先级

本引擎采用基于优先级的消息队列系统，支持 **5 级优先级调度**：

| 优先级 | 名称 | 典型用途 | 策略 |
|:------|:-----|:---------|:-----|
| P0 | Critical | 系统告警（内存溢出、强制退出） | None |
| P1 | High | 物理系统（碰撞、触发器） | None |
| P2 | Normal | 游戏逻辑（扣血、技能） | None |
| P3 | Low | 渲染系统（特效、UI） | Sample |
| P4 | Background | 异步任务（资源加载结果） | Throttle |

### 策略说明

- **None**: 队列满时直接丢弃
- **Sample**: 队列满时丢弃最旧的消息，保留新的
- **Throttle**: 队列满时静默丢弃新消息

### 优先级配置

相关配置位于 `Engine/Include/System/Event/BucketManager.h`：
```cpp
static constexpr uint32_t MAX_PRIORITY_LEVELS = 5; // P0-P4
```

## 7. 开发规范

- **代码风格**: 遵循 Google C++ Style Guide 或项目内置的 `.clang-format`。
- **头文件包含**: 
  - 引擎内部模块间引用请使用相对路径或基于 `Engine/Include` 的路径。
  - 例如: `#include "Core/Logger/Logger.h"`
- **资源管理**: 所有运行时需要的静态资源（模型、纹理）应放置在 `Content/` 目录下，CMake 会在构建后自动将其复制到可执行文件同级目录。
- **内存管理**: 项目默认启用 mimalloc 全局替换 (MI_OVERRIDE=ON)，std::vector 等容器会自动使用 mimalloc 分配器，无需额外配置。
