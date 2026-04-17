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
.\vcpkg install nlohmann-json:x64-windows
.\vcpkg install directxtk12:x64-windows
.\vcpkg install concurrentqueue:x64-windows
.\vcpkg install entt:x64-windows
.\vcpkg install spdlog:x64-windows
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
│   ├── settings.json
│   └── ...
│
├── Build/                  # [构建产物] (Git Ignored)
│   ├── Bin/                # 可执行文件 (.exe) 和 PDB
│   │   └── x64/
│   │       └── Debug/
│   └── Intermediate/       # 中间文件 (.obj, .tlog)
│       └── x64/
│           └── Debug/
│
├── Config/                 # [配置文件]
│   └── ...                 # 存放 JSON/YAML 等运行时配置文件
│
├── Content/                # [数据资产] (构建时自动复制到输出目录)
│   ├── Models/             # 3D 模型 (.obj, .gltf, .fbx)
│   └── Textures/           # 纹理资源 (.dds, .png, .jpg)
│
├── Docs/                   # [项目文档]
│   ├── Architecture.md
│   └── Notes.md
│
├── Engine/                 # [核心引擎代码]
│   ├── Include/            # 公开头文件 (接口定义)
│   │   ├── Core/
│   │   │   ├── Config/     # 配置模块头文件
│   │   │   └── Logger/     # 日志模块头文件
│   │   └── Renderer/       # 渲染模块头文件
│   └── Src/                # 源码实现
│       └── Core/
│           ├── Config/     # 配置模块实现
│           └── Logger/     # 日志模块实现
│
├── Runtime/                # [业务逻辑/入口]
│   ├── DX12_Base.cpp       # WinMain 入口与主循环
│   └── ...                 # 其他应用层代码
│
├── Shaders/                # [着色器代码]
│   ├── Source/             # HLSL 源码
│   └── Compiled/           # 编译后的 .cso (可选，也可运行时编译)
│
├── CMakeLists.txt          # CMake 构建脚本
└── README.md               # 项目说明
```
## 4. 技术栈与依赖说明

| 库名称 | 用途描述 |
| :--- | :--- |
| DirectX 12 | 底层图形 API，提供高性能渲染能力。 |
| DirectXTK12 | DX12 辅助库，简化纹理加载、模型加载、SpriteBatch 等常见操作。 |
| nlohmann/json | 现代 C++ JSON 库，用于解析 Config/ 下的配置文件及场景数据。 |
| spdlog | 高性能 C++ 日志库，用于引擎内部调试信息输出与错误追踪。 |
| concurrentqueue | 无锁并发队列，用于多线程环境下的命令缓冲提交或任务调度。 |
| EnTT | ECS (Entity-Component-System) 框架，用于游戏对象管理与架构解耦。 |

## 5. 开发规范

- **代码风格**: 遵循 Google C++ Style Guide 或项目内置的 `.clang-format`。
- **头文件包含**:
  - 引擎内部模块间引用请使用相对路径或基于 `Engine/Include` 的路径。
  - 例如: `#include "Core/Logger/Logger.h"`
- **资源管理**: 所有运行时需要的静态资源（模型、纹理）应放置在 `Content/` 目录下，CMake 会在构建后自动将其复制到可执行文件同级目录。
