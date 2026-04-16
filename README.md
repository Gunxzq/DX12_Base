# DX12_Base

基于 DirectX 12 的轻量级渲染引擎框架，使用 CMake 进行跨平台构建管理（目前主要支持 Windows）。

## 1. 环境要求与依赖安装

本项目使用 [vcpkg](https://github.com/microsoft/vcpkg) 管理 C++ 第三方库。请确保已安装 vcpkg 并配置好环境变量。

### 1.1 安装依赖
在项目根目录或 vcpkg 安装目录下，执行以下命令安装所需库：

```
# 进入 vcpkg 目录 (假设路径为 D:\Dev\vcpkg)
cd D:\Dev\vcpkg

# 安装核心依赖
.\vcpkg install nlohmann-json:x64-windows
.\vcpkg install directxtk12:x64-windows
.\vcpkg install concurrentqueue:x64-windows
.\vcpkg install entt:x64-windows
```

### 1.2 依赖说明

| 库名称 | 用途描述 |
| :--- | :--- |
| nlohmann/json | 现代 C++ JSON 库，用于配置文件解析、场景数据序列化等。 |
| DirectXTK12 | DirectX 12 工具包，提供常用的数学库、纹理加载、模型加载及辅助渲染功能，简化 DX12 底层 API 调用。 |
| concurrentqueue | 高性能无锁并发队列，用于实现渲染线程与逻辑线程之间的数据通信（如命令缓冲提交）。 |
| EnTT | 极速且轻量的 Entity-Component-System (ECS) 库，用于游戏对象管理和架构解耦。 |

## 2. 项目目录结构
text
DX12_Base/
│
├── Build/                # [构建产物] (Git Ignored)
│   ├── Bin/              # 编译生成的可执行文件 (.exe) 和调试符号 (.pdb)
│   └── Intermediate/     # 编译中间文件 (.obj, .tlog)，可安全清理
│
├── Content/              # [数据资产]
│   ├── Models/           # 3D模型文件 (.obj, .gltf, .fbx)
│   ├── Textures/         # 纹理资源 (.dds, .png, .jpg)
│   └── *.rc              # Windows资源脚本 (图标、菜单、版本信息)
│
├── Docs/                 # [项目文档]
│   ├── Architecture.md   # 架构设计文档
│   └── Notes.md          # 开发笔记与踩坑记录
│
├── Engine/               # [核心引擎代码]
│   ├── Renderer/         # 渲染核心 (Device, SwapChain, CommandList封装)
│   └── Math/             # 数学库 (矩阵、向量运算，通常依赖 DirectXMath)
│
├── Runtime/              # [业务逻辑代码]
│   ├── DX12_Base.cpp     # 入口文件 (WinMain) 与主循环逻辑
│   └── *.h               # 对应的头文件
│
├── Shaders/              # [图形着色器代码]
│   ├── Source/           # HLSL 源码 (.hlsl)
│   └── Compiled/         # 编译后的着色器对象 (.cso)
│
├── ThirdParty/           # [第三方依赖] (若不使用 vcpkg 则在此手动管理)
│   ├── Include/          # 第三方库头文件
│   └── Lib/              # 第三方库文件 (.lib)
│
├── CMakeLists.txt        # CMake 构建脚本
└── README.md             # 项目说明文档
 