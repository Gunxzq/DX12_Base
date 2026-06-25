# DX12_Base

基于 DirectX 12 的轻量级 3D 渲染引擎框架，采用现代 C++ (C++17) 开发，使用 CMake 进行跨平台构建管理（目前主要支持 Windows）。

## 技术栈

### 核心
| 技术 | 用途 |
|:---|:-----|
| **DirectX 12** | 底层图形 API，高性能 GPU 渲染 |
| **C++17** | 开发语言标准 |
| **CMake** (≥ 3.14) | 跨平台构建系统 |
| **MSVC** (Visual Studio 2022) | 编译器 |

### 第三方库 (via vcpkg)
| 库 | 用途 |
|:---|:-----|
| **Dear ImGui** | 即时模式 GUI，调试界面与编辑器工具 |
| **EnTT** | ECS (Entity-Component-System) 框架，游戏对象管理 |
| **assimp** | 3D 模型加载 (.obj, .gltf, .fbx 等) |
| **spdlog** | 高性能日志系统 |
| **nlohmann/json** | JSON 配置解析 |
| **concurrentqueue** | 无锁并发队列，多线程任务调度 |
| **mimalloc** | 高性能内存分配器 (全局替换) |
| **Jolt Physics** | 物理引擎，刚体碰撞与关节模拟 |
| **Google Test** | 单元测试框架 (可选) |

### 引擎核心模块
| 模块 | 说明 |
|:---|:-----|
| **Renderer** | DirectX 12 渲染管线：RHI 抽象层、Opaque/Shadow/Sky/Water/Terrain/Billboard 渲染器 |
| **Resource** | GPU 资源管理：描述符堆、纹理、几何体、材质、上传缓冲 |
| **ECS** | 基于 EnTT 的实体组件系统 |
| **Event** | 多级优先级的消息分发系统 (P0-P4) |
| **Async** | 后台异步任务执行器 |
| **Network** | 网络传输层抽象 (GNS / P2P) |
| **Platform** | 平台抽象：输入系统、窗口管理 |
| **Math** | 数学工具：包围体、射线相交、哈希 |
| **Logger** | 多端日志输出 (调试输出 / 日志窗口 / 空) |
| **Boot** | 启动引导与依赖注入 (GameContext) |

## 目录结构

```
DX12_Base/
├── Engine/                        # 核心引擎代码
│   ├── Async/                     # 后台异步任务
│   ├── Boot/                      # 启动引导、配置、GameContext
│   ├── Common/                    # 公共工具 (d3dUtil, MathHelper, d3dx12)
│   ├── DebugUI/                   # ImGui 调试面板
│   ├── ECS/                       # 实体组件系统
│   ├── Event/                     # 事件/消息系统
│   ├── Framework/                 # 系统注册与构建
│   ├── Logger/                    # 日志系统
│   ├── Math/                      # 数学库
│   ├── Network/                   # 网络传输
│   ├── Platform/                  # 输入、窗口
│   ├── Renderer/                  # 渲染管线 (Pipeline / RHI / Scene / FrameResources)
│   └── Resource/                  # GPU 资源管理 (Descriptor / Geometry / Material / Texture)
│
├── Runtime/                       # 业务逻辑与入口
│   ├── Application/               # 应用程序入口
│   └── Game/                      # 游戏逻辑 (Game, GameWorld, Input)
│
├── Shaders/                       # HLSL 着色器
├── Config/                        # JSON 配置文件
├── Content/                       # 运行时资源 (模型、纹理)
├── Tests/                         # Google Test 单元测试
│
├── CMakeLists.txt
├── CMakeSettings.json
└── README.md
```

## 环境要求

- **系统**: Windows 10/11 (64-bit)
- **编译器**: MSVC (Visual Studio 2022)
- **构建工具**: CMake (≥ 3.14)
- **包管理器**: vcpkg

## 快速开始

```powershell
# 1. 安装依赖 (vcpkg)
# 修改为你的 vcpkg 路径
cd D:\Dev\vcpkg
.\vcpkg install nlohmann-json:x64-windows concurrentqueue:x64-windows ^
    entt:x64-windows spdlog:x64-windows assimp:x64-windows ^
    mimalloc:x64-windows joltphysics:x64-windows ^
    imgui[dx12-binding]:x64-windows gtest:x64-windows

# 2. 配置 CMake (项目根目录)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=D:/Dev/vcpkg/scripts/buildsystems/vcpkg.cmake

# 3. 构建
cmake --build build

# 4. 运行
./build/Bin/x64/Debug/DX12_Base.exe
```
