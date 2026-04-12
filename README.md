# 


## 目录结构

DX12_Base/
│
├── Build/                # [构建产物]
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
│   └── Math/             # 数学库 (矩阵、向量运算)
│
├── Runtime/              # [业务逻辑代码]
│   ├── *.cpp             # 入口文件 (WinMain) 与游戏逻辑
│   └── *.h               # 对应的头文件
│
├── Shaders/              # [图形着色器代码]
│   ├── Source/           # HLSL 源码 (.hlsl)
│   └── Compiled/         # 编译后的着色器对象 (.cso)
│
├── ThirdParty/           # [第三方依赖]
│   ├── Include/          # 第三方库头文件 (如 DirectXMath, ImGui)
│   └── Lib/              # 第三方库文件 (.lib)
│
└── *.sln / *.vcxproj     # [VS解决方案与项目配置]